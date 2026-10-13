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
   * @brief How split-frame encoding is set up for a session, as far as the guard is concerned.
   */
  enum class split_frame_budget {
    unavailable,  ///< H.264, API or headers older than 12.1, HEVC weighted prediction, or split disabled.
    implicit,  ///< NV_ENC_SPLIT_AUTO_MODE: the driver splits only for some presets and resolutions.
    forced,  ///< NV_ENC_SPLIT_AUTO_FORCED_MODE: every frame is split.
  };

  /**
   * @brief What the guard needs to know about a session.
   */
  struct frame_budget_session {
    int width = 0;  ///< Encode width in pixels.
    int height = 0;  ///< Encode height in pixels.
    double fps = 0.0;  ///< Stream framerate.
    int video_format = 0;  ///< 0 = H.264, 1 = HEVC, 2 = AV1.
    bool yuv444 = false;  ///< 4:4:4 chroma, twice the samples of 4:2:0.
    int num_engines = 1;  ///< NVENC engines on the GPU.
    split_frame_budget split = split_frame_budget::unavailable;
  };

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
    int num_engines = 1;  ///< NVENC engines counted towards the estimate at the effective preset.
  };

  /**
   * @brief Number of NVENC engines a frame is spread over at a given preset.
   *
   * Forced split-frame encoding uses every engine. In implicit mode NVIDIA's programming guide
   * documents the split only for P1-P4 with (ultra) low-latency tuning, which Sunshine always uses,
   * and for frames at least 2112 (HEVC) or 2048 (AV1) pixels high; anything else runs on one engine.
   * @param session The session.
   * @param preset NVENC quality preset 1-7.
   * @return The engines counted, at least 1.
   */
  int split_frame_engines(const frame_budget_session &session, int preset);

  /**
   * @brief Pick the highest preset, up to the configured one, whose estimated per-frame encode
   *        time fits within the frame budget of the session. Pure function, no hardware access.
   * @param configured_preset NVENC quality preset 1-7 (out-of-range values are clamped).
   * @param session The session.
   * @param guard_enabled When false the configured preset is kept; the budget fields are still filled in.
   * @return The verdict. Without a usable resolution or framerate, the configured preset is kept.
   */
  frame_budget_verdict evaluate_frame_budget(int configured_preset, const frame_budget_session &session, bool guard_enabled);

}  // namespace nvenc
