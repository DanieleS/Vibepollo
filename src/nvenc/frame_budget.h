/**
 * @file src/nvenc/frame_budget.h
 * @brief Declarations for the NVENC frame budget guard.
 *
 * Kept free of NVENC SDK includes so the estimator can be unit tested without a GPU.
 * Ported from AlkaidLab/foundation-sunshine (a791716d, src/nvenc/frame_budget.*), GPL-3.0.
 */
#pragma once

namespace nvenc {

  /**
   * @brief Result of checking a quality preset against the frame interval of a session.
   */
  struct frame_budget_verdict {
    bool clamped = false;  ///< The effective preset was lowered to fit the budget.
    bool budget_exceeded_at_floor = false;  ///< Even P1 is estimated not to fit the budget.
    int configured_preset = 1;
    int effective_preset = 1;
    double fps = 0.0;
    double budget_ms = 0.0;  ///< Encode time allowed per frame (a fraction of the frame interval).
    double estimated_ms = 0.0;  ///< Estimated encode time at the effective preset.
    double configured_estimated_ms = 0.0;  ///< Estimated encode time at the configured preset.
    int num_engines = 1;  ///< NVENC engines counted towards the estimate.
  };

  /**
   * @brief Pick the highest preset, up to the configured one, whose estimated per-frame encode
   *        time fits within the frame budget of the session. Pure function, no hardware access.
   * @param configured_preset NVENC quality preset 1-7 (out-of-range values are clamped).
   * @param width Encode width in pixels.
   * @param height Encode height in pixels.
   * @param fps Stream framerate.
   * @param num_engines NVENC engines a frame can be spread over (1 unless split-frame encoding may engage).
   * @param guard_enabled When false the configured preset is kept; the budget fields are still filled in.
   * @return The verdict. Without a usable resolution or framerate, the configured preset is kept.
   */
  frame_budget_verdict evaluate_frame_budget(int configured_preset, int width, int height, double fps, int num_engines, bool guard_enabled);

}  // namespace nvenc
