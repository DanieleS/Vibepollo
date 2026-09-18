/**
 * @file src/igdb_client.h
 * @brief Runtime half of the IGDB integration: credentials, tokens, requests, cache, art.
 */
#pragma once

// standard includes
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// local includes
#include "game_metadata.h"
#include "igdb_policy.h"

namespace igdb {

  /// @brief What the settings page needs to show, without ever returning the secret itself.
  struct status_t {
    bool enabled {false};
    // Both a client id and a stored secret are present. Says nothing about whether they work.
    bool configured {false};
    // A token was obtained at least once this run and has not been rejected since.
    bool authenticated {false};
    // Why the last request or token exchange failed, for the settings page to show verbatim.
    std::string last_error;
  };

  status_t status();

  /**
   * @brief Store the client secret outside sunshine.conf.
   *
   * GET /api/config returns the config file as it stands, so a secret kept there would be
   * handed to anything allowed to read settings. This writes it to its own owner-only file,
   * the same way host credentials are kept.
   */
  bool save_secret(const std::string &secret);

  /// @brief Forget the stored secret. Leaves the client id alone; that one is not sensitive.
  bool clear_secret();

  /// @brief Exchange the stored credentials for a token now, to tell the user they work.
  bool verify(std::string &error_out);

  /// @brief Fetch one IGDB record, from the on-disk cache when it is still fresh.
  std::optional<policy::game_t> fetch_game(const std::string &igdb_id, std::string &error_out);

  /// @brief Search IGDB by name. Used by auto-matching and by the manual picker.
  std::vector<policy::game_t> search(const std::string &name, int limit, std::string &error_out);

  /**
   * @brief Find the IGDB game a set of store ids belongs to.
   *
   * Returns the id of the first store IGDB recognised. Empty when none of the ids belong to an
   * indexed store or IGDB knows none of them -- both are ordinary outcomes, not errors.
   */
  std::string resolve_store_ids(const std::vector<metadata::store_id_t> &ids, std::string &error_out);

  /**
   * @brief Download a record's cover and hero art into the managed covers directory.
   *
   * Returns the local path, or empty when the record has no such image or the download failed.
   * An existing file is reused, so a re-resolve costs nothing.
   */
  std::string download_cover(const policy::game_t &game, const std::filesystem::path &covers_root);
  std::string download_background(const policy::game_t &game, const std::filesystem::path &covers_root);

  /// @brief Drop every cached record and downloaded image. Returns how many files went.
  std::size_t clear_cache();

}  // namespace igdb
