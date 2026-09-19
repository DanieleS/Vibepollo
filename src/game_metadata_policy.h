/**
 * @file src/game_metadata_policy.h
 * @brief Provider-neutral game metadata stored on apps.json entries, plus the pure
 *        matching and parsing rules for the IGDB mirror (LizardByte GameDB).
 *
 * Every provider (the Playnite sync on Windows, the IGDB bulk fetch anywhere) writes the
 * same `metadata-*` keys, so clients and the streaming server never care where a value came
 * from. `metadata-source` records the provider that last wrote the descriptive fields.
 *
 * Nothing in this file touches the network, the filesystem or process-global configuration,
 * so it is covered by a self-contained component test.
 */
#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace game_metadata {

  namespace keys {
    constexpr const char *source = "metadata-source";
    constexpr const char *igdb_id = "metadata-igdb-id";
    constexpr const char *description = "metadata-description";
    constexpr const char *genres = "metadata-genres";
    constexpr const char *developers = "metadata-developers";
    constexpr const char *publishers = "metadata-publishers";
    constexpr const char *release_date = "metadata-release-date";
    constexpr const char *community_score = "metadata-community-score";
    constexpr const char *critic_score = "metadata-critic-score";
    constexpr const char *last_played = "metadata-last-played";
    constexpr const char *playtime_minutes = "metadata-playtime-minutes";
    constexpr const char *background = "metadata-background";
  }  // namespace keys

  // Values of `metadata-source`.
  constexpr const char *source_playnite = "playnite";
  constexpr const char *source_igdb = "igdb";

  /**
   * @brief Metadata carried by an app, as read from apps.json.
   *
   * Absent values stay empty or -1; `present` is set when the app carries any value at all.
   */
  struct metadata_t {
    bool present {false};
    std::string source;  ///< Provider that wrote the descriptive fields ("playnite", "igdb", or empty).
    std::string igdb_id;  ///< IGDB game id when the descriptive fields came from IGDB.
    std::string description;
    std::vector<std::string> genres;
    std::vector<std::string> developers;
    std::vector<std::string> publishers;
    std::string release_date;
    int community_score {-1};
    int critic_score {-1};
    // Activity data. Only Playnite can supply these; they are kept apart from the descriptive
    // fields so disabling Playnite metadata does not lose them.
    std::string last_played;
    uint64_t playtime_minutes {0};
    // Local path to a PNG background/hero image. Empty when the game has none.
    std::string background_image_path;
  };

  /**
   * @brief Descriptive fields a provider supplies for a game.
   */
  struct descriptive_t {
    std::string description;
    std::vector<std::string> genres;
    std::vector<std::string> developers;
    std::vector<std::string> publishers;
    std::string release_date;
    int community_score {-1};
    int critic_score {-1};
    std::string background_path;
  };

  /**
   * @brief Read the metadata of an app node.
   *
   * Entries written before the keys became provider-neutral used a `playnite-*` prefix; those
   * are still read when the `metadata-*` key is absent, and are attributed to Playnite.
   */
  metadata_t read_app_metadata(const nlohmann::json &app);

  /**
   * @brief Write the descriptive fields of a provider onto an app node.
   *
   * Every key is set when it has a value and erased otherwise, so apps.json stays tidy and a
   * repeated write is idempotent. Legacy `playnite-*` descriptive keys are erased.
   * @param igdb_id Recorded as `metadata-igdb-id`; erased when empty.
   */
  void write_descriptive(nlohmann::json &app, const descriptive_t &fields, std::string_view source, std::string_view igdb_id = {});

  /**
   * @brief Write the activity fields. Zero playtime and an empty timestamp erase their keys.
   */
  void write_activity(nlohmann::json &app, const std::string &last_played, uint64_t playtime_minutes);

  /// True when the node carries any descriptive metadata (from any provider).
  bool has_descriptive_metadata(const nlohmann::json &app);

  // ---------------------------------------------------------------------------------------
  // IGDB mirror (LizardByte GameDB): https://raw.githubusercontent.com/LizardByte/GameDB
  // ---------------------------------------------------------------------------------------

  constexpr const char *gamedb_base_url = "https://raw.githubusercontent.com/LizardByte/GameDB/gh-pages";
  constexpr const char *igdb_image_base_url = "https://images.igdb.com/igdb/image/upload";
  // IGDB image size presets used for the artwork this module stores.
  constexpr const char *igdb_cover_size = "t_cover_big_2x";
  constexpr const char *igdb_background_size = "t_1080p";

  /**
   * @brief Bucket file a title is indexed under: its first two characters, lower-cased, with
   *        anything outside [a-z0-9] dropped; "@" when nothing is left. Mirrors the Web UI
   *        cover search so both look in the same place.
   */
  std::string bucket_key(std::string_view title);

  /// URL of a bucket index file.
  std::string bucket_url(std::string_view key);
  /// URL of a game record.
  std::string game_url(std::string_view id);
  /// URL of an image on images.igdb.com as PNG at the given size preset.
  std::string image_url(std::string_view size, std::string_view slug);

  /**
   * @brief Title comparison form: lower-cased ASCII letters and digits only.
   *
   * Punctuation, spacing, trademark symbols and other non-alphanumeric bytes are dropped, so
   * "DOOM: Eternal", "Doom Eternal" and "DOOM Eternal™" compare equal.
   */
  std::string normalize_title(std::string_view title);

  /// A title with a trailing parenthetical removed ("Prey (2017)" -> "Prey"); unchanged otherwise.
  std::string strip_trailing_parenthetical(std::string_view title);

  struct bucket_entry_t {
    std::string id;
    std::string name;
  };

  /// Parse a bucket index ({"<id>": {"name": "..."}, ...}). Entries without a name are skipped.
  std::vector<bucket_entry_t> parse_bucket(const nlohmann::json &bucket);

  enum class match_kind_e {
    none,  ///< No candidate; the app is reported as unmatched rather than guessed.
    exact,  ///< A candidate whose normalized name equals the normalized title.
    unique_prefix,  ///< Exactly one candidate whose normalized name starts with the title.
  };

  struct match_t {
    match_kind_e kind {match_kind_e::none};
    std::string id;
    std::string name;
    // Number of candidates that matched exactly. More than one means the pick was a tie-break
    // (a case-sensitive exact name first, else the lowest id) and deserves a look.
    std::size_t exact_candidates {0};
  };

  /**
   * @brief Pick the bucket entry for a title.
   *
   * Exact normalized matches win; a trailing parenthetical is dropped and retried; finally a
   * single candidate that starts with the title is accepted. Anything else is `none`.
   */
  match_t select_match(std::string_view title, const std::vector<bucket_entry_t> &entries);

  struct igdb_game_t {
    std::string id;
    std::string name;
    std::string summary;
    std::vector<std::string> genres;
    std::vector<std::string> developers;
    std::vector<std::string> publishers;
    std::string release_date;  ///< Earliest release as YYYY-MM-DD, or YYYY when only a year is known.
    int community_score {-1};  ///< IGDB user rating, rounded to 0-100.
    int critic_score {-1};  ///< IGDB aggregated critic rating, rounded to 0-100.
    std::string cover_slug;  ///< Image id of the cover, e.g. "co2lbd".
    std::string artwork_slug;  ///< Image id of the first artwork, or first screenshot when no artwork exists.
  };

  /// Parse a GameDB game record. Returns nullopt when the record has no id or name.
  std::optional<igdb_game_t> parse_game(const nlohmann::json &game);

  /// Slug of an IGDB image URL ("//images.igdb.com/igdb/image/upload/t_thumb/co2lbd.jpg" -> "co2lbd").
  std::string image_slug(std::string_view url);

  /// Format a Unix timestamp (UTC) as YYYY-MM-DD.
  std::string format_release_date(std::int64_t unix_seconds);

  /// Map an IGDB record to the descriptive fields stored on an app.
  descriptive_t to_descriptive(const igdb_game_t &game, std::string background_path);

}  // namespace game_metadata
