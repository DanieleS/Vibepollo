/**
 * @file src/input_boost_policy.cpp
 * @brief Definitions for the encode cadence boost that follows client input.
 */
#include "input_boost_policy.h"

// standard includes
#include <algorithm>

namespace video::input_boost {
  settings_t make_settings(
    const bool enabled,
    const int boost_fps,
    const int window_ms,
    const clock::duration stream_interval,
    const clock::duration idle_interval
  ) {
    if (!enabled || boost_fps < 0 || window_ms <= 0 || stream_interval <= clock::duration::zero()) {
      return {};
    }

    auto boost_interval = stream_interval;
    if (boost_fps > 0) {
      boost_interval = std::max(
        boost_interval,
        std::chrono::duration_cast<clock::duration>(std::chrono::nanoseconds(std::chrono::seconds(1)) / boost_fps)
      );
    }
    if (boost_interval >= idle_interval) {
      // minimum_fps_target already keeps the stream at least this busy.
      return {};
    }

    return {
      boost_interval,
      std::chrono::duration_cast<clock::duration>(std::chrono::milliseconds(window_ms)),
      std::max(idle_interval, std::chrono::duration_cast<clock::duration>(STATIC_SCREEN_FLOOR)),
    };
  }

  state_t::state_t(settings_t settings):
      settings_ {settings} {
  }

  bool state_t::enabled() const {
    return settings_.boost_interval > clock::duration::zero();
  }

  void state_t::on_frame(const clock::time_point at) {
    if (last_frame_ && at <= *last_frame_) {
      return;
    }
    const auto previous = last_frame_;
    last_frame_ = at;

    if (!boost_until_) {
      return;
    }
    if (at >= *boost_until_) {
      end();
      return;
    }
    if (!following_) {
      // Frames captured before the input reached the OS cannot be its response.
      following_ = at >= *armed_at_;
      return;
    }
    if (previous && at - *previous < settings_.static_after) {
      // The screen is live again: duplicates would only be interleaved with it.
      end();
    }
  }

  bool state_t::on_input(const clock::time_point at) {
    if (!enabled() || !last_frame_) {
      // Before the first live frame the bootstrap logic owns the cadence.
      return false;
    }
    if (at < *last_frame_ || at - *last_frame_ < settings_.static_after) {
      return false;
    }
    if (armed_at_ && at <= *armed_at_) {
      return false;
    }

    // Wait for this input's own response before sending duplicates again.
    armed_at_ = at;
    boost_until_ = at + settings_.window;
    following_ = false;
    return true;
  }

  bool state_t::active(const clock::time_point now) const {
    return enabled() && following_ && boost_until_ && now < *boost_until_;
  }

  void state_t::end() {
    armed_at_.reset();
    boost_until_.reset();
    following_ = false;
  }

  const settings_t &state_t::settings() const {
    return settings_;
  }
}  // namespace video::input_boost
