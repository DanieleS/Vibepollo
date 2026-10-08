/**
 * @file src/successstory.h
 * @brief Achievements, read from SuccessStory's data: the pure half of `/appachievements`.
 *
 * SuccessStory (Lacro59's Playnite extension) fetches achievements from Steam, Epic, GOG, Xbox,
 * RetroAchievements and more and keeps one JSON file per game in its data folder. Vibepollo only
 * reads those files, so it has nothing to log into and nothing to fetch; without SuccessStory
 * there are no achievements, and the endpoints say so with a 404.
 *
 * A port of CouchPilot's AchievementsService, minus the folder watcher: clients poll, so a cheap
 * check of each file's modification time when it is asked for is enough.
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

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "play_stats.h"

namespace successstory {

  /// @brief One achievement, the parts of SuccessStory's item that are read.
  struct achievement_t {
    std::string name;
    std::string api_name;  ///< The source's own name for it (Steam's API name, say); may be empty.
    std::string description;
    std::string url_unlocked;
    std::string url_locked;
    bool unlocked {false};  ///< DateUnlocked is set to a real year.
    /// When it was unlocked (Unix seconds), or nullopt: still locked, or the source doesn't say
    /// when. SuccessStory itself writes 1982 for "unlocked, no idea when".
    std::optional<std::int64_t> unlocked_at;
    bool hidden {false};
    float percent {100};  ///< How many players have it, 0-100; 100 is also "unknown".
    float gamer_score {0};
    bool no_rarety {false};  ///< SuccessStory's spelling: the source gives no rarity.
  };

  /// @brief One game's file.
  struct game_t {
    std::string id;  ///< Normalized Playnite id.
    std::string name;
    std::vector<achievement_t> items;
    std::optional<std::int64_t> last_refresh;  ///< Unix seconds.
    bool ignored {false};  ///< The user told SuccessStory to ignore the game.
  };

  /**
   * @brief Parse one of SuccessStory's files.
   * @param text The file's content (a UTF-8 BOM is fine).
   * @param fallback_id The id to use when the file's own `Id` is missing or empty: its name.
   * @return nullopt when it isn't one of SuccessStory's game files, or it is cut short mid-write.
   */
  std::optional<game_t> parse_game(std::string_view text, std::string_view fallback_id, const play_stats::to_local_t &to_local = play_stats::os_utc_to_local);

  /**
   * @brief Where a client fetches an icon: the web URL as it is, or `/appachievementicon?...` for
   * one SuccessStory keeps on disk. Empty for no icon.
   */
  std::string icon_url(std::string_view url, std::string_view uuid, std::size_t index, bool locked);

  /**
   * @brief One achievement as the endpoints show it:
   * `{id,name,description,unlocked,unlocked_at,hidden,percent,gamer_score,icon,locked_icon}`.
   */
  nlohmann::json item_json(const achievement_t &a, std::size_t index, std::string_view uuid);

  /**
   * @brief The `/appachievements` body: `{uuid,total,unlocked,last_refresh,items}`, unlocked
   * ones first, newest first, then the locked ones, the most common first.
   * @return nullopt when the game has no achievements or is ignored (404).
   */
  std::optional<nlohmann::json> game_json(const game_t &game, std::string_view uuid);

  /// @brief An unlock with a known date, and the game it belongs to.
  struct unlock_t {
    std::shared_ptr<const game_t> game;
    std::size_t index {0};
    std::int64_t when {0};  ///< Unix seconds.
  };

  /**
   * @brief Unlocks as the endpoints list them: item + `uuid` + `game`, only for games in the
   * caller's catalogue, at most @p limit, in the order given (newest first).
   */
  nlohmann::json unlocks_json(const std::vector<unlock_t> &unlocks, const play_stats::catalogue_t &catalogue, std::size_t limit);

  /**
   * @brief Resolve a local icon path under one of the allowed roots.
   *
   * A relative path is taken from each root in turn, an absolute one as it is; either way the
   * result must lie inside that root (no `..` escapes, no symlink escapes) and be a regular file.
   * @return nullopt for a web URL, an empty one, or anything outside the roots.
   */
  std::optional<std::filesystem::path> resolve_icon(std::string_view url, const std::vector<std::filesystem::path> &roots);

  /// @brief A path from UTF-8 text, as the connector and SuccessStory write them.
  std::filesystem::path utf8_path(std::string_view text);

  /// @brief A path as UTF-8 text, whatever the OS's own encoding.
  std::string utf8_string(const std::filesystem::path &path);

  /// @brief The Content-Type for an icon file, by its extension.
  std::string content_type_for(const std::filesystem::path &path);

  /**
   * @brief SuccessStory's files, cached per file by modification time.
   *
   * Thread-safe. The whole folder is looked at again at most every @p rescan_interval, for the
   * queries that span every game; one game's file is checked each time it is asked for.
   */
  class store_t {
  public:
    /**
     * @param data_dir SuccessStory's data folder (`<ExtensionsData>\cebe6d32-...`); the files
     *        are in its `SuccessStory` subfolder.
     * @param resources_dir The `Resources` folder next to SuccessStory's assembly, or empty.
     */
    store_t(std::filesystem::path data_dir, std::filesystem::path resources_dir, play_stats::to_local_t to_local = play_stats::os_utc_to_local, std::chrono::steady_clock::duration rescan_interval = std::chrono::seconds(5));

    const std::filesystem::path &data_dir() const {
      return data_dir_;
    }

    const std::filesystem::path &resources_dir() const {
      return resources_dir_;
    }

    /// @brief SuccessStory has written something: there are achievements to show.
    bool available();

    /// @brief One game's file, or null when SuccessStory has none.
    std::shared_ptr<const game_t> game(std::string_view playnite_id);

    /// @brief Unlocked between two moments (`to` exclusive), every game but ignored ones, newest first.
    std::vector<unlock_t> unlocked(std::int64_t from_utc, std::int64_t to_utc);

    /// @brief The file of an icon SuccessStory keeps on disk, or nullopt (web icons included).
    std::optional<std::filesystem::path> icon_path(std::string_view playnite_id, std::size_t index, bool locked);

  private:
    struct cached_t {
      std::filesystem::file_time_type written {};
      std::uintmax_t size {0};
      std::shared_ptr<const game_t> data;
    };

    std::filesystem::path folder() const;
    std::shared_ptr<const game_t> load_locked(const std::string &id, const std::filesystem::path &path);
    void rescan_locked();

    std::filesystem::path data_dir_;
    std::filesystem::path resources_dir_;
    play_stats::to_local_t to_local_;
    std::chrono::steady_clock::duration rescan_interval_;
    std::mutex mutex_;
    std::unordered_map<std::string, cached_t> cache_;
    bool scanned_ {false};
    bool any_files_ {false};
    std::chrono::steady_clock::time_point scanned_at_ {};
  };

}  // namespace successstory
