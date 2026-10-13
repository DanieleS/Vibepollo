/**
 * @file tests/unit/test_nvenc_frame_budget.cpp
 * @brief Test the NVENC frame budget guard and the defaults it protects.
 */
#include <gtest/gtest.h>

#include <tuple>

#include "src/nvenc/frame_budget.h"
#include "src/nvenc/nvenc_config.h"

namespace {

  constexpr int k1080pWidth = 1920;
  constexpr int k1080pHeight = 1080;
  constexpr int k4kWidth = 3840;
  constexpr int k4kHeight = 2160;

  nvenc::frame_budget_session session(int width, int height, double fps, int video_format = 1, int num_engines = 1, nvenc::split_frame_budget split = nvenc::split_frame_budget::unavailable) {
    nvenc::frame_budget_session s;
    s.width = width;
    s.height = height;
    s.fps = fps;
    s.video_format = video_format;
    s.num_engines = num_engines;
    s.split = split;
    return s;
  }

  nvenc::frame_budget_session implicit_split(int width, int height, double fps, int video_format = 1, int num_engines = 2) {
    return session(width, height, fps, video_format, num_engines, nvenc::split_frame_budget::implicit);
  }

}  // namespace

TEST(NvencFrameBudgetTest, DefaultsAreP4WithSpatialAqAndTheGuardOn) {
  const nvenc::nvenc_config config;
  EXPECT_EQ(config.quality_preset, 4);
  EXPECT_TRUE(config.adaptive_quantization);
  EXPECT_TRUE(config.frame_budget_guard);
}

TEST(NvencFrameBudgetTest, P4FitsTheFrameBudgetAt1080p120) {
  const auto verdict = nvenc::evaluate_frame_budget(4, session(k1080pWidth, k1080pHeight, 120.0), true);
  EXPECT_FALSE(verdict.clamped);
  EXPECT_FALSE(verdict.budget_exceeded_at_floor);
  EXPECT_EQ(verdict.effective_preset, 4);
  EXPECT_NEAR(verdict.budget_ms, 1000.0 / 120.0 * 0.65, 1e-9);
  EXPECT_LT(verdict.estimated_ms, verdict.budget_ms);
}

TEST(NvencFrameBudgetTest, SlowestPresetStillFitsAt1080p120) {
  const auto verdict = nvenc::evaluate_frame_budget(7, session(k1080pWidth, k1080pHeight, 120.0), true);
  EXPECT_FALSE(verdict.clamped);
  EXPECT_EQ(verdict.effective_preset, 7);
}

TEST(NvencFrameBudgetTest, FourKAt60KeepsP4) {
  const auto verdict = nvenc::evaluate_frame_budget(4, session(k4kWidth, k4kHeight, 60.0), true);
  EXPECT_FALSE(verdict.clamped);
  EXPECT_EQ(verdict.effective_preset, 4);
}

TEST(NvencFrameBudgetTest, FourKAt90StepsDownToP2) {
  const auto verdict = nvenc::evaluate_frame_budget(4, session(k4kWidth, k4kHeight, 90.0), true);
  EXPECT_TRUE(verdict.clamped);
  EXPECT_EQ(verdict.configured_preset, 4);
  EXPECT_EQ(verdict.effective_preset, 2);
  EXPECT_FALSE(verdict.budget_exceeded_at_floor);
  EXPECT_LE(verdict.estimated_ms, verdict.budget_ms);
  EXPECT_GT(verdict.configured_estimated_ms, verdict.budget_ms);
}

TEST(NvencFrameBudgetTest, FourKAt120OnOneEngineFallsToP1) {
  const auto verdict = nvenc::evaluate_frame_budget(4, session(k4kWidth, k4kHeight, 120.0), true);
  EXPECT_TRUE(verdict.clamped);
  EXPECT_EQ(verdict.effective_preset, 1);
  // P1 is the floor: it is used even though it is estimated not to fit.
  EXPECT_TRUE(verdict.budget_exceeded_at_floor);
}

TEST(NvencFrameBudgetTest, ForcedSplitFrameCountsEveryEngineAtEveryPreset) {
  const auto forced = [](int width, int height) {
    return session(width, height, 120.0, 2, 2, nvenc::split_frame_budget::forced);
  };
  const auto p4 = nvenc::evaluate_frame_budget(4, forced(k4kWidth, k4kHeight), true);
  EXPECT_FALSE(p4.clamped);
  EXPECT_EQ(p4.effective_preset, 4);
  EXPECT_EQ(p4.num_engines, 2);

  const auto p7 = nvenc::evaluate_frame_budget(7, forced(k4kWidth, k4kHeight), true);
  EXPECT_TRUE(p7.clamped);
  EXPECT_EQ(p7.effective_preset, 5);
  EXPECT_EQ(p7.num_engines, 2);

  EXPECT_EQ(nvenc::split_frame_engines(forced(2560, 1440), 7), 2);
}

TEST(NvencFrameBudgetTest, ImplicitSplitFrameCountsEnginesOnlyWhereTheDriverSplits) {
  // NVIDIA's programming guide: P1-P4 with low-latency tuning, HEVC >= 2112 or AV1 >= 2048 lines.
  EXPECT_EQ(nvenc::split_frame_engines(implicit_split(k4kWidth, k4kHeight, 120.0), 4), 2);
  EXPECT_EQ(nvenc::split_frame_engines(implicit_split(k4kWidth, k4kHeight, 120.0), 5), 1);
  EXPECT_EQ(nvenc::split_frame_engines(implicit_split(3840, 2111, 120.0), 1), 1);
  EXPECT_EQ(nvenc::split_frame_engines(implicit_split(3840, 2112, 120.0), 1), 2);
  EXPECT_EQ(nvenc::split_frame_engines(implicit_split(3840, 2048, 120.0, 2), 1), 2);
  EXPECT_EQ(nvenc::split_frame_engines(implicit_split(3840, 2047, 120.0, 2), 1), 1);
  EXPECT_EQ(nvenc::split_frame_engines(implicit_split(2560, 1440, 240.0), 1), 1);
  // Unavailable split (H.264, old API, weighted prediction, disabled) never counts extra engines.
  EXPECT_EQ(nvenc::split_frame_engines(session(k4kWidth, k4kHeight, 120.0, 1, 2), 1), 1);
  EXPECT_EQ(nvenc::split_frame_engines(implicit_split(k4kWidth, k4kHeight, 120.0, 0), 1), 1);
}

TEST(NvencFrameBudgetTest, FourKAt120KeepsP4WhereTheDriverSplitsImplicitly) {
  const auto verdict = nvenc::evaluate_frame_budget(4, implicit_split(k4kWidth, k4kHeight, 120.0), true);
  EXPECT_FALSE(verdict.clamped);
  EXPECT_EQ(verdict.effective_preset, 4);
  EXPECT_EQ(verdict.num_engines, 2);
}

TEST(NvencFrameBudgetTest, ImplicitSplitDoesNotHelp1440pOnTwoEngines) {
  // 1440p is below the implicit split height, so both engines count as one.
  const auto qhd240 = nvenc::evaluate_frame_budget(4, implicit_split(2560, 1440, 240.0), true);
  EXPECT_TRUE(qhd240.clamped);
  EXPECT_EQ(qhd240.effective_preset, 1);
  EXPECT_EQ(qhd240.num_engines, 1);

  const auto ultrawide165 = nvenc::evaluate_frame_budget(4, implicit_split(3440, 1440, 165.0), true);
  EXPECT_TRUE(ultrawide165.clamped);
  EXPECT_EQ(ultrawide165.effective_preset, 1);
}

TEST(NvencFrameBudgetTest, ImplicitSplitDoesNotHelpP7At4K60) {
  // The driver doesn't split P5-P7 in implicit mode: P7 is estimated on one engine.
  const auto verdict = nvenc::evaluate_frame_budget(7, implicit_split(k4kWidth, k4kHeight, 60.0), true);
  EXPECT_TRUE(verdict.clamped);
  EXPECT_EQ(verdict.effective_preset, 6);
  EXPECT_EQ(verdict.num_engines, 1);
}

TEST(NvencFrameBudgetTest, Yuv444CostsTwiceTheSamples) {
  auto qhd120 = session(2560, 1440, 120.0);
  EXPECT_EQ(nvenc::evaluate_frame_budget(4, qhd120, true).effective_preset, 4);
  qhd120.yuv444 = true;
  const auto verdict = nvenc::evaluate_frame_budget(4, qhd120, true);
  EXPECT_TRUE(verdict.clamped);
  EXPECT_EQ(verdict.effective_preset, 1);

  auto fhd120 = session(k1080pWidth, k1080pHeight, 120.0);
  fhd120.yuv444 = true;
  EXPECT_EQ(nvenc::evaluate_frame_budget(4, fhd120, true).effective_preset, 4);
}

TEST(NvencFrameBudgetTest, DisabledGuardKeepsTheConfiguredPresetButReportsTheBudget) {
  const auto verdict = nvenc::evaluate_frame_budget(4, session(k4kWidth, k4kHeight, 120.0), false);
  EXPECT_FALSE(verdict.clamped);
  EXPECT_FALSE(verdict.budget_exceeded_at_floor);
  EXPECT_EQ(verdict.effective_preset, 4);
  EXPECT_GT(verdict.budget_ms, 0.0);
  EXPECT_DOUBLE_EQ(verdict.estimated_ms, verdict.configured_estimated_ms);
  EXPECT_GT(verdict.estimated_ms, verdict.budget_ms);
}

TEST(NvencFrameBudgetTest, MissingSessionInfoKeepsTheConfiguredPreset) {
  for (const auto &[width, height, fps] : {std::tuple {0, k4kHeight, 120.0}, std::tuple {k4kWidth, 0, 120.0}, std::tuple {k4kWidth, k4kHeight, 0.0}}) {
    const auto verdict = nvenc::evaluate_frame_budget(7, session(width, height, fps), true);
    EXPECT_FALSE(verdict.clamped);
    EXPECT_EQ(verdict.effective_preset, 7);
    EXPECT_EQ(verdict.budget_ms, 0.0);
  }
}

TEST(NvencFrameBudgetTest, OutOfRangeInputsAreClamped) {
  EXPECT_EQ(nvenc::evaluate_frame_budget(9, session(k1080pWidth, k1080pHeight, 60.0), true).effective_preset, 7);
  EXPECT_EQ(nvenc::evaluate_frame_budget(0, session(k1080pWidth, k1080pHeight, 60.0), true).effective_preset, 1);
  EXPECT_EQ(nvenc::evaluate_frame_budget(4, session(k1080pWidth, k1080pHeight, 60.0, 1, 0, nvenc::split_frame_budget::forced), true).num_engines, 1);
}
