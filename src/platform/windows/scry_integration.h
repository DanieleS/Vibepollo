/**
 * @file src/platform/windows/scry_integration.h
 * @brief Game-memory telemetry, read out of the running game by the `scry` helper.
 *
 * `scry.exe` is a **read-only** telemetry engine: given a running game and a
 * per-game profile, it emits the values that profile names (HP, party, lap
 * time…) as one JSON object per line on stdout. It is driven as a subprocess,
 * never linked, for two reasons that both matter here: the read loop runs
 * against a game's address space, so a hang or a crash in it must not be able to
 * take the streaming host down with it; and it can be launched into the user's
 * session with the user's own token, rather than reading a game's memory from a
 * SYSTEM service.
 *
 * This module owns that subprocess: it decides *which* process is the game,
 * keeps the helper attached to it, and maintains the last known value of every
 * watch. Consumers read @ref latest — a complete picture, not a delta — because
 * a client can connect at any point in a session and must be able to render
 * immediately.
 */
#pragma once

#ifdef _WIN32

  #include <cstdint>
  #include <memory>
  #include <optional>
  #include <string>

  #include <nlohmann/json.hpp>

  #include "src/platform/common.h"

namespace platf::scry {

  /**
   * @brief Everything currently known about the attached game.
   *
   * `values` is the **full** current picture, not the tick's diff: the helper
   * reports only what changed, and this module accumulates. That is what lets a
   * client that joins mid-session be served a complete frame at once.
   */
  struct snapshot_t {
    /// Whether the helper is attached to a game right now. When false every
    /// other field is stale or empty, and nothing should be sent.
    bool attached {false};

    /// Bumped on every observed change. A consumer that remembers the last
    /// revision it sent knows, in one comparison, whether there is news — and a
    /// consumer that has sent nothing yet (revision 0) naturally gets a full
    /// snapshot on its first look.
    std::uint64_t revision {0};

    /// The app this telemetry belongs to, as Vibepollo knows it. Phase 1 has no
    /// profile registry, so this is the configured app name; when the registry
    /// lands this becomes its slug and the meaning of the field does not change.
    std::string slug;

    /// Executable the helper attached to (the *game*, which is often not the
    /// process Vibepollo launched — launchers spawn the real thing).
    std::string process;

    /// Label of the profile that claimed the process, and the file it came from.
    /// The profile is chosen by the target's memory (scry's probe test), never by
    /// us, so both are reported rather than assumed.
    std::string profile;
    std::string profile_file;

    /// Shape of `values`, as declared by the winning profile. Absent when the
    /// profile declares no contract; a renderer downstream keys off this.
    std::optional<std::uint32_t> contract_version;

    /// Watch name → last known value. A watch that went unreadable is `null`,
    /// which is a different claim from being absent.
    nlohmann::json values = nlohmann::json::object();
  };

  /**
   * @brief Start the supervisor thread.
   *
   * Idempotent-ish in the usual Sunshine sense: the returned guard stops the
   * supervisor and kills any running helper when destroyed. Safe to call when
   * the feature is disabled — the supervisor simply parks and re-checks, so a
   * config change takes effect without a restart.
   */
  std::unique_ptr<::platf::deinit_t> start();

  /**
   * @brief The current picture. Thread-safe; cheap enough to poll.
   */
  snapshot_t latest();

  /**
   * @brief Whether the helper binary is installed next to Sunshine.
   *
   * Reported separately from "enabled" so the UI can tell a user who switched
   * the feature on that the tooling is missing, instead of failing silently.
   */
  bool helper_available();

}  // namespace platf::scry

#endif  // _WIN32
