/**
 * @file src/game_metadata_bulk.h
 * @brief Background job that fills app metadata from the IGDB mirror (LizardByte GameDB).
 *
 * One job runs at a time. It walks apps.json, looks each app up by name, and writes the
 * provider-neutral `metadata-*` keys (see game_metadata_policy.h) plus optional background
 * art and cover. Requests are paced to at most four per second, the limit IGDB documents
 * for its own API, so the mirror and images.igdb.com are never hammered.
 */
#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace game_metadata::bulk {

  struct options_t {
    // Re-fetch apps that already carry descriptive metadata (from any provider). Off by default
    // so Playnite-owned and previously fetched entries are left alone.
    bool refresh_existing {false};
    // Download the first artwork as the app background (PNG on images.igdb.com).
    bool download_background {true};
    // Download the cover for apps that have no cover yet. Existing covers are never replaced.
    bool download_cover {true};
    // Restrict the run to these app UUIDs; empty means every app.
    std::vector<std::string> uuids;
  };

  /**
   * @brief Start a job. Returns false when one is already running.
   */
  bool start(options_t options);

  /**
   * @brief Ask the running job to stop after the current app. Returns false when idle.
   */
  bool cancel();

  /**
   * @brief Snapshot of the current or last job, as served by the Web UI status endpoint.
   */
  nlohmann::json status();

  /**
   * @brief Stop and join the worker. Called during shutdown.
   */
  void shutdown();

}  // namespace game_metadata::bulk
