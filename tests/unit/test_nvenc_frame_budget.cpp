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

}  // namespace

TEST(NvencFrameBudgetTest, DefaultsAreP4WithSpatialAqAndTheGuardOn) {
  const nvenc::nvenc_config config;
  EXPECT_EQ(config.quality_preset, 4);
  EXPECT_TRUE(config.adaptive_quantization);
  EXPECT_TRUE(config.frame_budget_guard);
}

TEST(NvencFrameBudgetTest, P4FitsTheFrameBudgetAt1080p120) {
  const auto verdict = nvenc::evaluate_frame_budget(4, k1080pWidth, k1080pHeight, 120.0, 1, true);
  EXPECT_FALSE(verdict.clamped);
  EXPECT_FALSE(verdict.budget_exceeded_at_floor);
  EXPECT_EQ(verdict.effective_preset, 4);
  EXPECT_NEAR(verdict.budget_ms, 1000.0 / 120.0 * 0.65, 1e-9);
  EXPECT_LT(verdict.estimated_ms, verdict.budget_ms);
}

TEST(NvencFrameBudgetTest, SlowestPresetStillFitsAt1080p120) {
  const auto verdict = nvenc::evaluate_frame_budget(7, k1080pWidth, k1080pHeight, 120.0, 1, true);
  EXPECT_FALSE(verdict.clamped);
  EXPECT_EQ(verdict.effective_preset, 7);
}

TEST(NvencFrameBudgetTest, FourKAt60KeepsP4) {
  const auto verdict = nvenc::evaluate_frame_budget(4, k4kWidth, k4kHeight, 60.0, 1, true);
  EXPECT_FALSE(verdict.clamped);
  EXPECT_EQ(verdict.effective_preset, 4);
}

TEST(NvencFrameBudgetTest, FourKAt90StepsDownToP2) {
  const auto verdict = nvenc::evaluate_frame_budget(4, k4kWidth, k4kHeight, 90.0, 1, true);
  EXPECT_TRUE(verdict.clamped);
  EXPECT_EQ(verdict.configured_preset, 4);
  EXPECT_EQ(verdict.effective_preset, 2);
  EXPECT_FALSE(verdict.budget_exceeded_at_floor);
  EXPECT_LE(verdict.estimated_ms, verdict.budget_ms);
  EXPECT_GT(verdict.configured_estimated_ms, verdict.budget_ms);
}

TEST(NvencFrameBudgetTest, FourKAt120OnOneEngineFallsToP1) {
  const auto verdict = nvenc::evaluate_frame_budget(4, k4kWidth, k4kHeight, 120.0, 1, true);
  EXPECT_TRUE(verdict.clamped);
  EXPECT_EQ(verdict.effective_preset, 1);
  // P1 is the floor: it is used even though it is estimated not to fit.
  EXPECT_TRUE(verdict.budget_exceeded_at_floor);
}

TEST(NvencFrameBudgetTest, SplitFrameEnginesRaiseTheAffordablePreset) {
  const auto p4 = nvenc::evaluate_frame_budget(4, k4kWidth, k4kHeight, 120.0, 2, true);
  EXPECT_FALSE(p4.clamped);
  EXPECT_EQ(p4.effective_preset, 4);
  EXPECT_EQ(p4.num_engines, 2);

  const auto p7 = nvenc::evaluate_frame_budget(7, k4kWidth, k4kHeight, 120.0, 2, true);
  EXPECT_TRUE(p7.clamped);
  EXPECT_EQ(p7.effective_preset, 5);
}

TEST(NvencFrameBudgetTest, DisabledGuardKeepsTheConfiguredPresetButReportsTheBudget) {
  const auto verdict = nvenc::evaluate_frame_budget(4, k4kWidth, k4kHeight, 120.0, 1, false);
  EXPECT_FALSE(verdict.clamped);
  EXPECT_FALSE(verdict.budget_exceeded_at_floor);
  EXPECT_EQ(verdict.effective_preset, 4);
  EXPECT_GT(verdict.budget_ms, 0.0);
  EXPECT_DOUBLE_EQ(verdict.estimated_ms, verdict.configured_estimated_ms);
  EXPECT_GT(verdict.estimated_ms, verdict.budget_ms);
}

TEST(NvencFrameBudgetTest, MissingSessionInfoKeepsTheConfiguredPreset) {
  for (const auto &[width, height, fps] : {std::tuple {0, k4kHeight, 120.0}, std::tuple {k4kWidth, 0, 120.0}, std::tuple {k4kWidth, k4kHeight, 0.0}}) {
    const auto verdict = nvenc::evaluate_frame_budget(7, width, height, fps, 1, true);
    EXPECT_FALSE(verdict.clamped);
    EXPECT_EQ(verdict.effective_preset, 7);
    EXPECT_EQ(verdict.budget_ms, 0.0);
  }
}

TEST(NvencFrameBudgetTest, OutOfRangeInputsAreClamped) {
  EXPECT_EQ(nvenc::evaluate_frame_budget(9, k1080pWidth, k1080pHeight, 60.0, 1, true).effective_preset, 7);
  EXPECT_EQ(nvenc::evaluate_frame_budget(0, k1080pWidth, k1080pHeight, 60.0, 1, true).effective_preset, 1);
  EXPECT_EQ(nvenc::evaluate_frame_budget(4, k1080pWidth, k1080pHeight, 60.0, 0, true).num_engines, 1);
}
