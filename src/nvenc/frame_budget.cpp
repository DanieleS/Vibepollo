/**
 * @file src/nvenc/frame_budget.cpp
 * @brief Definitions for the NVENC frame budget guard.
 */
// this include
#include "frame_budget.h"

// standard includes
#include <algorithm>

namespace nvenc {

  namespace {

    /**
     * Estimated single-engine encode time per preset, in milliseconds per megapixel, indexed by
     * preset number. Calibrated on RTX 50-series measurements (4K60: P1 ~5.3 ms, P7 ~10.4 ms per
     * frame) and rounded up to stay conservative on older generations. It is a heuristic: the guard
     * can be turned off with `nvenc_frame_budget_guard`.
     */
    constexpr double encode_ms_per_mpix[8] = {0.0, 0.7, 0.8, 0.9, 1.0, 1.1, 1.2, 1.4};

    /**
     * Share of the frame interval encoding may take. The rest belongs to capture, conversion
     * and sending the frame.
     */
    constexpr double frame_budget_utilization = 0.65;

    /**
     * Throughput gain when split-frame encoding can spread a frame over several NVENC engines.
     */
    constexpr double multi_engine_speedup = 1.75;

  }  // namespace

  frame_budget_verdict evaluate_frame_budget(int configured_preset, int width, int height, double fps, int num_engines, bool guard_enabled) {
    configured_preset = std::clamp(configured_preset, 1, 7);
    num_engines = std::max(1, num_engines);

    frame_budget_verdict verdict;
    verdict.configured_preset = configured_preset;
    verdict.effective_preset = configured_preset;
    verdict.fps = fps;
    verdict.num_engines = num_engines;

    if (fps <= 0.0 || width <= 0 || height <= 0) {
      // Nothing to reason about; keep the configured preset.
      return verdict;
    }

    verdict.budget_ms = 1000.0 / fps * frame_budget_utilization;

    const double mpix = static_cast<double>(width) * height / 1e6;
    const double speedup = num_engines > 1 ? multi_engine_speedup : 1.0;
    const auto estimate_ms = [&](int preset) {
      return encode_ms_per_mpix[preset] * mpix / speedup;
    };

    verdict.configured_estimated_ms = estimate_ms(configured_preset);
    verdict.estimated_ms = verdict.configured_estimated_ms;
    if (!guard_enabled) {
      return verdict;
    }

    int preset = configured_preset;
    while (preset > 1 && estimate_ms(preset) > verdict.budget_ms) {
      --preset;
    }

    verdict.effective_preset = preset;
    verdict.estimated_ms = estimate_ms(preset);
    verdict.clamped = preset < configured_preset;
    verdict.budget_exceeded_at_floor = verdict.estimated_ms > verdict.budget_ms;
    return verdict;
  }

}  // namespace nvenc
