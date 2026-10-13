/**
 * @file src/input_boost_policy.h
 * @brief Portable state machine for the encode cadence boost that follows client input.
 */
#pragma once

// standard includes
#include <chrono>
#include <optional>

namespace video::input_boost {
  using clock = std::chrono::steady_clock;

  /**
   * @brief Content that updates more often than this is never treated as a static screen,
   *        whatever minimum_fps_target is set to.
   */
  constexpr std::chrono::milliseconds STATIC_SCREEN_FLOOR {50};

  /**
   * @brief Resolved boost parameters for one encode session.
   * @details A zero boost_interval means the boost is off.
   */
  struct settings_t {
    clock::duration boost_interval {};  ///< Duplicate-frame interval while boosted.
    clock::duration window {};  ///< How long the boost lasts after the last qualifying input.
    clock::duration static_after {};  ///< Time without a captured frame after which the screen counts as static.
  };

  /**
   * @brief Resolve the boost parameters from configuration and the session's cadence.
   * @param enabled config::video.input_activity_boost.
   * @param boost_fps config::video.input_activity_boost_fps; 0 follows the stream framerate.
   * @param window_ms config::video.input_activity_boost_window_ms.
   * @param stream_interval The negotiated stream frame interval. The boost never encodes faster.
   * @param idle_interval The minimum_fps_target duplicate interval the boost replaces.
   * @return The settings; off when disabled, or when the boost would not be faster than idle.
   */
  settings_t make_settings(
    bool enabled,
    int boost_fps,
    int window_ms,
    clock::duration stream_interval,
    clock::duration idle_interval
  );

  /**
   * @brief Tracks whether the encode loop should currently run at the boosted cadence.
   * @details Input only arms the boost while the screen is static, i.e. no frame has
   *          been captured for settings_t::static_after. Input arriving while frames
   *          are flowing neither arms nor extends it, so continuous input during
   *          gameplay cannot keep duplicates interleaved with live frames; the boost
   *          runs out at most settings_t::window after the screen came back to life.
   */
  class state_t {
  public:
    explicit state_t(settings_t settings);

    [[nodiscard]] bool enabled() const;

    /**
     * @brief Record a live (non-placeholder) captured frame.
     */
    void on_frame(clock::time_point at);

    /**
     * @brief Record qualifying client input.
     * @return true if the input armed or extended the boost.
     */
    bool on_input(clock::time_point at);

    [[nodiscard]] bool active(clock::time_point now) const;

    /**
     * @brief The deadline for an image wait that began at wait_started.
     * @details While boosted, the wait is cut short to one boost interval after it
     *          began (which is also after the previous encode), so input arriving
     *          during a long idle wait stops it without ever encoding faster than
     *          the boosted cadence.
     */
    [[nodiscard]] clock::time_point wait_deadline(
      clock::time_point wait_started,
      clock::time_point idle_deadline,
      clock::time_point now
    ) const;

    [[nodiscard]] const settings_t &settings() const;

  private:
    settings_t settings_;
    std::optional<clock::time_point> last_frame_;
    std::optional<clock::time_point> boost_until_;
  };
}  // namespace video::input_boost
