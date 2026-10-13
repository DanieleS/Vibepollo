/**
 * @file src/video_fec.cpp
 * @brief Dynamic Reed-Solomon video FEC driven by client loss summaries.
 */
// standard includes
#include <algorithm>
#include <charconv>
#include <cmath>

// local includes
#include "video_fec.h"

namespace video_fec {
  namespace {
    constexpr std::size_t max_rs_shards = 255;  // DATA_SHARDS_MAX in rswrapper.h
    constexpr std::size_t max_blocks = 4;  // Two-bit FEC block count on the wire.
    constexpr unsigned max_wire_percentage = 255;  // Eight-bit percentage in fecInfo.
    constexpr std::uint32_t max_feedback_data_packets = 1000000;
    constexpr std::uint32_t sequence_half_range = 0x80000000U;
    // Independent-loss model target for losing any block of a frame. Not a measured guarantee.
    constexpr double frame_failure_target = 0.001;
  }  // namespace

  std::optional<int> parse_preference(std::string_view text) noexcept {
    if (text.empty()) {
      return std::nullopt;
    }
    int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size() ||
        value < host_preference || value > static_cast<int>(max_client_percentage)) {
      return std::nullopt;
    }
    return value;
  }

  layout_t layout(std::size_t data_shards, unsigned percentage, std::size_t min_parity) {
    layout_t result;
    if (!data_shards) {
      return result;
    }

    // Same arithmetic as stream.cpp: D = 255 * 100 / (100 + F) data shards fit one block.
    const auto max_data_per_block = (max_rs_shards * 100) / (100 + percentage);
    result.blocks = (data_shards + max_data_per_block - 1) / max_data_per_block;
    if (result.blocks > max_blocks) {
      result.blocks = max_blocks;
      result.skipped = true;
      percentage = 0;
    }

    const auto per_block = (data_shards + result.blocks - 1) / result.blocks;
    std::size_t remaining = data_shards;
    for (std::size_t i = 0; i < result.blocks; ++i) {
      const auto data = i + 1 == result.blocks ? remaining : std::min(per_block, remaining);
      remaining -= data;
      auto parity = (data * percentage + 99) / 100;
      if (percentage && parity < min_parity) {
        parity = min_parity;
      }
      result.data[i] = data;
      result.parity[i] = parity;
    }
    return result;
  }

  double block_failure(std::size_t shards, std::size_t parity, double loss) {
    if (parity >= shards || loss <= 0) {
      return 0;
    }
    if (loss >= 1) {
      return 1;
    }
    // This runs per frame, so walk the binomial terms by their ratio instead of taking a
    // log-gamma per term. Sum whichever side starts nearer the mean, so that the starting
    // term cannot underflow while the sum still matters.
    const auto n = static_cast<double>(shards);
    const auto odds = loss / (1 - loss);
    const auto binomial = [&](double k) {
      return std::exp(std::lgamma(n + 1) - std::lgamma(k + 1) - std::lgamma(n - k + 1) +
                      k * std::log(loss) + (n - k) * std::log1p(-loss));
    };
    if (static_cast<double>(parity) < n * loss) {
      // P(X > parity) = 1 - P(X <= parity), summed downwards from P(X = parity).
      auto term = binomial(static_cast<double>(parity));
      double kept = 0;
      for (auto k = parity + 1; k-- > 0;) {
        kept += term;
        term *= static_cast<double>(k) / (n - static_cast<double>(k) + 1) / odds;
      }
      return std::clamp(1 - kept, 0.0, 1.0);
    }

    // Sum the tail upwards from P(X = parity + 1).
    auto term = binomial(static_cast<double>(parity + 1));
    double failure = 0;
    for (auto k = parity + 1; k <= shards; ++k) {
      failure += term;
      const auto kd = static_cast<double>(k);
      term *= (n - kd) / (kd + 1) * odds;
      // Past the mode the terms only shrink; stop once they no longer matter.
      if (term < failure * 1e-12 && kd > n * loss) {
        break;
      }
    }
    return std::min(failure, 1.0);
  }

  unsigned select_percentage(double loss, std::size_t data_shards, std::size_t min_parity, unsigned maximum) {
    if (loss <= 0 || !data_shards) {
      return 0;
    }
    unsigned best = 0;
    for (unsigned percentage = 1; percentage <= std::min(maximum, max_wire_percentage); ++percentage) {
      const auto frame = layout(data_shards, percentage, min_parity);
      if (frame.skipped) {
        continue;
      }
      best = percentage;
      bool sufficient = true;
      for (std::size_t i = 0; i < frame.blocks; ++i) {
        // Union bound across the frame's blocks.
        if (block_failure(frame.data[i] + frame.parity[i], frame.parity[i], loss) > frame_failure_target / frame.blocks) {
          sufficient = false;
          break;
        }
      }
      if (sufficient) {
        return percentage;
      }
    }
    // The cap is not a guarantee. Keep the most protection that still fits the RS blocks
    // rather than a percentage that would turn FEC off for this frame.
    return best;
  }

  void loss_window_t::record(std::uint32_t data, std::uint32_t missing, std::int64_t now_ms) {
    const auto second = now_ms / 1000;
    auto &bucket = buckets_[static_cast<std::size_t>(second) % buckets_.size()];
    if (bucket.second != second) {
      bucket = {second, 0, 0};
    }
    bucket.data += data;
    bucket.missing += missing;
  }

  std::optional<double> loss_window_t::estimate(std::int64_t now_ms) const {
    if (now_ms < 0) {
      return std::nullopt;
    }
    const auto second = now_ms / 1000;
    std::optional<double> result;
    for (const auto &bucket : buckets_) {
      if (bucket.data && bucket.second >= 0 && bucket.second <= second &&
          second - bucket.second < static_cast<std::int64_t>(buckets_.size())) {
        result = std::max(result.value_or(0), static_cast<double>(bucket.missing) / static_cast<double>(bucket.data));
      }
    }
    return result;
  }

  controller_t::controller_t(unsigned fallback, std::optional<unsigned> fixed):
      fallback_ {std::min(fallback, max_wire_percentage)},
      fixed_ {fixed ? std::optional<unsigned> {std::min(*fixed, max_client_percentage)} : std::nullopt} {
  }

  bool controller_t::report(std::uint32_t sequence, std::uint32_t data, std::uint32_t missing, std::int64_t now_ms) {
    if (!data || data > max_feedback_data_packets || missing > data || now_ms < 0) {
      return false;
    }
    std::lock_guard lock {mutex_};
    const auto delta = sequence - last_sequence_;
    if (last_report_ms_ >= 0 && (now_ms < last_report_ms_ || delta == 0 || delta >= sequence_half_range)) {
      return false;
    }
    if (last_report_ms_ >= 0 && (delta != 1 || now_ms - last_report_ms_ > feedback_timeout_ms)) {
      // A skipped summary or a silent stretch is unknown, not clean: start a new observation period.
      resume_since_ms_ = now_ms;
      loss_window_ = {};
    }
    loss_window_.record(data, missing, now_ms);
    last_sequence_ = sequence;
    last_report_ms_ = now_ms;
    return true;
  }

  unsigned controller_t::percentage(std::size_t data_shards, std::size_t min_parity, std::int64_t now_ms) const {
    if (fixed_) {
      return *fixed_;
    }
    double loss;
    {
      std::lock_guard lock {mutex_};
      const bool fresh = last_report_ms_ >= 0 && now_ms >= last_report_ms_ &&
                         now_ms - last_report_ms_ <= feedback_timeout_ms;
      if (!fresh) {
        return fallback_;
      }
      const auto estimate = loss_window_.estimate(now_ms);
      // After a gap, a clean report must cover a full window before protection is released.
      // A connection's first reports are not a gap.
      if (!estimate || (*estimate == 0 && resume_since_ms_ >= 0 && now_ms - resume_since_ms_ < loss_window_t::duration_ms)) {
        return fallback_;
      }
      loss = *estimate;
    }
    return select_percentage(loss, data_shards, min_parity, automatic_maximum);
  }

  std::optional<double> controller_t::estimate(std::int64_t now_ms) const {
    std::lock_guard lock {mutex_};
    return loss_window_.estimate(now_ms);
  }

  budget_t::budget_t(int video_kbps, unsigned percentage):
      video_kbps_ {std::max(video_kbps, 1)},
      allowance_ {std::min(percentage, max_wire_percentage)} {
  }

  int budget_t::encoder_kbps_for(int video_kbps, unsigned percentage) {
    return std::max(1, static_cast<int>(static_cast<std::int64_t>(video_kbps) * 100 / (100 + percentage)));
  }

  int budget_t::encoder_kbps() const {
    std::lock_guard lock {mutex_};
    return encoder_kbps_for(video_kbps_, allowance_);
  }

  unsigned budget_t::allowance() const {
    std::lock_guard lock {mutex_};
    return allowance_;
  }

  int budget_t::video_kbps() const {
    std::lock_guard lock {mutex_};
    return video_kbps_;
  }

  std::optional<budget_t::change_t> budget_t::observe(unsigned requested, std::int64_t now_ms) {
    std::lock_guard lock {mutex_};
    if (window_start_ms_ < 0 || now_ms < window_start_ms_) {
      window_start_ms_ = now_ms;
      window_peak_ = requested;
      return std::nullopt;
    }
    if (now_ms - window_start_ms_ < window_ms) {
      window_peak_ = std::max(window_peak_, requested);
      return std::nullopt;
    }

    const auto target = std::min(window_peak_, max_wire_percentage);
    window_start_ms_ = now_ms;
    window_peak_ = requested;

    // Ignore one-point wobble, but always allow turning protection on or off.
    const auto difference = target > allowance_ ? target - allowance_ : allowance_ - target;
    if (target == allowance_ || (difference < 2 && target && allowance_)) {
      return std::nullopt;
    }
    const auto kbps = encoder_kbps_for(video_kbps_, target);
    if (requested_kbps_ == kbps) {
      return std::nullopt;  // Still waiting on, or refused, this exact change.
    }
    requested_kbps_ = kbps;
    return change_t {target, kbps};
  }

  void budget_t::applied(int encoder_kbps) {
    if (encoder_kbps <= 0) {
      return;
    }
    std::lock_guard lock {mutex_};
    const auto room = static_cast<std::int64_t>(video_kbps_) * 100 / encoder_kbps - 100;
    allowance_ = static_cast<unsigned>(std::clamp<std::int64_t>(room, 0, max_wire_percentage));
    requested_kbps_.reset();
  }

  int budget_t::set_video_kbps(int video_kbps) {
    std::lock_guard lock {mutex_};
    video_kbps_ = std::max(video_kbps, 1);
    requested_kbps_.reset();
    return encoder_kbps_for(video_kbps_, allowance_);
  }
}  // namespace video_fec
