#include "../tests_common.h"

#include "src/video_fec.h"

#include <cmath>
#include <cstdint>
#include <optional>

namespace {
  using namespace video_fec;

  // stream.cpp's block split on frame bytes, for checking layout() against it.
  struct stream_split_t {
    std::size_t blocks;
    std::size_t first_block_shards;
    bool skipped;
  };

  stream_split_t stream_split(std::size_t payload, std::size_t blocksize, unsigned percentage) {
    const std::size_t max_data_shards_per_fec_block = (255 * 100) / (100 + percentage);
    const auto max_data_per_fec_block = max_data_shards_per_fec_block * blocksize;
    auto blocks = (payload + max_data_per_fec_block - 1) / max_data_per_fec_block;
    bool skipped = false;
    if (blocks > 4) {
      blocks = 4;
      skipped = true;
    }
    const auto unaligned_size = payload / blocks;
    const auto aligned_size = ((unaligned_size + blocksize - 1) / blocksize) * blocksize;
    return {blocks, aligned_size / blocksize, skipped};
  }
}  // namespace

TEST(VideoFecTest, ParsesClientPreference) {
  EXPECT_EQ(parse_preference("-2"), host_preference);
  EXPECT_EQ(parse_preference("-1"), automatic_preference);
  EXPECT_EQ(parse_preference("0"), 0);
  EXPECT_EQ(parse_preference("100"), 100);
  EXPECT_FALSE(parse_preference(""));
  EXPECT_FALSE(parse_preference("-3"));
  EXPECT_FALSE(parse_preference("101"));
  EXPECT_FALSE(parse_preference("20%"));
  EXPECT_FALSE(parse_preference("auto"));
}

TEST(VideoFecTest, LayoutMatchesTheStreamSplit) {
  constexpr std::size_t blocksize = 1392 + 16;
  for (unsigned percentage : {0u, 1u, 5u, 20u, 50u, 100u}) {
    for (std::size_t payload = 1; payload < 1200 * blocksize; payload += 997) {
      const auto shards = (payload + blocksize - 1) / blocksize;
      const auto expected = stream_split(payload, blocksize, percentage);
      const auto actual = layout(shards, percentage, 2);
      ASSERT_EQ(actual.blocks, expected.blocks) << payload << " bytes at " << percentage << '%';
      ASSERT_EQ(actual.skipped, expected.skipped) << payload << " bytes at " << percentage << '%';
      // Rounding whole shards can only overestimate a block by one shard.
      ASSERT_GE(actual.data[0], expected.first_block_shards);
      ASSERT_LE(actual.data[0], expected.first_block_shards + 1);

      std::size_t total = 0;
      for (std::size_t i = 0; i < actual.blocks; ++i) {
        total += actual.data[i];
        if (!actual.skipped) {
          ASSERT_LE(actual.data[i] + actual.parity[i], 255u);
        }
      }
      ASSERT_EQ(total, shards);
    }
  }
}

TEST(VideoFecTest, LayoutAppliesTheMinimumParityOnlyWithFec) {
  EXPECT_EQ(layout(5, 1, 2).parity[0], 2u);
  EXPECT_EQ(layout(5, 0, 2).parity[0], 0u);
  EXPECT_EQ(layout(100, 20, 2).parity[0], 20u);
  const auto skipped = layout(2000, 20, 2);
  EXPECT_TRUE(skipped.skipped);
  EXPECT_EQ(skipped.parity[0], 0u);
}

TEST(VideoFecTest, BlockFailureMatchesTheBinomialTail) {
  EXPECT_EQ(block_failure(10, 2, 0), 0);
  EXPECT_EQ(block_failure(10, 10, 0.5), 0);
  EXPECT_NEAR(block_failure(10, 0, 0.1), 1 - std::pow(0.9, 10), 1e-12);
  EXPECT_NEAR(block_failure(3, 1, 0.5), 0.5, 1e-12);
  EXPECT_EQ(block_failure(10, 2, 1), 1);

  // Parity far below the expected losses: the answer is near 1, not an underflowed 0.
  EXPECT_GT(block_failure(255, 2, 0.99), 0.999);
  EXPECT_NEAR(block_failure(20, 3, 0.3), 0.8929131954962691, 1e-9);
  EXPECT_NEAR(block_failure(200, 30, 0.05) / 2.4782444268131164e-08, 1, 1e-6);
}

TEST(VideoFecTest, SelectsTheLowestSufficientPercentage) {
  EXPECT_EQ(select_percentage(0, 100, 2, automatic_maximum), 0u);
  EXPECT_EQ(select_percentage(0.01, 100, 2, 0), 0u);

  unsigned previous = 0;
  for (double loss : {0.0005, 0.002, 0.01, 0.03, 0.08}) {
    const auto selected = select_percentage(loss, 100, 2, automatic_maximum);
    EXPECT_GE(selected, previous) << loss;
    previous = selected;
    ASSERT_GT(selected, 0u);
    ASSERT_LT(selected, automatic_maximum);

    const auto frame = layout(100, selected, 2);
    EXPECT_LE(block_failure(frame.data[0] + frame.parity[0], frame.parity[0], loss), 0.001) << loss;
    if (selected > 1) {
      const auto lower = layout(100, selected - 1, 2);
      EXPECT_GT(block_failure(lower.data[0] + lower.parity[0], lower.parity[0], loss), 0.001) << loss;
    }
  }

  // Unreachable targets get the cap, never a percentage that skips FEC.
  EXPECT_EQ(select_percentage(0.4, 100, 2, automatic_maximum), automatic_maximum);
  EXPECT_FALSE(layout(800, select_percentage(0.4, 800, 2, automatic_maximum), 2).skipped);
}

TEST(VideoFecTest, LossWindowKeepsTheWorstSecondForTenSeconds) {
  loss_window_t window;
  EXPECT_FALSE(window.estimate(0));
  window.record(1000, 0, 0);
  EXPECT_EQ(window.estimate(0), 0.0);
  window.record(1000, 50, 1000);
  window.record(1000, 10, 2000);
  EXPECT_DOUBLE_EQ(*window.estimate(2000), 0.05);
  EXPECT_DOUBLE_EQ(*window.estimate(10999), 0.05);
  EXPECT_DOUBLE_EQ(*window.estimate(11000), 0.01);
  EXPECT_FALSE(window.estimate(12000));
}

TEST(VideoFecTest, FixedModeIgnoresFeedback) {
  controller_t controller {20, 10};
  EXPECT_FALSE(controller.automatic());
  EXPECT_EQ(controller.percentage(100, 2, 0), 10u);
  EXPECT_TRUE(controller.report(1, 1000, 500, 0));
  EXPECT_EQ(controller.percentage(100, 2, 0), 10u);
}

TEST(VideoFecTest, AutomaticModeFollowsFreshFeedback) {
  controller_t controller {20, std::nullopt};
  EXPECT_TRUE(controller.automatic());
  EXPECT_EQ(controller.percentage(100, 2, 0), 20u);  // No feedback yet.

  ASSERT_TRUE(controller.report(1, 1000, 20, 1000));
  const auto lossy = controller.percentage(100, 2, 1000);
  EXPECT_EQ(lossy, select_percentage(0.02, 100, 2, automatic_maximum));
  EXPECT_GT(lossy, 0u);

  // Stale feedback falls back to the host's percentage.
  EXPECT_EQ(controller.percentage(100, 2, 1000 + controller_t::feedback_timeout_ms + 1), 20u);
}

TEST(VideoFecTest, CleanFeedbackAtSessionStartNeedsAFullWindow) {
  controller_t controller {20, std::nullopt};

  // One clean second is not enough to drop parity at the start of a session.
  ASSERT_TRUE(controller.report(1, 1000, 0, 1000));
  EXPECT_EQ(controller.percentage(100, 2, 1000), 20u);

  std::uint32_t sequence = 1;
  std::int64_t now = 1000;
  while (now + 1000 < 1000 + loss_window_t::duration_ms) {
    now += 1000;
    ASSERT_TRUE(controller.report(++sequence, 1000, 0, now));
    EXPECT_EQ(controller.percentage(100, 2, now), 20u) << now;
  }
  now += 1000;
  ASSERT_TRUE(controller.report(++sequence, 1000, 0, now));
  EXPECT_EQ(controller.percentage(100, 2, now), 0u);
}

TEST(VideoFecTest, LossAtSessionStartRaisesProtectionAtOnce) {
  controller_t controller {20, std::nullopt};
  ASSERT_TRUE(controller.report(1, 1000, 30, 1000));
  EXPECT_EQ(controller.percentage(100, 2, 1000), select_percentage(0.03, 100, 2, automatic_maximum));
}

TEST(VideoFecTest, RejectsInvalidAndOutOfOrderSummaries) {
  controller_t controller {20, std::nullopt};
  EXPECT_FALSE(controller.report(1, 0, 0, 0));
  EXPECT_FALSE(controller.report(1, 10, 11, 0));
  EXPECT_FALSE(controller.report(1, 2000000, 0, 0));
  EXPECT_FALSE(controller.report(1, 10, 0, -1));

  ASSERT_TRUE(controller.report(5, 1000, 0, 1000));
  EXPECT_FALSE(controller.report(5, 1000, 0, 2000));  // Duplicate.
  EXPECT_FALSE(controller.report(4, 1000, 0, 2000));  // Backwards.
  EXPECT_FALSE(controller.report(6, 1000, 0, 500));  // Time went backwards.

  controller_t wrapping {20, std::nullopt};
  ASSERT_TRUE(wrapping.report(0xFFFFFFFFu, 1000, 0, 1000));
  EXPECT_TRUE(wrapping.report(0, 1000, 0, 2000));
}

TEST(VideoFecTest, CleanFeedbackAfterAGapNeedsAFullWindow) {
  controller_t controller {20, std::nullopt};
  ASSERT_TRUE(controller.report(1, 1000, 30, 1000));
  ASSERT_GT(controller.percentage(100, 2, 1000), 0u);

  // Summary 2 never arrived: the old history is dropped and a clean report is not trusted yet.
  ASSERT_TRUE(controller.report(3, 1000, 0, 3000));
  EXPECT_EQ(controller.percentage(100, 2, 3000), 20u);

  std::uint32_t sequence = 3;
  std::int64_t now = 3000;
  while (now < 3000 + loss_window_t::duration_ms) {
    now += 1000;
    ASSERT_TRUE(controller.report(++sequence, 1000, 0, now));
  }
  EXPECT_EQ(controller.percentage(100, 2, now), 0u);

  // Loss reported after a gap still raises protection immediately.
  ASSERT_TRUE(controller.report(sequence + 2, 1000, 30, now + 1000));
  EXPECT_GT(controller.percentage(100, 2, now + 1000), 0u);
}

TEST(VideoFecTest, BudgetSizesTheEncoderForTheAllowance) {
  budget_t budget {24000, 20};
  EXPECT_EQ(budget.allowance(), 20u);
  EXPECT_EQ(budget.encoder_kbps(), 20000);
  EXPECT_EQ(budget_t::encoder_kbps_for(24000, 0), 24000);
}

TEST(VideoFecTest, BudgetRequestsOncePerWindow) {
  budget_t budget {24000, 20};
  EXPECT_FALSE(budget.observe(5, 0));
  EXPECT_FALSE(budget.observe(3, 500));
  EXPECT_FALSE(budget.observe(5, 999));

  // The window's peak, 5%, becomes the new encoder target.
  const auto request = budget.observe(4, 1000);
  ASSERT_TRUE(request);
  EXPECT_EQ(request->percentage, 5u);
  EXPECT_EQ(request->encoder_kbps, budget_t::encoder_kbps_for(24000, 5));

  // Lowering protection does not wait for the encoder, but the allowance does.
  EXPECT_EQ(budget.allowance(), 20u);
  EXPECT_FALSE(budget.observe(5, 1500));  // Same change, still within the window.
  budget.applied(request->encoder_kbps);
  EXPECT_EQ(budget.allowance(), 5u);
  EXPECT_FALSE(budget.observe(5, 2000));
}

TEST(VideoFecTest, BudgetRetriesARefusedChangeOncePerWindow) {
  budget_t budget {24000, 0};
  budget.observe(30, 0);
  const auto first = budget.observe(30, 1000);
  ASSERT_TRUE(first);

  // The encoder never confirmed it: protection must not stay frozen at 0%.
  EXPECT_FALSE(budget.observe(30, 1999));
  const auto retry = budget.observe(30, 2000);
  ASSERT_TRUE(retry);
  EXPECT_EQ(retry->encoder_kbps, first->encoder_kbps);
  EXPECT_EQ(budget.allowance(), 0u);

  budget.applied(retry->encoder_kbps);
  EXPECT_EQ(budget.allowance(), 30u);
}

TEST(VideoFecTest, BudgetRaisesProtectionOnlyAfterTheEncoderMakesRoom) {
  budget_t budget {24000, 0};
  budget.observe(30, 0);
  const auto request = budget.observe(30, 1000);
  ASSERT_TRUE(request);
  EXPECT_EQ(budget.allowance(), 0u);
  budget.applied(request->encoder_kbps);
  EXPECT_EQ(budget.allowance(), 30u);
}

TEST(VideoFecTest, BudgetIgnoresOnePointWobbleButNotZero) {
  budget_t budget {24000, 10};
  budget.observe(11, 0);
  EXPECT_FALSE(budget.observe(11, 1000));

  budget_t from_zero {24000, 0};
  from_zero.observe(1, 0);
  EXPECT_TRUE(from_zero.observe(1, 1000));

  budget_t to_zero {24000, 1};
  to_zero.observe(0, 0);
  EXPECT_TRUE(to_zero.observe(0, 1000));
}

TEST(VideoFecTest, BudgetFollowsRuntimeBitrateChanges) {
  budget_t budget {24000, 20};
  const auto encoder = budget.set_video_kbps(12000);
  EXPECT_EQ(encoder, 10000);
  EXPECT_EQ(budget.video_kbps(), 12000);
  budget.applied(encoder);
  EXPECT_EQ(budget.allowance(), 20u);
}

TEST(VideoFecTest, EncoderTargetIsSizedFromTheBudgetWhenApplied) {
  // A change requested under the old budget lands after a runtime bitrate request:
  // the encoder sizes it from the new budget, never the old one.
  budget_t budget {24000, 20};
  budget.observe(0, 0);
  const auto change = budget.observe(0, 1000);
  ASSERT_TRUE(change);
  EXPECT_EQ(change->encoder_kbps, 24000);

  budget.applied(budget.set_video_kbps(12000));
  const auto applied = budget.encoder_kbps_at(change->percentage);
  EXPECT_EQ(applied, 12000);
  budget.applied(applied);
  EXPECT_EQ(budget.allowance(), 0u);
  EXPECT_EQ(budget.encoder_kbps(), 12000);
}
