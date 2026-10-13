/**
 * @file src/video_fec.h
 * @brief Dynamic Reed-Solomon video FEC driven by client loss summaries.
 *
 * Ported from AlkaidLab/foundation-sunshine (src/streaming/fec.cpp, GPLv3, commits 6470fbed
 * and 46525886) and adapted to this host's FEC block layout and runtime bitrate path.
 *
 * Nothing here runs unless the client negotiates it: the host advertises
 * `x-ss-video[0].fecControlVersion:1` in DESCRIBE, and only a client that answers with
 * `x-ml-video.fecSummaryVersion:1` and a `x-ml-video.fecPercentage` other than "host default"
 * gets a controller. Every other session keeps the fixed `fec_percentage` path unchanged.
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>

namespace video_fec {
  /// Wire values of `x-ml-video.fecPercentage`; 0-100 asks for a fixed percentage.
  constexpr int host_preference = -2;
  constexpr int automatic_preference = -1;
  constexpr unsigned max_client_percentage = 100;

  /// Highest percentage the automatic mode selects.
  constexpr unsigned automatic_maximum = 50;

  /// Client-to-host control message with the client's loss summary for the last second:
  /// three big-endian uint32 (sequence, data shards seen, data shards missing before recovery).
  constexpr std::uint16_t summary_packet_type = 0x5510;
  constexpr std::size_t summary_size = 12;

  /**
   * @brief Parse `x-ml-video.fecPercentage`.
   * @return The preference, or nothing when the text is not an integer in [-2, 100].
   */
  std::optional<int> parse_preference(std::string_view text) noexcept;

  /// Per-block layout of one frame, mirroring the split in stream.cpp's video send path.
  struct layout_t {
    std::size_t blocks = 0;
    std::array<std::size_t, 4> data {};
    std::array<std::size_t, 4> parity {};
    bool skipped = false;  ///< The frame needs more than four blocks, so it is sent without FEC.
  };

  /**
   * @brief Lay out @p data_shards packets at @p percentage the way stream.cpp does.
   * @param min_parity The client's minimum parity per block (applies only when percentage > 0).
   */
  layout_t layout(std::size_t data_shards, unsigned percentage, std::size_t min_parity);

  /// Probability that more than @p parity of @p shards are lost, each independently with @p loss.
  double block_failure(std::size_t shards, std::size_t parity, double loss);

  /**
   * @brief Lowest percentage, up to @p maximum, that keeps a frame's chance of losing any block
   *        under 0.1% when shards are lost independently with probability @p loss.
   *
   * Returns the highest representable percentage within @p maximum when none reaches the target.
   */
  unsigned select_percentage(double loss, std::size_t data_shards, std::size_t min_parity, unsigned maximum);

  /**
   * @brief Ten one-second buckets of client feedback. No observation is distinct from zero loss.
   */
  class loss_window_t {
  public:
    static constexpr std::int64_t duration_ms = 10000;

    void record(std::uint32_t data, std::uint32_t missing, std::int64_t now_ms);

    /// Highest packet-weighted one-second loss rate in the window, or nothing without observations.
    std::optional<double> estimate(std::int64_t now_ms) const;

  private:
    struct bucket_t {
      std::int64_t second = -1;
      std::uint64_t data = 0;
      std::uint64_t missing = 0;
    };

    std::array<bucket_t, duration_ms / 1000> buckets_ {};
  };

  /**
   * @brief Per-session FEC percentage: a client-chosen fixed value, or chosen from the client's
   *        loss summaries, falling back to the host's `fec_percentage` without fresh feedback.
   */
  class controller_t {
  public:
    static constexpr std::int64_t feedback_timeout_ms = 3000;

    /**
     * @param fallback The host's fixed percentage, used while automatic mode lacks feedback.
     * @param fixed The client's fixed percentage, or nothing for automatic mode.
     */
    controller_t(unsigned fallback, std::optional<unsigned> fixed);

    bool automatic() const {
      return !fixed_;
    }

    /**
     * @brief Record one loss summary.
     * @return false when the summary is rejected (empty, inconsistent, duplicate or out of order).
     */
    bool report(std::uint32_t sequence, std::uint32_t data, std::uint32_t missing, std::int64_t now_ms);

    /// The percentage to request for a frame of @p data_shards packets.
    unsigned percentage(std::size_t data_shards, std::size_t min_parity, std::int64_t now_ms) const;

    /// The loss estimate automatic mode would use now, for logging.
    std::optional<double> estimate(std::int64_t now_ms) const;

  private:
    mutable std::mutex mutex_;
    const unsigned fallback_;
    const std::optional<unsigned> fixed_;
    std::uint32_t last_sequence_ = 0;
    std::int64_t last_report_ms_ = -1;
    std::int64_t resume_since_ms_ = -1;
    loss_window_t loss_window_;
  };

  /**
   * @brief Splits the session's video budget between the encoder and FEC parity.
   *
   * The encoder runs at video * 100 / (100 + F), so data plus parity fit the budget. The
   * percentage the encoder is sized for is the allowance; frames never carry more FEC than it.
   * Each one-second window's peak request becomes a new encoder target. Raising protection waits
   * until the encoder has lowered its bitrate; lowering it takes effect at once.
   */
  class budget_t {
  public:
    static constexpr std::int64_t window_ms = 1000;

    struct change_t {
      unsigned percentage;  ///< The window's peak request.
      int encoder_kbps;  ///< Encoder bitrate that leaves room for it.
    };

    budget_t(int video_kbps, unsigned percentage);

    static int encoder_kbps_for(int video_kbps, unsigned percentage);

    /// Encoder bitrate for the current allowance.
    int encoder_kbps() const;

    unsigned allowance() const;

    int video_kbps() const;

    /**
     * @brief Feed the percentage requested for one frame.
     * @return The encoder change to request when a window closes with a different peak.
     */
    std::optional<change_t> observe(unsigned requested, std::int64_t now_ms);

    /// The encoder now runs at @p encoder_kbps: size the allowance to the room it leaves.
    void applied(int encoder_kbps);

    /**
     * @brief Replace the video budget (a runtime bitrate request).
     * @return The encoder bitrate for the new budget at the current allowance.
     */
    int set_video_kbps(int video_kbps);

  private:
    mutable std::mutex mutex_;
    int video_kbps_;
    unsigned allowance_;
    unsigned window_peak_ = 0;
    std::int64_t window_start_ms_ = -1;
    std::optional<int> requested_kbps_;
  };
}  // namespace video_fec
