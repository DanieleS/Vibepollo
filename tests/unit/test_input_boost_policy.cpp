/**
 * @file tests/unit/test_input_boost_policy.cpp
 * @brief Tests for the encode cadence boost that follows client input.
 */
#include "../tests_common.h"

#include <src/input_boost_policy.h>

#include <algorithm>
#include <vector>

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

  state.on_frame(t0);
  // Frames are flowing: input during gameplay does nothing.
  EXPECT_FALSE(state.on_input(t0 + 20ms));
  state.on_frame(t0 + 30ms);
  EXPECT_FALSE(state.active(t0 + 40ms));

  // The screen has been static for the idle interval.
  EXPECT_TRUE(state.on_input(t0 + 80ms));
}

TEST(InputBoost, WaitsForTheResponseBeforeSendingDuplicates) {
  auto state = default_state();
  state.on_frame(t0);
  ASSERT_TRUE(state.on_input(t0 + 100ms));

  // Armed, but a copy of the stale pre-input image would show nothing new: the
  // idle wait in progress is left alone.
  EXPECT_FALSE(state.active(t0 + 100ms));
  EXPECT_FALSE(state.active(t0 + 130ms));

  // The response frame arrives; duplicates now follow it at the boosted cadence.
  state.on_frame(t0 + 135ms);
  EXPECT_TRUE(state.active(t0 + 135ms));
  EXPECT_TRUE(state.active(t0 + 249ms));
  // The window counts from the input, not from the response.
  EXPECT_FALSE(state.active(t0 + 250ms));
}

TEST(InputBoost, InputThatChangesNothingSendsNothingExtra) {
  auto state = default_state();
  state.on_frame(t0);
  // A held stick on a screen that ignores it: every packet re-arms, nothing responds.
  for (auto at = t0 + 100ms; at < t0 + 2s; at += 30ms) {
    EXPECT_TRUE(state.on_input(at));
    EXPECT_FALSE(state.active(at + 1ms));
  }
}

TEST(InputBoost, FramesCapturedBeforeTheInputAreNotItsResponse) {
  auto state = default_state();
  state.on_frame(t0);
  ASSERT_TRUE(state.on_input(t0 + 100ms));

  // Captured just before the input reached the OS, popped just after it.
  state.on_frame(t0 + 98ms);
  EXPECT_FALSE(state.active(t0 + 101ms));

  // The actual response, soon after that stale frame, still gets its followers.
  state.on_frame(t0 + 120ms);
  EXPECT_TRUE(state.active(t0 + 121ms));
}

TEST(InputBoost, EndsOnceTheScreenIsLiveAgain) {
  auto state = default_state();
  state.on_frame(t0);
  ASSERT_TRUE(state.on_input(t0 + 100ms));

  // The input starts a 60 fps animation: one follower at most fits before its
  // second frame, which ends the boost instead of doubling the animation's frames.
  state.on_frame(t0 + 110ms);
  EXPECT_TRUE(state.active(t0 + 111ms));
  state.on_frame(t0 + 127ms);
  EXPECT_FALSE(state.active(t0 + 128ms));

  // Held input keeps arriving with every frame; it cannot restart the boost.
  for (auto at = t0 + 143ms; at < t0 + 400ms; at += 16ms) {
    EXPECT_FALSE(state.on_input(at - 1ms));
    state.on_frame(at);
    EXPECT_FALSE(state.active(at + 1ms));
  }
}

TEST(InputBoost, SparseResponsesEachGetFollowers) {
  auto state = default_state();
  state.on_frame(t0);
  ASSERT_TRUE(state.on_input(t0 + 100ms));
  state.on_frame(t0 + 120ms);
  ASSERT_TRUE(state.active(t0 + 121ms));

  // A second, slower change of the same response still counts as static content.
  state.on_frame(t0 + 180ms);
  EXPECT_TRUE(state.active(t0 + 181ms));
}

TEST(InputBoost, NewInputWaitsForItsOwnResponse) {
  auto state = default_state();
  state.on_frame(t0);
  ASSERT_TRUE(state.on_input(t0 + 100ms));
  state.on_frame(t0 + 120ms);
  ASSERT_TRUE(state.active(t0 + 121ms));

  // The next press on the again-static screen pauses the followers until its response.
  EXPECT_TRUE(state.on_input(t0 + 200ms));
  EXPECT_FALSE(state.active(t0 + 201ms));
  state.on_frame(t0 + 215ms);
  EXPECT_TRUE(state.active(t0 + 349ms));
  EXPECT_FALSE(state.active(t0 + 350ms));

  // An older event never re-arms.
  EXPECT_FALSE(state.on_input(t0 + 150ms));
  EXPECT_TRUE(state.active(t0 + 300ms));
}

TEST(InputBoost, InputOlderThanTheLastFrameIsIgnored) {
  auto state = default_state();
  state.on_frame(t0 + 100ms);
  EXPECT_FALSE(state.on_input(t0));
}

TEST(InputBoost, DisabledStateNeverBoosts) {
  state_t state {settings_t {}};
  state.on_frame(t0);
  EXPECT_FALSE(state.on_input(t0 + 1s));
  state.on_frame(t0 + 1s + 10ms);
  EXPECT_FALSE(state.active(t0 + 1s + 11ms));
}

namespace {
  struct encode_t {
    clock::time_point at;
    bool duplicate;
  };

  // Mirrors encode_run's use of the state: pending input is recorded at the top of
  // each iteration and before each live frame, the image wait starts after the
  // previous encode and lasts boost_interval while active (idle_20 otherwise), and a
  // wait that ends without an image encodes a duplicate.
  std::vector<encode_t> simulate(
    state_t &state,
    const std::vector<clock::time_point> &frames,
    const clock::time_point input_at,
    const clock::time_point end
  ) {
    constexpr auto encode_time = std::chrono::duration_cast<clock::duration>(2ms);
    std::vector<encode_t> encodes;
    bool input_pending = true;
    const auto consume = [&](const clock::time_point now) {
      if (input_pending && input_at <= now) {
        input_pending = false;
        state.on_input(input_at);
      }
    };

    auto next_frame = frames.begin();
    auto now = *next_frame++;
    state.on_frame(now);
    encodes.push_back({now, false});
    now += encode_time;
    while (now < end) {
      consume(now);
      const auto deadline = now + (state.active(now) ? state.settings().boost_interval : idle_20);
      if (next_frame != frames.end() && *next_frame <= deadline) {
        now = std::max(now, *next_frame++);
        consume(now);
        state.on_frame(now);
        encodes.push_back({now, false});
      } else {
        now = deadline;
        encodes.push_back({now, true});
      }
      now += encode_time;
    }
    return encodes;
  }
}  // namespace

TEST(InputBoostEncodeLoop, FollowsTheResponseAtTheStreamCadenceThenFallsBackToIdle) {
  auto state = default_state();
  const auto input_at = t0 + 100ms;
  const auto response_at = t0 + 130ms;
  const auto encodes = simulate(state, {t0, response_at}, input_at, t0 + 600ms);

  int boosted = 0;
  for (std::size_t i = 1; i < encodes.size(); ++i) {
    const auto &encode = encodes[i];
    const auto gap = encode.at - encodes[i - 1].at;
    if (!encode.duplicate) {
      continue;
    }
    // Never faster than the negotiated stream rate.
    EXPECT_GE(gap, stream_120);
    if (gap < idle_20) {
      // Boosted waits only start between the response and the end of the window.
      EXPECT_GE(encodes[i - 1].at, response_at);
      EXPECT_LT(encodes[i - 1].at, input_at + 150ms);
      ++boosted;
    }
  }
  // About (input + window - response) / (boost interval + encode time) followers.
  EXPECT_GE(boosted, 10);
  EXPECT_LE(boosted, 12);
}

TEST(InputBoostEncodeLoop, AnimationStartedByTheInputGetsAtMostOneFollower) {
  auto state = default_state();
  std::vector<clock::time_point> frames {t0};
  for (auto at = t0 + 130ms; at < t0 + 600ms; at += 16ms) {
    frames.push_back(at);
  }
  const auto encodes = simulate(state, frames, t0 + 100ms, t0 + 600ms);

  int interleaved = 0;
  for (std::size_t i = 1; i < encodes.size(); ++i) {
    if (encodes[i].duplicate && encodes[i].at - encodes[i - 1].at < idle_20) {
      ++interleaved;
    }
  }
  EXPECT_LE(interleaved, 1);
}

TEST(InputBoostEncodeLoop, InputWithoutAResponseKeepsTheIdleCadence) {
  auto state = default_state();
  const auto encodes = simulate(state, {t0}, t0 + 100ms, t0 + 600ms);
  for (std::size_t i = 1; i < encodes.size(); ++i) {
    EXPECT_GE(encodes[i].at - encodes[i - 1].at, idle_20);
  }
}
