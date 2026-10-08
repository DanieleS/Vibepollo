/**
 * @file src/play_stats.h
 * @brief How much is played, by day, week, month and year: the pure half of `/appstats`.
 *
 * A port of CouchPilot's StatsService. The Playnite connector appends one line per game session
 * to a log (Playnite itself keeps only totals, so the history starts the day the connector learns
 * to write it); this file reads that log and adds it up, together with the library totals Playnite
 * reports. Nothing here touches Playnite, the file system or the clock: the caller hands in the
 * sessions, the games, what is running and when "now" is, which is what lets the tests pin a time
 * zone and a date.
 *
 * Everything is in the host's local time, and a day of play runs from 5:00 to 5:00: a session from
 * 23:00 to 1:00 is all the evening's.
 */
#pragma once

// standard includes
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

namespace play_stats {

  /**
   * @brief Maps a UTC instant (Unix seconds) to the local wall-clock time, expressed as seconds
   * since 1970-01-01 00:00 *local*.
   *
   * Injected so the tests can pin a zone; production uses the OS zone (os_utc_to_local).
   */
  using to_local_t = std::function<std::int64_t(std::int64_t)>;

  /// @brief The OS local zone, DST included.
  std::int64_t os_utc_to_local(std::int64_t utc);

  /// @brief The inverse of a to_local_t, good across DST changes except inside the skipped hour.
  std::int64_t local_to_utc(std::int64_t local, const to_local_t &to_local);

  /**
   * @brief Parse an ISO 8601 date-time, as .NET's "o" format and Newtonsoft write them.
   *
   * Fractions of a second are dropped. `Z` and `+hh:mm` offsets are honoured; a time without
   * either is a local time, which is how .NET writes a DateTime of unspecified kind.
   * @return Unix seconds, or nullopt when the text is not a date-time.
   */
  std::optional<std::int64_t> parse_iso8601(std::string_view text, const to_local_t &to_local);

  /// @brief The year as written in an ISO 8601 date, before any zone conversion; 0 when unreadable.
  int iso8601_year(std::string_view text);

  /// @brief `YYYY-MM-DDTHH:MM:SSZ`.
  std::string format_utc(std::int64_t utc);

  /// @brief `YYYY-MM-DD` for a day number (days since 1970-01-01).
  std::string format_day(std::int64_t day);

  /// @brief The day of play a local time belongs to: days since 1970-01-01, cut at 5:00.
  std::int64_t day_of(std::int64_t local);

  /// @brief A Playnite id as it is compared: lowercase, without braces or surrounding blanks.
  std::string normalize_id(std::string_view id);

  /**
   * @brief One game session from the connector's log.
   *
   * The log's lines are CouchPilot's: `{"Game":"<guid>","Start":"<ISO 8601 UTC>","Seconds":N}`.
   */
  struct session_t {
    std::string game;  ///< Normalized Playnite id.
    std::int64_t start {0};  ///< Unix seconds.
    std::int64_t seconds {0};  ///< Playnite's own count of the session's length.
  };

  /**
   * @brief Read the session log. Blank lines, lines cut short by a crash mid-write and sessions
   * of zero seconds or less are skipped; the rest are kept in file order.
   */
  std::vector<session_t> parse_session_log(std::string_view text, const to_local_t &to_local = os_utc_to_local);

  /// @brief What the stats need of one Playnite game: its totals, as Playnite keeps them.
  struct game_t {
    std::string id;  ///< Normalized Playnite id.
    std::string name;
    std::int64_t playtime_seconds {0};
    std::int64_t play_count {0};
    std::optional<std::int64_t> last_activity;  ///< Unix seconds; nullopt when never played.
    bool installed {false};
    bool hidden {false};
  };

  /// @brief A game being played right now, and since when.
  struct running_t {
    std::string game;  ///< Normalized Playnite id.
    std::int64_t started_at {0};  ///< Unix seconds.
  };

  /// @brief Everything the stats are computed from.
  struct input_t {
    std::vector<session_t> sessions;
    std::vector<game_t> games;  ///< The whole Playnite library, hidden games included.
    std::vector<running_t> running;
    std::int64_t now {0};  ///< Unix seconds.
    to_local_t to_local {os_utc_to_local};
  };

  /// @brief How a Playnite game appears to the calling client.
  struct catalogue_entry_t {
    std::string uuid;  ///< The app's UUID.
    std::string name;  ///< The app's name, as /applist shows it.
  };

  /// @brief The caller's catalogue, keyed by normalized Playnite id. Games outside it are counted
  /// in totals but never named.
  using catalogue_t = std::unordered_map<std::string, catalogue_entry_t>;

  /// @brief A period of the overview, in days of play.
  struct period_t {
    std::string range;  ///< week, month or year.
    int offset {0};
    std::int64_t today {0};
    std::int64_t from {0};  ///< First day.
    std::int64_t to {0};  ///< Day after the last.
    std::int64_t previous_from {0};
    std::int64_t previous_to {0};  ///< Cut to the same point as `to` while the period is under way.
    std::int64_t from_utc {0};  ///< When `from` starts (5:00 local), as Unix seconds.
    std::int64_t to_utc {0};  ///< When `to` starts, as Unix seconds.
  };

  /**
   * @brief The period `offset` steps back from the current week, month or year.
   *
   * An unknown range is a week. The previous period is compared up to the same point while the
   * current one is under way: the first 5 days of last month for the 5th of this one.
   */
  period_t period(std::string_view range, int offset, std::int64_t now, const to_local_t &to_local);

  /// @brief `offset` as the endpoint takes it: an integer from -500 to 0, anything else is 0.
  int parse_offset(std::string_view text);

  /**
   * @brief The achievements part of the overview for a period: `null`, or `{unlocked, recent}`.
   * Receives the period's bounds as Unix seconds, `to` exclusive.
   */
  using achievements_fn = std::function<nlohmann::json(std::int64_t from_utc, std::int64_t to_utc)>;

  /**
   * @brief The `/appstats?range=&offset=` body.
   *
   * `top` and `resume` list only games in @p catalogue (then take their 5 and 6); totals,
   * buckets, sessions and `library` count everything.
   */
  nlohmann::json overview(const input_t &input, std::string_view range, int offset, const catalogue_t &catalogue, const achievements_fn &achievements = {});

  /**
   * @brief The `/appstats?appuuid=` body for one game, or nullopt when Playnite doesn't know it.
   * @param playnite_id The game's Playnite id (any case).
   * @param uuid The app's UUID, echoed back.
   */
  std::optional<nlohmann::json> game(const input_t &input, std::string_view playnite_id, std::string_view uuid);

}  // namespace play_stats
