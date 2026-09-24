/**
 * @file src/metadata_resolver.h
 * @brief Fills app metadata from IGDB, whichever launcher the app came from.
 */
#pragma once

// standard includes
#include <cstddef>
#include <string>

// lib includes
#include <nlohmann/json.hpp>

namespace metadata::resolver {

  /// @brief What happened to one app node.
  struct outcome_t {
    bool changed {false};
    // Set when the app is now linked to an IGDB record, whether or not this call did the linking.
    std::string igdb_id;
    // Empty when the app simply has no match; set only when something actually went wrong.
    std::string error;
    // The app was left alone because the user had edited it by hand.
    bool skipped_locked {false};
  };

  /**
   * @brief Resolve one app node in place.
   *
   * Store ids come first: a Steam or GOG id is an exact answer, where a title is a guess.
   * Falls back to an exact-title search when the app has no id IGDB indexes and the user has
   * left name matching on.
   *
   * `force` re-fetches an app that already has metadata, and is the only way to touch one the
   * user has locked.
   */
  outcome_t resolve_app(nlohmann::json &app, bool force);

  /// @brief Link an app to an IGDB record the user picked, and mark it as theirs.
  bool apply_igdb_id(nlohmann::json &app, const std::string &igdb_id, std::string &error_out);

  struct summary_t {
    std::size_t considered {0};
    std::size_t matched {0};
    std::size_t unmatched {0};
    std::size_t skipped {0};
    std::size_t failed {0};
    bool changed {false};
    // The first error, if any, so a caller has something to show without a log dive.
    std::string error;
  };

  /**
   * @brief Resolve every app under root["apps"].
   *
   * Stops early and reports what it managed once IGDB starts refusing requests, rather than
   * spending a library's worth of them finding out the credentials are wrong.
   */
  summary_t resolve_all(nlohmann::json &root, bool force);

  /**
   * @brief Ask for a library-wide pass in the background, soon.
   *
   * Called from wherever apps.json is written, so a game that has just appeared in the library
   * gets described without the user going looking for a button. Repeated calls collapse into
   * one pass, the pass itself is debounced, and it does its network work without holding the
   * apps file, so a sync that adds forty games costs one pass and blocks nothing.
   *
   * Does nothing unless IGDB is both configured and set to resolve automatically. Entries that
   * are not games (Desktop, the remote-session controls) are skipped, and so, for a day, are
   * apps a pass already found no match for.
   */
  void schedule_background_resolve();

  /**
   * @brief Ask for a pass because the user pressed a button.
   *
   * Unlike the automatic trigger this runs even with automatic resolution turned off, and
   * `force` re-fetches apps that already carry metadata. Either way it retries the apps a
   * recent pass found no match for. It returns as soon as the pass is
   * queued: describing a library of a few hundred games takes minutes at IGDB's four requests
   * a second, which is far longer than an HTTP request should stay open.
   */
  void request_library_pass(bool force);

  /// @brief Whether a background pass is queued or running, for the UI to report.
  bool background_pass_running();

  /**
   * @brief Stop the background worker and wait for an in-flight pass. Called at shutdown.
   *
   * A pass checks for the stop between apps and then writes nothing, so the wait is normally
   * one IGDB request long. It is bounded at ten seconds all the same, so a request stuck on a
   * dead connection cannot hold shutdown; the worker then finishes on its own and exits.
   */
  void stop_background_resolve();

}  // namespace metadata::resolver
