/**
 * @file src/metadata_resolver.cpp
 * @brief Fills app metadata from IGDB, whichever launcher the app came from.
 */

// local includes
#include "metadata_resolver.h"

#include "config.h"
#include "confighttp.h"
#include "file_handler.h"
#include "game_metadata.h"
#include "igdb_client.h"
#include "igdb_policy.h"
#include "logging.h"
#include "platform/common.h"

// standard includes
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

namespace metadata::resolver {
  namespace {
    // What apps.json holds when nothing has ever supplied a cover. Replacing it is not
    // replacing the user's choice; replacing anything else would be.
    constexpr const char *k_placeholder_cover = "./assets/box.png";

    std::filesystem::path covers_root() {
      return platf::appdata() / "covers";
    }

    std::string app_name(const nlohmann::json &app) {
      if (auto it = app.find("name"); it != app.end() && it->is_string()) {
        return it->get<std::string>();
      }
      return {};
    }

    bool cover_is_ours_to_set(const nlohmann::json &app) {
      auto it = app.find("image-path");
      if (it == app.end() || !it->is_string()) {
        return true;
      }
      const auto path = it->get<std::string>();
      return path.empty() || path == k_placeholder_cover;
    }

    /**
     * @brief Merge an IGDB record onto whatever the app already carries.
     *
     * Playtime and last-played survive untouched: they come from whoever launched the game and
     * a database has no opinion on them. So does a background the launcher already converted,
     * unless IGDB has artwork and the app had none.
     */
    bool apply_record(nlohmann::json &app, const igdb::policy::game_t &record) {
      const auto before = read_from_app(app);
      auto merged = igdb::policy::to_metadata(record);
      merged.last_played = before.last_played;
      merged.playtime_minutes = before.playtime_minutes;
      merged.locked = before.locked;
      merged.background_image_path = before.background_image_path;

      if (merged.background_image_path.empty()) {
        merged.background_image_path = igdb::download_background(record, covers_root());
      }
      merged.present = has_any_value(merged);
      write_to_app(app, merged);

      if (cover_is_ours_to_set(app)) {
        const auto cover = igdb::download_cover(record, covers_root());
        if (!cover.empty()) {
          app["image-path"] = cover;
        }
      }
      return true;
    }

    /// @brief The IGDB id for an app, by store id first and by title second.
    std::string find_igdb_id(const nlohmann::json &app, std::string &error_out) {
      const auto ids = store_ids_of(app);
      if (!ids.empty()) {
        const auto matched = igdb::resolve_store_ids(ids, error_out);
        if (!matched.empty()) {
          return matched;
        }
        if (!error_out.empty()) {
          return {};
        }
      }
      if (!config::igdb.allow_name_match) {
        return {};
      }
      const auto name = app_name(app);
      if (name.empty()) {
        return {};
      }
      const auto hits = igdb::search(name, 10, error_out);
      if (hits.empty()) {
        return {};
      }
      const auto best = igdb::policy::best_name_match(hits, name);
      return best ? best->igdb_id : std::string {};
    }
  }  // namespace

  outcome_t resolve_app(nlohmann::json &app, bool force) {
    outcome_t outcome;
    if (!app.is_object()) {
      return outcome;
    }
    const auto existing = read_from_app(app);
    outcome.igdb_id = existing.igdb_id;

    if (existing.locked && !force) {
      outcome.skipped_locked = true;
      return outcome;
    }
    // Already described by IGDB and not asked to refresh: nothing to do and no request spent.
    if (!force && existing.source == "igdb" && existing.present) {
      return outcome;
    }

    std::string error;
    auto igdb_id = existing.igdb_id;
    if (igdb_id.empty()) {
      igdb_id = find_igdb_id(app, error);
    }
    if (igdb_id.empty()) {
      outcome.error = error;
      return outcome;
    }

    const auto record = igdb::fetch_game(igdb_id, error);
    if (!record) {
      outcome.error = error;
      return outcome;
    }
    outcome.igdb_id = igdb_id;
    outcome.changed = apply_record(app, *record);
    return outcome;
  }

  bool apply_igdb_id(nlohmann::json &app, const std::string &igdb_id, std::string &error_out) {
    if (!app.is_object()) {
      error_out = "Not an app";
      return false;
    }
    if (igdb_id.empty()) {
      // Unlinking: the descriptive fields go with the link, but the launcher-owned playtime
      // and whatever cover the user has set both stay.
      const auto before = read_from_app(app);
      game_metadata_t cleared;
      cleared.last_played = before.last_played;
      cleared.playtime_minutes = before.playtime_minutes;
      cleared.present = has_any_value(cleared);
      write_to_app(app, cleared);
      return true;
    }
    const auto record = igdb::fetch_game(igdb_id, error_out);
    if (!record) {
      return false;
    }
    apply_record(app, *record);
    // The user chose this record, so a later automatic pass must not quietly replace it.
    auto meta = read_from_app(app);
    meta.locked = true;
    meta.igdb_id = igdb_id;
    write_to_app(app, meta);
    return true;
  }

  summary_t resolve_all(nlohmann::json &root, bool force) {
    summary_t summary;
    auto apps = root.find("apps");
    if (apps == root.end() || !apps->is_array()) {
      return summary;
    }
    for (auto &app : *apps) {
      if (!app.is_object()) {
        continue;
      }
      ++summary.considered;
      const auto outcome = resolve_app(app, force);
      if (outcome.skipped_locked) {
        ++summary.skipped;
        continue;
      }
      if (!outcome.error.empty()) {
        ++summary.failed;
        if (summary.error.empty()) {
          summary.error = outcome.error;
        }
        // One failure is a game IGDB does not know; a run of them is a broken token or a rate
        // limit, and grinding through the rest of the library would only make it worse.
        if (summary.failed >= 5) {
          BOOST_LOG(warning) << "Metadata resolve: giving up after " << summary.failed
                             << " failures, last was: " << summary.error;
          break;
        }
        continue;
      }
      if (outcome.changed) {
        ++summary.matched;
        summary.changed = true;
      } else if (outcome.igdb_id.empty()) {
        ++summary.unmatched;
      }
    }
    return summary;
  }

  namespace {
    struct background_t {
      std::mutex mutex;
      std::condition_variable wake;
      bool worker_running {false};
      bool pending {false};
      // A queued pass re-fetches everything because someone asked it to, not because a game
      // appeared. Sticky until the pass that honours it starts.
      bool pending_force {false};
      bool stopping {false};
      // Set while the worker writes apps.json, so its own write does not ask for another pass.
      bool writing {false};
    };

    background_t &background() {
      static background_t instance;
      return instance;
    }

    // Long enough that a library sync writing apps.json several times in a row still costs one
    // pass, short enough that a game added by hand is described before the user looks away.
    constexpr std::chrono::seconds k_debounce {5};

    nlohmann::json read_apps_file() {
      return nlohmann::json::parse(file_handler::read_file(config::stream.file_apps.c_str()), nullptr, false);
    }

    std::string node_key(const nlohmann::json &app) {
      if (auto it = app.find("uuid"); it != app.end() && it->is_string()) {
        return it->get<std::string>();
      }
      if (auto it = app.find("id"); it != app.end() && it->is_string()) {
        return it->get<std::string>();
      }
      return {};
    }

    /**
     * @brief Run one pass without holding the apps file across the network.
     *
     * Resolving takes as long as IGDB takes, which is far too long to hold a lock every app
     * endpoint needs. So the pass works on a copy, then re-reads the file and copies the
     * results onto whatever is there now -- an app the user deleted or renamed meanwhile keeps
     * the user's version, and only metadata is carried over.
     */
    void run_pass(bool force) {
      nlohmann::json snapshot;
      {
        std::lock_guard apps_lock {confighttp::apps_file_mutex()};
        snapshot = read_apps_file();
      }
      if (!snapshot.is_object()) {
        return;
      }
      const auto summary = resolve_all(snapshot, force);
      if (!summary.changed) {
        return;
      }
      BOOST_LOG(info) << "Metadata resolve: matched " << summary.matched << " of "
                      << summary.considered << " apps (" << summary.unmatched << " without a match)";

      std::lock_guard apps_lock {confighttp::apps_file_mutex()};
      auto current = read_apps_file();
      if (!current.is_object() || !current.contains("apps") || !current["apps"].is_array()) {
        return;
      }
      bool changed = false;
      for (auto &app : current["apps"]) {
        if (!app.is_object()) {
          continue;
        }
        const auto key = node_key(app);
        if (key.empty()) {
          continue;
        }
        for (const auto &resolved : snapshot["apps"]) {
          if (!resolved.is_object() || node_key(resolved) != key) {
            continue;
          }
          const auto meta = read_from_app(resolved);
          if (meta.source != "igdb") {
            break;
          }
          write_to_app(app, meta);
          // The cover follows the record only where the resolver was allowed to set one.
          if (auto it = resolved.find("image-path"); it != resolved.end() && it->is_string() &&
                                                     cover_is_ours_to_set(app)) {
            app["image-path"] = *it;
          }
          changed = true;
          break;
        }
      }
      if (!changed) {
        return;
      }
      auto &state = background();
      {
        std::lock_guard lock {state.mutex};
        state.writing = true;
      }
      confighttp::refresh_client_apps_cache(current, false);
      {
        std::lock_guard lock {state.mutex};
        state.writing = false;
      }
    }
  }  // namespace

  namespace {
    /// @brief Queue a pass and make sure a worker is alive to run it. Caller holds nothing.
    void queue_pass(bool force) {
      auto &state = background();
      std::unique_lock lock {state.mutex};
      if (state.stopping || state.writing) {
        return;
      }
      state.pending = true;
      state.pending_force = state.pending_force || force;
      if (state.worker_running) {
        state.wake.notify_all();
        return;
      }
      state.worker_running = true;
      std::thread([]() {
        auto &state = background();
        for (;;) {
          bool force = false;
          {
            std::unique_lock lock {state.mutex};
            // Every request that lands during the wait folds into this one pass.
            state.wake.wait_for(lock, k_debounce, [&state]() {
              return state.stopping;
            });
            if (state.stopping) {
              state.worker_running = false;
              state.pending = false;
              return;
            }
            if (!state.pending) {
              state.worker_running = false;
              return;
            }
            state.pending = false;
            force = state.pending_force;
            state.pending_force = false;
          }
          try {
            run_pass(force);
          } catch (const std::exception &e) {
            BOOST_LOG(warning) << "Metadata resolve: pass failed: " << e.what();
          } catch (...) {
            BOOST_LOG(warning) << "Metadata resolve: pass failed";
          }
        }
      }).detach();
    }
  }  // namespace

  void schedule_background_resolve() {
    if (!config::igdb.enabled || !config::igdb.auto_resolve) {
      return;
    }
    queue_pass(false);
  }

  void request_library_pass(bool force) {
    if (!config::igdb.enabled) {
      return;
    }
    queue_pass(force);
  }

  bool background_pass_running() {
    auto &state = background();
    std::scoped_lock lock {state.mutex};
    return state.worker_running;
  }

  void stop_background_resolve() {
    auto &state = background();
    std::unique_lock lock {state.mutex};
    state.stopping = true;
    state.pending = false;
    state.wake.notify_all();
  }

}  // namespace metadata::resolver
