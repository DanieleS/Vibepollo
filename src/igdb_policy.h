/**
 * @file src/igdb_policy.h
 * @brief Portable half of the IGDB integration: query bodies, response parsing, pacing.
 *
 * Nothing here touches the network, the clock or the disk, so the shape of every request we
 * send and every answer we accept can be tested without an IGDB account.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "game_metadata.h"

namespace igdb::policy {

  /// @brief IGDB's own external_games category for one of our store slugs, or -1 when it has none.
  int external_category_for_store(std::string_view store);

  /// @brief Our store slug for an IGDB external_games category, or empty when unknown.
  std::string store_for_external_category(int category);

  /**
   * @brief APIcalypse body asking which IGDB games these store ids belong to.
   *
   * Returns empty when none of the ids belong to a store IGDB indexes -- Ubisoft Connect,
   * the EA app and Battle.net have no category, so their games can only be found by name.
   */
  std::string external_lookup_query(const std::vector<metadata::store_id_t> &ids);

  /// @brief APIcalypse body fetching the full record for one or more IGDB game ids.
  std::string games_by_id_query(const std::vector<std::string> &igdb_ids);

  /// @brief APIcalypse body searching by name, for auto-matching and for the manual picker.
  std::string search_query(const std::string &name, int limit);

  /// @brief One store id resolved to the IGDB game that owns it.
  struct match_t {
    std::string igdb_id;
    std::string store;
    std::string store_id;
  };

  /// @brief Parse an external_games response. Malformed entries are dropped, not fatal.
  std::vector<match_t> parse_external_matches(const std::string &body);

  /// @brief An IGDB game record, still in IGDB's own terms.
  struct game_t {
    std::string igdb_id;
    std::string name;
    std::string summary;
    std::vector<std::string> genres;
    std::vector<std::string> developers;
    std::vector<std::string> publishers;
    // Seconds since the epoch, as IGDB reports first_release_date. 0 when unreleased/unknown.
    std::int64_t first_release_date {0};
    // 0-100, or -1 when IGDB has too few votes to report one.
    int community_score {-1};
    int critic_score {-1};
    std::string cover_image_id;
    std::string artwork_image_id;
  };

  /// @brief Parse a games response. Malformed entries are dropped, not fatal.
  std::vector<game_t> parse_games(const std::string &body);

  /// @brief Render a unix timestamp as the YYYY-MM-DD release date we store.
  std::string release_date_from_timestamp(std::int64_t seconds);

  /**
   * @brief Convert an IGDB record into the container the rest of the host speaks.
   *
   * Playtime and last-played are deliberately left at their defaults: they belong to whoever
   * launches the game, and a database cannot know them. The caller merges them back in.
   */
  metadata::game_metadata_t to_metadata(const game_t &game);

  /// @brief Full URL for an IGDB image, e.g. size "t_cover_big_2x" or "t_1080p".
  std::string image_url(const std::string &image_id, std::string_view size);

  /**
   * @brief Pick the best search hit for a name, or nothing when no hit is close enough.
   *
   * IGDB's search is fuzzy enough to answer "Half-Life" with a fan mod, so a hit only counts
   * when its name normalizes to the same string. Guessing wrong here writes the wrong summary
   * and cover onto someone's library, which is worse than leaving the game unmatched.
   */
  std::optional<game_t> best_name_match(const std::vector<game_t> &hits, const std::string &name);

  /// @brief Fold a title to its comparable form: lowercase, no punctuation, no edition suffix.
  std::string normalize_title(const std::string &name);

  /**
   * @brief IGDB's published limits: four requests a second, eight of them in flight.
   *
   * Exceeding either earns a 429, and the burst that earns it is the library scan we run on
   * first setup, so the limiter is not optional.
   */
  inline constexpr int k_requests_per_second = 4;
  inline constexpr int k_max_concurrent_requests = 8;

  /**
   * @brief How long to wait before a request, given when the previous ones went out.
   *
   * `recent` holds the send times of the last requests, oldest first, already trimmed to the
   * window the caller cares about. Zero means "send now".
   */
  std::chrono::milliseconds pacing_delay(const std::vector<std::chrono::steady_clock::time_point> &recent,
                                         std::chrono::steady_clock::time_point now);

  /// @brief Whether a cached record written at `written` is still usable at `now`.
  bool cache_is_fresh(std::chrono::system_clock::time_point written,
                      std::chrono::system_clock::time_point now,
                      std::chrono::hours ttl);

  /// @brief Default cache lifetime. Summaries and ratings drift slowly; a month is plenty.
  inline constexpr std::chrono::hours k_default_cache_ttl {24 * 30};

}  // namespace igdb::policy
