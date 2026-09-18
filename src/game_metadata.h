/**
 * @file src/game_metadata.h
 * @brief Provider-agnostic game metadata: the container, its apps.json form, and store identity.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

namespace metadata {

  /**
   * @brief Descriptive metadata for one app, whatever produced it.
   *
   * This started life as Playnite passthrough and is now a plain container: Playnite, IGDB and
   * the user all fill the same fields, and `source` records which of them did it last. Absent
   * values stay empty or -1; `present` is set when the app carries any metadata at all.
   */
  struct game_metadata_t {
    bool present {false};
    std::string description;
    std::vector<std::string> genres;
    std::vector<std::string> developers;
    std::vector<std::string> publishers;
    std::string release_date;
    int community_score {-1};
    int critic_score {-1};
    // When the game was last played (ISO8601), and for how long in total. Unlike anything a
    // client can track on its own, these cover sessions played at the PC itself. Empty / 0 when
    // the game has never been played or the provider reports nothing.
    std::string last_played;
    uint64_t playtime_minutes {0};
    // Local path to the converted background/hero image, served via /appbackground. Empty when
    // the game has none.
    std::string background_image_path;

    // Which provider produced the descriptive fields above: "playnite", "igdb", "manual", or
    // empty for metadata that predates this field. Playtime and last-played are excluded from
    // that claim -- they come from whoever actually launches the game, never from a database.
    std::string source;
    // IGDB game id this app resolved to, as a decimal string. Set by the resolver when it
    // matched, or by the user when they corrected a wrong match. Kept even when the descriptive
    // fields are locked, because it is what a refresh would re-fetch.
    std::string igdb_id;
    // The user edited this app's metadata by hand. Automatic resolvers must leave every
    // descriptive field alone while this is set; without it a background refresh would quietly
    // undo the correction the user just made.
    bool locked {false};
  };

  /// @brief One store's identifier for a game, as IGDB's external_games knows it.
  struct store_id_t {
    // Lowercase store slug: "steam", "gog", "epic", "ubisoft", "microsoft", "itch", "origin",
    // "battlenet". These are our names; igdb_policy maps them to IGDB's own numbering.
    std::string store;
    std::string id;

    bool operator==(const store_id_t &) const = default;
  };

  /**
   * @brief Read an app node's metadata.
   *
   * Reads the canonical `meta-*` keys and falls back to the `playnite-*` keys written before
   * metadata stopped being Playnite's alone, so an existing apps.json keeps its metadata across
   * the upgrade without a migration pass.
   */
  game_metadata_t read_from_app(const nlohmann::json &app);

  /**
   * @brief Write metadata back into an app node.
   *
   * Writes the canonical keys and erases both the absent ones and every legacy `playnite-*`
   * key, so a node never carries two spellings of the same value and apps.json stays
   * idempotent across repeated syncs.
   */
  void write_to_app(nlohmann::json &app, const game_metadata_t &meta);

  /// @brief Recompute `present` from the descriptive fields. Called by read_from_app.
  bool has_any_value(const game_metadata_t &meta);

  /**
   * @brief Every store identity an app node carries, most specific first.
   *
   * A Lutris record for a GOG game knows both "this is Lutris game 42" and "this is GOG id
   * 1207658924"; only the latter means anything to IGDB, so Lutris' own id is not returned
   * here. Steam-backed Lutris records yield the same steam identity a direct Steam app would.
   */
  std::vector<store_id_t> store_ids_of(const nlohmann::json &app);

  /// @brief Normalize a provider's store name ("GOG Store", "Epic Games") to our slug.
  std::string normalize_store_name(const std::string &name);

}  // namespace metadata
