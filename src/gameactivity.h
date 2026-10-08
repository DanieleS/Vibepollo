/**
 * @file src/gameactivity.h
 * @brief Play sessions, read from GameActivity's data: where `/appstats` gets its history.
 *
 * Playnite keeps only totals. GameActivity (Lacro59's Playnite extension, by SuccessStory's author)
 * records every session, and keeps one JSON file per game in its data folder,
 * `<ExtensionsData>\afbb1a0d-04a1-4d0c-9afa-c6e42ca855b4\GameActivity\<PlayniteGameId>.json`.
 * Vibepollo only reads those files, as it reads SuccessStory's; without GameActivity there is no
 * history, and the statistics say so.
 *
 * The parts read are the file's `Id` and its `Items`, one per session:
 * `{"DateSession":"2024-03-11T18:00:00.1234567Z","ElapsedSeconds":3600,...}`. `DateSession` is the
 * session's start, taken as `DateTime.Now.ToUniversalTime()` and written by Newtonsoft with a `Z`;
 * a value without one (a file written from a DateTime of unspecified kind) is UTC all the same,
 * which is how GameActivity itself reads it. Everything else (the source, platforms, action name,
 * configuration, hardware samples, and the legacy `ItemsDetails` block) is ignored.
 *
 * A session's item is added when the game starts and its length filled in when it stops, so an
 * item of 0 seconds is a run still under way (or one whose stop was lost): it is skipped, and the
 * running game is counted from Playnite's start instead (see play_stats). GameActivity's "ignore
 * sessions shorter than" setting only filters its own views; every session longer than 0 counts.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// local includes
#include "play_stats.h"

namespace gameactivity {

  /// @brief GameActivity's Playnite plugin id: its data folder's name under ExtensionsData.
  inline constexpr std::string_view k_plugin_id = "afbb1a0d-04a1-4d0c-9afa-c6e42ca855b4";

  /**
   * @brief Parse one of GameActivity's files.
   * @param text The file's content (a UTF-8 BOM is fine).
   * @param fallback_id The id to use when the file's own `Id` is missing or empty: its name.
   * @return The sessions longer than 0 seconds, in file order; nullopt when it isn't one of
   *         GameActivity's files, or it is cut short mid-write.
   */
  std::optional<std::vector<play_stats::session_t>> parse_game(std::string_view text, std::string_view fallback_id);

  /**
   * @brief GameActivity's data folder: the one the connector reported or, from a connector that
   * predates it (and only reports SuccessStory's), its sibling under Playnite's ExtensionsData.
   * @param reported The connector's `gameActivityData`, UTF-8; may be empty.
   * @param success_story_data The connector's `successStoryData`, UTF-8; may be empty.
   * @return Empty when neither is known.
   */
  std::filesystem::path resolve_data_dir(std::string_view reported, std::string_view success_story_data);

  /**
   * @brief GameActivity has written something under @p data_dir: at least one game file in its
   * `GameActivity` subfolder. The same test the store's `found` makes, without reading any file.
   */
  bool data_found(const std::filesystem::path &data_dir);

  /// @brief Every session GameActivity has, and whether it has anything at all.
  struct snapshot_t {
    /// GameActivity's folder holds at least one game file. False: not installed (or never used),
    /// and there is no history to show.
    bool found {false};
    std::vector<play_stats::session_t> sessions;
  };

  /**
   * @brief GameActivity's files, cached per file by modification time.
   *
   * Thread-safe. The folder is looked at again at most every @p rescan_interval; between scans the
   * last result is served. Clients poll, so there is no watcher.
   */
  class store_t {
  public:
    /**
     * @param data_dir GameActivity's data folder (`<ExtensionsData>\afbb1a0d-...`); the files are
     *        in its `GameActivity` subfolder. Its `SaveSession_*.json` running-session backups,
     *        next to that subfolder, are not read.
     */
    explicit store_t(std::filesystem::path data_dir, std::chrono::steady_clock::duration rescan_interval = std::chrono::seconds(5));

    const std::filesystem::path &data_dir() const {
      return data_dir_;
    }

    /// @brief The sessions of every game, from the files as they are now (or a few seconds ago).
    snapshot_t snapshot();

  private:
    struct cached_t {
      std::filesystem::file_time_type written {};
      std::uintmax_t size {0};
      std::shared_ptr<const std::vector<play_stats::session_t>> sessions;
    };

    void rescan_locked();

    std::filesystem::path data_dir_;
    std::chrono::steady_clock::duration rescan_interval_;
    std::mutex mutex_;
    std::unordered_map<std::string, cached_t> cache_;
    bool scanned_ {false};
    bool found_ {false};
    std::chrono::steady_clock::time_point scanned_at_ {};
  };

}  // namespace gameactivity
