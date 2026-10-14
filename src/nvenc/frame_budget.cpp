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
     * Estimated single-engine encode time per preset, in milliseconds per megapixel of 4:2:0
     * video, indexed by preset number. Taken from Alkaid's calibration on community RTX 50-series
     * measurements (4K60: P1 ~5.3 ms, P7 ~10.4 ms per frame), rounded up to stay conservative on
     * older generations. The settings of those measurements are not known, so the cost of spatial
     * AQ, two-pass and 10-bit is only covered by that rounding. It is a heuristic: the guard can
     * be turned off with `nvenc_frame_budget_guard`.
     */
    constexpr double encode_ms_per_mpix[8] = {0.0, 0.7, 0.8, 0.9, 1.0, 1.1, 1.2, 1.4};

    /**
     * Share of the frame interval encoding may take. The rest belongs to capture, conversion
     * and sending the frame.
     */
    constexpr double frame_budget_utilization = 0.65;

    /**
     * Throughput gain when split-frame encoding spreads a frame over several NVENC engines.
     */
    constexpr double multi_engine_speedup = 1.75;

    /**
     * 4:4:4 carries 3 samples per pixel against 1.5 for 4:2:0; assume the cost scales with them.
     */
    constexpr double yuv444_cost_factor = 2.0;

    /**
     * Highest preset for which the driver splits frames in implicit mode (low-latency tunings).
     */
    constexpr int implicit_split_max_preset = 4;

    constexpr int implicit_split_min_height_hevc = 2112;
    constexpr int implicit_split_min_height_av1 = 2048;

  }  // namespace

  int split_frame_engines(const frame_budget_session &session, int preset) {
    const int engines = std::max(1, session.num_engines);
    switch (session.split) {
      case split_frame_budget::forced:
        return engines;
      case split_frame_budget::implicit:
        {
          const int min_height = session.video_format == 1 ? implicit_split_min_height_hevc :
                                 session.video_format == 2 ? implicit_split_min_height_av1 :
                                                             0;
          if (min_height > 0 && preset <= implicit_split_max_preset && session.height >= min_height) {
            return engines;
          }
          return 1;
        }
      case split_frame_budget::unavailable:
      default:
        return 1;
    }
  }

  frame_budget_verdict evaluate_frame_budget(int configured_preset, const frame_budget_session &session, bool guard_enabled) {
    configured_preset = std::clamp(configured_preset, 1, 7);

    frame_budget_verdict verdict;
    verdict.configured_preset = configured_preset;
    verdict.effective_preset = configured_preset;
    verdict.fps = session.fps;
    verdict.num_engines = split_frame_engines(session, configured_preset);

    if (session.fps <= 0.0 || session.width <= 0 || session.height <= 0) {
      // Nothing to reason about; keep the configured preset.
      return verdict;
    }

    verdict.budget_ms = 1000.0 / session.fps * frame_budget_utilization;

    const double mpix = static_cast<double>(session.width) * session.height / 1e6 * (session.yuv444 ? yuv444_cost_factor : 1.0);
    const auto estimate_ms = [&](int preset) {
      const double speedup = split_frame_engines(session, preset) > 1 ? multi_engine_speedup : 1.0;
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
    verdict.num_engines = split_frame_engines(session, preset);
    verdict.clamped = preset < configured_preset;
    verdict.budget_exceeded_at_floor = verdict.estimated_ms > verdict.budget_ms;
    return verdict;
  }

}  // namespace nvenc
