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
#include "remote_session.h"

// standard includes
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>

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

    /**
     * @brief Art an earlier build saved as IGDB served it, JPEG.
     *
     * Clients are only ever handed PNG, so no client could show these files. They are treated
     * as not set, which lets the next resolve replace them with converted copies.
     */
    bool is_legacy_igdb_art(const std::string &path) {
      if (path.empty()) {
        return false;
      }
      const std::filesystem::path file {path};
      return file.extension() == ".jpg" && file.filename().string().rfind("igdb_", 0) == 0;
    }

    std::string image_path_of(const nlohmann::json &app) {
      if (auto it = app.find("image-path"); it != app.end() && it->is_string()) {
        return it->get<std::string>();
      }
      return {};
    }

    bool has_legacy_igdb_art(const nlohmann::json &app) {
      return is_legacy_igdb_art(image_path_of(app)) ||
             is_legacy_igdb_art(read_from_app(app).background_image_path);
    }

    bool cover_is_ours_to_set(const nlohmann::json &app) {
      auto it = app.find("image-path");
      if (it == app.end() || !it->is_string()) {
        return true;
      }
      const auto path = it->get<std::string>();
      return path.empty() || path == k_placeholder_cover || is_legacy_igdb_art(path);
    }

    /**
     * @brief Swap legacy JPEG art for converted copies and change nothing else.
     *
     * Used for apps the resolver would otherwise leave alone, a locked one included: the user
     * chose the record, not the file format, so replacing art they could never see is not
     * overriding them.
     */
    bool replace_legacy_art(nlohmann::json &app, const igdb::policy::game_t &record) {
      bool changed = false;
      auto meta = read_from_app(app);
      if (is_legacy_igdb_art(meta.background_image_path)) {
        meta.background_image_path = igdb::download_background(record, covers_root());
        meta.present = has_any_value(meta);
        write_to_app(app, meta);
        changed = true;
      }
      if (is_legacy_igdb_art(image_path_of(app))) {
        const auto cover = igdb::download_cover(record, covers_root());
        if (cover.empty()) {
          app.erase("image-path");
        } else {
          app["image-path"] = cover;
        }
        changed = true;
      }
      return changed;
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
      merged.background_image_path = is_legacy_igdb_art(before.background_image_path) ?
                                       std::string {} :
                                       before.background_image_path;

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

    // An app the resolver otherwise leaves alone still gets its old JPEG art replaced, once.
    const auto refresh_legacy_art = [&app, &existing, &outcome]() {
      if (existing.igdb_id.empty() || !has_legacy_igdb_art(app)) {
        return;
      }
      std::string error;
      if (const auto record = igdb::fetch_game(existing.igdb_id, error)) {
        outcome.changed = replace_legacy_art(app, *record);
      }
    };

    if (existing.locked && !force) {
      outcome.skipped_locked = true;
      refresh_legacy_art();
      return outcome;
    }
    // Already described by IGDB and not asked to refresh: nothing to do and no request spent.
    if (!force && existing.source == "igdb" && existing.present) {
      refresh_legacy_art();
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

  namespace {
    /// @brief Whether a stop was asked for. Defined with the worker's state below.
    bool stop_requested();

    /**
     * @brief Whether an entry is a game at all.
     *
     * Desktop launches nothing of its own, and the remote-session entries stand for host
     * controls. Neither has anything to look up, and searching IGDB for "Desktop" on every
     * pass only spends requests on a guaranteed miss or, worse, a wrong match.
     */
    bool is_game_entry(const nlohmann::json &app) {
      if (auto it = app.find("uuid"); it != app.end() && it->is_string() &&
                                      remote_session::identify(0, std::string_view {it->get_ref<const std::string &>()}) != remote_session::control_e::none) {
        return false;
      }
      if (remote_session::reserved_name(app_name(app))) {
        return false;
      }
      const auto launches = [&app](const char *key) {
        auto it = app.find(key);
        if (it == app.end()) {
          return false;
        }
        if (it->is_string()) {
          return !it->get_ref<const std::string &>().empty();
        }
        return it->is_array() && !it->empty();
      };
      return launches("cmd") || launches("detached");
    }

    /**
     * @brief Apps the background pass recently looked up and found nothing for.
     *
     * Without this every write to apps.json queued a pass that searched IGDB again for every
     * game it had just failed to match. Keyed by everything the lookup used, so renaming an
     * app, a new store id or turning name matching on retries at once. Only automatic passes
     * consult it; a pass the user asks for clears it.
     */
    struct unmatched_t {
      std::mutex mutex;
      std::unordered_map<std::string, std::chrono::steady_clock::time_point> until;
    };

    unmatched_t &unmatched() {
      static unmatched_t instance;
      return instance;
    }

    constexpr std::chrono::hours k_unmatched_ttl {24};

    std::string lookup_key(const nlohmann::json &app) {
      std::string key = config::igdb.allow_name_match ? "name:" : "ids:";
      key += app_name(app);
      for (const auto &id : store_ids_of(app)) {
        key += '\n';
        key += id.store;
        key += ':';
        key += id.id;
      }
      return key;
    }

    bool recently_unmatched(const std::string &key) {
      auto &cache = unmatched();
      std::scoped_lock lock {cache.mutex};
      const auto found = cache.until.find(key);
      if (found == cache.until.end()) {
        return false;
      }
      if (found->second <= std::chrono::steady_clock::now()) {
        cache.until.erase(found);
        return false;
      }
      return true;
    }

    void remember_unmatched(const std::string &key) {
      auto &cache = unmatched();
      std::scoped_lock lock {cache.mutex};
      const auto now = std::chrono::steady_clock::now();
      // Entries for apps that were renamed or removed would otherwise stay forever.
      if (cache.until.size() > 4096) {
        std::erase_if(cache.until, [now](const auto &entry) {
          return entry.second <= now;
        });
      }
      cache.until[key] = now + k_unmatched_ttl;
    }

    void forget_unmatched() {
      auto &cache = unmatched();
      std::scoped_lock lock {cache.mutex};
      cache.until.clear();
    }

    /**
     * @brief The loop behind resolve_all.
     *
     * `automatic` is set for the background pass: it skips entries that are not games, skips
     * apps it failed to match within the last day, and stops between apps once shutdown asks.
     */
    summary_t resolve_library(nlohmann::json &root, bool force, bool automatic) {
      summary_t summary;
      auto apps = root.find("apps");
      if (apps == root.end() || !apps->is_array()) {
        return summary;
      }
      for (auto &app : *apps) {
        if (!app.is_object()) {
          continue;
        }
        if (automatic && stop_requested()) {
          break;
        }
        std::string key;
        if (automatic && read_from_app(app).igdb_id.empty()) {
          if (!is_game_entry(app)) {
            continue;
          }
          key = lookup_key(app);
          if (!force && recently_unmatched(key)) {
            ++summary.considered;
            ++summary.unmatched;
            continue;
          }
        }
        ++summary.considered;
        const auto outcome = resolve_app(app, force);
        if (outcome.skipped_locked) {
          ++summary.skipped;
          // Skipped for its metadata, but it may still have had old art replaced.
          summary.changed = summary.changed || outcome.changed;
          continue;
        }
        if (!outcome.error.empty()) {
          ++summary.failed;
          if (summary.error.empty()) {
            summary.error = outcome.error;
          }
          // One failure is a game IGDB does not know; a run of them is a broken token or a
          // rate limit, and grinding through the rest of the library would only make it worse.
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
          // A clean miss, not an error: an error says nothing about whether a match exists.
          if (!key.empty()) {
            remember_unmatched(key);
          }
        }
      }
      return summary;
    }
  }  // namespace

  summary_t resolve_all(nlohmann::json &root, bool force) {
    return resolve_library(root, force, false);
  }

  namespace {
    struct background_t {
      std::mutex mutex;
      std::condition_variable wake;
      // Signalled when the worker exits, for shutdown to wait on.
      std::condition_variable stopped;
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

    bool stop_requested() {
      auto &state = background();
      std::scoped_lock lock {state.mutex};
      return state.stopping;
    }

    // How long shutdown waits for a pass to reach a stopping point. A pass checks between
    // apps, so this normally ends within one request; a request stuck on a dead connection
    // must not hold shutdown for its full timeout.
    constexpr std::chrono::seconds k_stop_wait {10};

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
     * endpoint needs. So the pass works on a copy, then re-reads the file and carries over
     * only what it changed, through merge_resolved: an app the user deleted meanwhile stays
     * deleted, one the user edited or re-linked keeps the user's version, and playtime a
     * library sync wrote in the meantime is not rolled back to the copy's.
     */
    void run_pass(bool force) {
      nlohmann::json snapshot;
      {
        std::lock_guard apps_lock {confighttp::apps_file_mutex()};
        snapshot = read_apps_file();
      }
      if (!snapshot.is_object() || !snapshot.contains("apps") || !snapshot["apps"].is_array()) {
        return;
      }
      // What the pass started from, so the write-back can tell what the pass itself changed.
      const auto original = snapshot;
      const auto summary = resolve_library(snapshot, force, true);
      // A pass cut short by shutdown writes nothing: what it found is found again next start,
      // and writing apps.json while the process tears down is not worth the risk.
      if (!summary.changed || stop_requested()) {
        return;
      }
      BOOST_LOG(info) << "Metadata resolve: matched " << summary.matched << " of "
                      << summary.considered << " apps (" << summary.unmatched << " without a match)";

      std::lock_guard apps_lock {confighttp::apps_file_mutex()};
      auto current = read_apps_file();
      if (!current.is_object() || !current.contains("apps") || !current["apps"].is_array()) {
        return;
      }
      // resolve_all edits nodes in place and never reorders them, so the same index names the
      // same app before and after the pass.
      const auto &before_apps = original["apps"];
      const auto &after_apps = snapshot["apps"];
      bool changed = false;
      for (auto &app : current["apps"]) {
        if (!app.is_object()) {
          continue;
        }
        const auto key = node_key(app);
        if (key.empty()) {
          continue;
        }
        for (std::size_t index = 0; index < after_apps.size() && index < before_apps.size(); ++index) {
          if (node_key(after_apps[index]) != key) {
            continue;
          }
          if (before_apps[index] != after_apps[index]) {
            changed = merge_resolved(app, before_apps[index], after_apps[index]) || changed;
          }
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
              state.stopped.notify_all();
              return;
            }
            if (!state.pending) {
              state.worker_running = false;
              state.stopped.notify_all();
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
    // The user asked, so games that recently found no match are tried again too.
    forget_unmatched();
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
    state.stopped.wait_for(lock, k_stop_wait, [&state]() {
      return !state.worker_running;
    });
  }

}  // namespace metadata::resolver
