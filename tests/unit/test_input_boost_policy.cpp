/**
 * @file tests/unit/test_input_boost_policy.cpp
 * @brief Tests for the encode cadence boost that follows client input.
 */
#include "../tests_common.h"

#include <src/input_boost_policy.h>

namespace {
  using namespace std::chrono_literals;
  using video::input_boost::clock;
  using video::input_boost::make_settings;
  using video::input_boost::settings_t;
  using video::input_boost::state_t;

  constexpr auto stream_120 = std::chrono::duration_cast<clock::duration>(std::chrono::nanoseconds(8333333));
  constexpr auto stream_60 = std::chrono::duration_cast<clock::duration>(std::chrono::nanoseconds(16666666));
  constexpr auto idle_20 = std::chrono::duration_cast<clock::duration>(50ms);

  const clock::time_point t0 {std::chrono::seconds(1000)};

  state_t default_state() {
    return state_t {make_settings(true, 0, 150, stream_120, idle_20)};
  }
}  // namespace

TEST(InputBoostSettings, FollowsTheStreamCadenceByDefault) {
  const auto settings = make_settings(true, 0, 150, stream_120, idle_20);
  EXPECT_EQ(settings.boost_interval, stream_120);
  EXPECT_EQ(settings.window, std::chrono::duration_cast<clock::duration>(150ms));
  EXPECT_EQ(settings.static_after, idle_20);
}

TEST(InputBoostSettings, NeverEncodesFasterThanTheStream) {
  EXPECT_EQ(make_settings(true, 1000, 150, stream_60, idle_20).boost_interval, stream_60);
  EXPECT_EQ(
    make_settings(true, 40, 150, stream_120, idle_20).boost_interval,
    std::chrono::duration_cast<clock::duration>(25ms)
  );
}

TEST(InputBoostSettings, IsOffWhenDisabledOrNotFasterThanIdle) {
  EXPECT_FALSE(state_t {make_settings(false, 0, 150, stream_120, idle_20)}.enabled());
  EXPECT_FALSE(state_t {make_settings(true, 0, 0, stream_120, idle_20)}.enabled());
  EXPECT_FALSE(state_t {make_settings(true, -1, 150, stream_120, idle_20)}.enabled());
  EXPECT_FALSE(state_t {make_settings(true, 0, 150, clock::duration::zero(), idle_20)}.enabled());
  // minimum_fps_target already at or above the boosted cadence.
  EXPECT_FALSE(state_t {make_settings(true, 20, 150, stream_120, idle_20)}.enabled());
  EXPECT_FALSE(state_t {make_settings(true, 0, 150, stream_60, stream_60)}.enabled());
  EXPECT_TRUE(default_state().enabled());
}

TEST(InputBoostSettings, StaticScreenThresholdHasAFloor) {
  // A high minimum_fps_target must not make a 30 fps game count as a static screen.
  const auto settings = make_settings(true, 0, 150, stream_120, std::chrono::duration_cast<clock::duration>(16ms));
  EXPECT_EQ(settings.static_after, std::chrono::duration_cast<clock::duration>(video::input_boost::STATIC_SCREEN_FLOOR));
}

TEST(InputBoost, ArmsOnlyOnAStaticScreen) {
  auto state = default_state();
  // Before the first live frame the bootstrap logic owns the cadence.
  EXPECT_FALSE(state.on_input(t0));
  EXPECT_FALSE(state.active(t0));

  state.on_frame(t0);
  // Frames are flowing: input during gameplay does nothing.
  EXPECT_FALSE(state.on_input(t0 + 20ms));
  EXPECT_FALSE(state.active(t0 + 20ms));

  // The screen has been static for the idle interval.
  EXPECT_TRUE(state.on_input(t0 + 80ms));
  EXPECT_TRUE(state.active(t0 + 80ms));
  EXPECT_TRUE(state.active(t0 + 229ms));
  EXPECT_FALSE(state.active(t0 + 230ms));
}

TEST(InputBoost, LiveFramesStopFurtherExtensions) {
  auto state = default_state();
  state.on_frame(t0);
  ASSERT_TRUE(state.on_input(t0 + 100ms));  // boosted until t0 + 250ms

  // The response starts flowing; held input keeps arriving with every frame.
  for (auto at = t0 + 120ms; at < t0 + 400ms; at += 16ms) {
    state.on_frame(at);
    EXPECT_FALSE(state.on_input(at + 1ms));
  }
  EXPECT_TRUE(state.active(t0 + 249ms));
  EXPECT_FALSE(state.active(t0 + 250ms));
}

TEST(InputBoost, InputOnAStillStaticScreenExtendsTheWindow) {
  auto state = default_state();
  state.on_frame(t0);
  ASSERT_TRUE(state.on_input(t0 + 100ms));
  // The first press changed nothing on screen; a second one keeps the boost alive.
  EXPECT_TRUE(state.on_input(t0 + 200ms));
  EXPECT_TRUE(state.active(t0 + 349ms));
  EXPECT_FALSE(state.active(t0 + 350ms));
  // An older event never shortens the window.
  EXPECT_TRUE(state.on_input(t0 + 120ms));
  EXPECT_TRUE(state.active(t0 + 349ms));
}

TEST(InputBoost, InputOlderThanTheLastFrameIsIgnored) {
  auto state = default_state();
  state.on_frame(t0 + 100ms);
  EXPECT_FALSE(state.on_input(t0));
}

TEST(InputBoost, CutsAnIdleWaitShortWithoutExceedingTheBoostedCadence) {
  auto state = default_state();
  state.on_frame(t0);

  const auto wait_started = t0 + 60ms;
  const auto idle_deadline = wait_started + idle_20;
  EXPECT_EQ(state.wait_deadline(wait_started, idle_deadline, wait_started + 5ms), idle_deadline);

  // Input lands 20 ms into the idle wait: the wait ends now, one boosted
  // interval after the previous encode has long passed.
  ASSERT_TRUE(state.on_input(wait_started + 20ms));
  EXPECT_EQ(state.wait_deadline(wait_started, idle_deadline, wait_started + 20ms), wait_started + stream_120);

  // Input landing right after the previous encode still waits a full boosted interval.
  const auto next_wait = wait_started + 21ms;
  EXPECT_EQ(state.wait_deadline(next_wait, next_wait + idle_20, next_wait + 1ms), next_wait + stream_120);

  // A shorter remaining budget (for example an AMF tail flush) is kept.
  EXPECT_EQ(state.wait_deadline(next_wait, next_wait + 2ms, next_wait), next_wait + 2ms);

  // Once the boost lapses the idle cadence returns.
  const auto late = t0 + 500ms;
  EXPECT_EQ(state.wait_deadline(late, late + idle_20, late), late + idle_20);
}

TEST(InputBoost, DisabledStateNeverChangesTheWait) {
  state_t state {settings_t {}};
  state.on_frame(t0);
  EXPECT_FALSE(state.on_input(t0 + 1s));
  EXPECT_FALSE(state.active(t0 + 1s));
  EXPECT_EQ(state.wait_deadline(t0 + 1s, t0 + 1s + idle_20, t0 + 1s), t0 + 1s + idle_20);
}
