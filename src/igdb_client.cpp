/**
 * @file src/igdb_client.cpp
 * @brief Runtime half of the IGDB integration: credentials, tokens, requests, cache, art.
 */

// local includes
#include "igdb_client.h"

#include "config.h"
#include "file_handler.h"
#include "httpcommon.h"
#include "logging.h"
#include "platform/common.h"

#ifdef _WIN32
  #include "src/platform/windows/image_convert.h"
#endif

// standard includes
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>

// lib includes
#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace igdb {
  namespace {
    constexpr const char *k_token_url = "https://id.twitch.tv/oauth2/token";
    constexpr const char *k_api_base = "https://api.igdb.com/v4/";
    // A stuck request must not hold the resolver, and the resolver must not hold a library
    // sync. IGDB answers in well under a second when it answers at all.
    constexpr long k_request_timeout_seconds = 20;
    constexpr int k_max_retries = 2;

    struct state_t {
      std::mutex mutex;
      std::string access_token;
      std::chrono::steady_clock::time_point token_expiry {};
      // Send times of recent requests, oldest first, trimmed to the pacing window.
      std::deque<std::chrono::steady_clock::time_point> recent_requests;
      bool authenticated {false};
      std::string last_error;
      // Which numeric id IGDB gives each store, asked of IGDB rather than hardcoded. Empty
      // until the first lookup needs it.
      policy::source_map_t sources;
      bool sources_loaded {false};
      // The sources endpoint could not be reached and the retired category numbering is in
      // use instead. Kept so a 400 on that field can turn store-id matching off for good
      // rather than spending a failed request per game.
      bool sources_legacy {false};
      bool external_lookup_supported {true};
    };

    state_t &state() {
      static state_t instance;
      return instance;
    }

    /// @brief Forget what was learned about store-id lookups, so the next one asks again.
    void reset_lookup_state_locked(state_t &s) {
      s.external_lookup_supported = true;
      s.sources_loaded = false;
      s.sources.clear();
      s.sources_legacy = false;
    }

    std::size_t append_body(char *data, std::size_t size, std::size_t count, void *user) {
      auto *out = static_cast<std::string *>(user);
      out->append(data, size * count);
      return size * count;
    }

    /// @brief Where the secret lives. Falls back to the default rather than giving up, so a
    /// config that never had the key resolved is still usable instead of failing to save.
    std::string secret_path() {
      const auto configured = std::filesystem::path {config::igdb.secret_file};
      std::error_code error;
      if (configured.empty()) {
        return (platf::appdata() / "igdb_secret").string();
      }
      // A path that names a directory is a configuration that cannot work: writing the secret
      // would fail and reading it would silently return nothing. Put the file inside it rather
      // than failing, which is what someone pointing at a folder meant anyway.
      if (std::filesystem::is_directory(configured, error)) {
        return (configured / "igdb_secret").string();
      }
      return configured.string();
    }

    std::string read_secret() {
      const auto path = secret_path();
      if (path.empty()) {
        return {};
      }
      auto secret = file_handler::read_file(path.c_str());
      // The file is edited by hand often enough that a trailing newline is the common case.
      while (!secret.empty() && (secret.back() == '\n' || secret.back() == '\r' || secret.back() == ' ')) {
        secret.pop_back();
      }
      return secret;
    }

    /// @brief Hold back until sending now stays inside IGDB's four-per-second limit.
    void pace_locked(std::unique_lock<std::mutex> &lock) {
      auto &s = state();
      for (;;) {
        const auto now = std::chrono::steady_clock::now();
        while (!s.recent_requests.empty() && now - s.recent_requests.front() > std::chrono::seconds {2}) {
          s.recent_requests.pop_front();
        }
        const std::vector<std::chrono::steady_clock::time_point> window {
          s.recent_requests.begin(), s.recent_requests.end()};
        const auto delay = policy::pacing_delay(window, now);
        if (delay <= std::chrono::milliseconds {0}) {
          s.recent_requests.push_back(now);
          return;
        }
        // Released while sleeping so a second caller can pace against the same window rather
        // than queueing behind this one and then bursting.
        lock.unlock();
        std::this_thread::sleep_for(delay);
        lock.lock();
      }
    }

    /// @brief Obtain a client-credentials token. Caller holds the lock.
    bool refresh_token_locked(std::string &error_out) {
      auto &s = state();
      const auto client_id = config::igdb.client_id;
      const auto secret = read_secret();
      if (client_id.empty() || secret.empty()) {
        error_out = "No IGDB client id or secret configured";
        return false;
      }

      CURL *curl = curl_easy_init();  // NOSONAR
      if (!curl) {
        error_out = "Could not create a CURL instance";
        return false;
      }
      // Credentials go in the body, not the query string, so they stay out of proxy logs.
      std::string post_fields = "client_id=" + http::url_escape(client_id) +
                                "&client_secret=" + http::url_escape(secret) +
                                "&grant_type=client_credentials";
      std::string body;
      long status_code = 0;
      http::configure_curl_tls(curl);
      curl_easy_setopt(curl, CURLOPT_URL, k_token_url);
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_fields.c_str());
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, k_request_timeout_seconds);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
      const auto result = curl_easy_perform(curl);
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
      curl_easy_cleanup(curl);
      // Overwrite rather than let the secret sit in a freed buffer for the rest of the run.
      std::fill(post_fields.begin(), post_fields.end(), '\0');

      if (result != CURLE_OK) {
        error_out = std::string {"Could not reach the token endpoint: "} + curl_easy_strerror(result);
        return false;
      }
      if (status_code != 200) {
        // Twitch answers a bad client id and a bad secret identically, so neither can be
        // singled out here; the message says what to check.
        error_out = "IGDB rejected the credentials (HTTP " + std::to_string(status_code) + ")";
        return false;
      }
      const auto parsed = nlohmann::json::parse(body, nullptr, false);
      if (!parsed.is_object() || !parsed.contains("access_token") || !parsed["access_token"].is_string()) {
        error_out = "The token endpoint answered something unexpected";
        return false;
      }
      s.access_token = parsed["access_token"].get<std::string>();
      // Tokens last about two months. A minute of margin covers the request in flight when it
      // expires; anything longer just costs an extra exchange.
      auto lifetime = std::chrono::seconds {3600};
      if (parsed.contains("expires_in") && parsed["expires_in"].is_number_integer()) {
        lifetime = std::chrono::seconds {std::max<std::int64_t>(60, parsed["expires_in"].get<std::int64_t>())};
      }
      s.token_expiry = std::chrono::steady_clock::now() + lifetime - std::chrono::seconds {60};
      s.authenticated = true;
      s.last_error.clear();
      return true;
    }

    bool ensure_token_locked(std::string &error_out) {
      auto &s = state();
      if (!s.access_token.empty() && std::chrono::steady_clock::now() < s.token_expiry) {
        return true;
      }
      return refresh_token_locked(error_out);
    }

    /// @brief POST an APIcalypse body to an IGDB endpoint. Empty result means the error is set.
    std::optional<std::string> request(const std::string &endpoint, const std::string &query, std::string &error_out) {
      if (query.empty()) {
        error_out = "Empty query";
        return std::nullopt;
      }
      auto &s = state();
      std::unique_lock lock {s.mutex};
      for (int attempt = 0; attempt <= k_max_retries; ++attempt) {
        if (!ensure_token_locked(error_out)) {
          s.authenticated = false;
          s.last_error = error_out;
          return std::nullopt;
        }
        const auto client_id = config::igdb.client_id;
        const auto token = s.access_token;
        pace_locked(lock);

        CURL *curl = curl_easy_init();  // NOSONAR
        if (!curl) {
          error_out = "Could not create a CURL instance";
          return std::nullopt;
        }
        curl_slist *headers = nullptr;
        headers = curl_slist_append(headers, ("Client-ID: " + client_id).c_str());
        headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
        headers = curl_slist_append(headers, "Accept: application/json");
        headers = curl_slist_append(headers, "Content-Type: text/plain");

        std::string body;
        long status_code = 0;
        const std::string url = std::string {k_api_base} + endpoint;
        http::configure_curl_tls(curl);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, query.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, k_request_timeout_seconds);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

        // The network call is the slow part and holds no shared state beyond the handle, so
        // the lock goes back while it runs and other callers can pace against the window.
        lock.unlock();
        const auto result = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        lock.lock();

        if (result != CURLE_OK) {
          error_out = std::string {"Could not reach IGDB: "} + curl_easy_strerror(result);
          s.last_error = error_out;
          return std::nullopt;
        }
        if (status_code == 200) {
          s.authenticated = true;
          s.last_error.clear();
          return body;
        }
        if (status_code == 401 && attempt < k_max_retries) {
          // The token was revoked or expired early; one more exchange, then give up.
          s.access_token.clear();
          continue;
        }
        if (status_code == 429 && attempt < k_max_retries) {
          lock.unlock();
          std::this_thread::sleep_for(std::chrono::milliseconds {1100});
          lock.lock();
          continue;
        }
        error_out = "IGDB answered HTTP " + std::to_string(status_code);
        if (!body.empty()) {
          error_out += ": " + body.substr(0, 200);
        }
        if (status_code == 401) {
          s.authenticated = false;
        }
        s.last_error = error_out;
        return std::nullopt;
      }
      return std::nullopt;
    }

    std::filesystem::path cache_root() {
      return std::filesystem::path {config::igdb.cache_dir};
    }

    std::filesystem::path record_cache_path(const std::string &igdb_id) {
      return cache_root() / ("game-" + igdb_id + ".json");
    }

    std::chrono::hours cache_ttl() {
      return std::chrono::hours {std::max(0, config::igdb.cache_ttl_days) * 24};
    }

    std::optional<policy::game_t> read_cached_record(const std::string &igdb_id) {
      if (config::igdb.cache_ttl_days <= 0) {
        return std::nullopt;
      }
      const auto path = record_cache_path(igdb_id);
      const auto text = file_handler::read_file(path.string().c_str());
      if (text.empty()) {
        return std::nullopt;
      }
      const auto parsed = nlohmann::json::parse(text, nullptr, false);
      if (!parsed.is_object() || !parsed.contains("fetched_at") || !parsed.contains("record")) {
        return std::nullopt;
      }
      if (!parsed["fetched_at"].is_number_integer()) {
        return std::nullopt;
      }
      const auto written = std::chrono::system_clock::time_point {
        std::chrono::seconds {parsed["fetched_at"].get<std::int64_t>()}};
      if (!policy::cache_is_fresh(written, std::chrono::system_clock::now(), cache_ttl())) {
        return std::nullopt;
      }
      // Stored as the raw IGDB array so one parser covers both the live and the cached path.
      auto games = policy::parse_games(parsed["record"].dump());
      if (games.empty()) {
        return std::nullopt;
      }
      return games.front();
    }

    void write_cached_record(const std::string &igdb_id, const nlohmann::json &record) {
      if (config::igdb.cache_ttl_days <= 0) {
        return;
      }
      try {
        file_handler::make_directory(cache_root().string());
        nlohmann::json wrapper;
        wrapper["fetched_at"] = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
        wrapper["record"] = nlohmann::json::array({record});
        file_handler::write_file(record_cache_path(igdb_id).string().c_str(), wrapper.dump());
      } catch (const std::exception &e) {
        BOOST_LOG(debug) << "IGDB: could not cache record " << igdb_id << ": " << e.what();
      }
    }

    std::string download_image(const std::string &image_id, std::string_view size,
                               const std::filesystem::path &destination) {
      if (image_id.empty()) {
        return {};
      }
      // Only a finished conversion is ever moved to the destination, so a file there is art.
      std::error_code exists_error;
      if (std::filesystem::exists(destination, exists_error) &&
          std::filesystem::file_size(destination, exists_error) > 0u) {
        return destination.generic_string();
      }
      const auto url = policy::image_url(image_id, size);
      if (url.empty()) {
        return {};
      }
      auto source = destination;
      source.replace_extension(".download");
      if (!http::download_file(url, source.string())) {
        return {};
      }
      // IGDB serves JPEG, but clients are only ever handed PNG: the app asset endpoints check
      // for a PNG signature and send the placeholder otherwise. So the download is converted
      // the same way Playnite art is, into a temporary file first so that a conversion that
      // fails halfway never leaves a truncated PNG where the cache would trust it.
      auto converted = destination;
      converted += ".tmp";
      bool ok = false;
      // A tiny file is an error body rather than art, whatever the status said.
      if (std::filesystem::file_size(source, exists_error) >= 1024u && !exists_error) {
#ifdef _WIN32
        ok = platf::img::convert_to_png_96dpi(source.wstring(), converted.wstring());
#else
        // Nothing here can transcode JPEG, and a JPEG would never reach a client anyway.
        ok = false;
#endif
      }
      std::filesystem::remove(source, exists_error);
      if (ok) {
        std::filesystem::rename(converted, destination, exists_error);
        ok = !exists_error;
      }
      if (!ok) {
        std::filesystem::remove(converted, exists_error);
        return {};
      }
      return destination.generic_string();
    }
  }  // namespace

  status_t status() {
    auto &s = state();
    std::scoped_lock lock {s.mutex};
    status_t out;
    out.enabled = config::igdb.enabled;
    out.configured = !config::igdb.client_id.empty() && !read_secret().empty();
    out.authenticated = s.authenticated;
    out.last_error = s.last_error;
    out.secret_file = secret_path();
    return out;
  }

  bool save_secret(const std::string &secret, std::string &error_out) {
    const auto path = secret_path();
    if (path.empty()) {
      error_out = "No location is configured for the IGDB secret";
      return false;
    }
    // write_file creates the parent directory itself, so there is nothing to prepare here.
    // The path is named in the error because it is the one piece of information that turns
    // "could not save" into something actionable, and a path is not a secret.
    if (file_handler::write_file(path.c_str(), secret) != 0) {
      error_out = "Could not write the IGDB secret to " + path;
      BOOST_LOG(error) << error_out;
      return false;
    }
    std::error_code permission_error;
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, permission_error);
    if (permission_error) {
      BOOST_LOG(warning) << "IGDB: could not restrict permissions on " << path << ": "
                         << permission_error.message();
    }
    auto &s = state();
    std::scoped_lock lock {s.mutex};
    // A new secret invalidates whatever the old one bought, including what the old account
    // was told about store-id lookups: a failure then may well have been the credentials.
    s.access_token.clear();
    s.authenticated = false;
    s.last_error.clear();
    reset_lookup_state_locked(s);
    return true;
  }

  bool clear_secret(std::string &error_out) {
    const auto path = secret_path();
    std::error_code remove_error;
    if (!path.empty()) {
      std::filesystem::remove(path, remove_error);
    }
    auto &s = state();
    std::scoped_lock lock {s.mutex};
    s.access_token.clear();
    s.authenticated = false;
    s.last_error.clear();
    reset_lookup_state_locked(s);
    if (remove_error) {
      error_out = "Could not remove " + path + ": " + remove_error.message();
      return false;
    }
    return true;
  }

  bool verify(std::string &error_out) {
    auto &s = state();
    std::unique_lock lock {s.mutex};
    s.access_token.clear();
    // Verifying is what a user does after fixing whatever was wrong, so store-id lookups get
    // another chance too.
    reset_lookup_state_locked(s);
    const bool ok = refresh_token_locked(error_out);
    if (!ok) {
      s.authenticated = false;
      s.last_error = error_out;
    }
    return ok;
  }

  std::optional<policy::game_t> fetch_game(const std::string &igdb_id, std::string &error_out) {
    if (igdb_id.empty()) {
      error_out = "No IGDB id";
      return std::nullopt;
    }
    // Checked before the id names a cache file: it arrives from the API and from apps.json,
    // and only digits keep it from naming a path outside the cache.
    if (!std::all_of(igdb_id.begin(), igdb_id.end(), [](unsigned char character) {
          return std::isdigit(character) != 0;
        })) {
      error_out = "Not a usable IGDB id";
      return std::nullopt;
    }
    if (auto cached = read_cached_record(igdb_id)) {
      return cached;
    }
    const auto query = policy::games_by_id_query({igdb_id});
    if (query.empty()) {
      error_out = "Not a usable IGDB id";
      return std::nullopt;
    }
    const auto body = request("games", query, error_out);
    if (!body) {
      return std::nullopt;
    }
    auto games = policy::parse_games(*body);
    if (games.empty()) {
      error_out = "IGDB has no game with id " + igdb_id;
      return std::nullopt;
    }
    const auto raw = nlohmann::json::parse(*body, nullptr, false);
    if (raw.is_array() && !raw.empty()) {
      write_cached_record(igdb_id, raw.front());
    }
    return games.front();
  }

  std::vector<policy::game_t> search(const std::string &name, int limit, std::string &error_out) {
    const auto query = policy::search_query(name, limit);
    if (query.empty()) {
      error_out = "Empty search";
      return {};
    }
    const auto body = request("games", query, error_out);
    if (!body) {
      return {};
    }
    auto games = policy::parse_games(*body);
    // Search results are cached individually so picking one in the editor costs no request.
    const auto raw = nlohmann::json::parse(*body, nullptr, false);
    if (raw.is_array()) {
      for (const auto &entry : raw) {
        if (entry.is_object() && entry.contains("id")) {
          write_cached_record(std::to_string(entry["id"].get<std::int64_t>()), entry);
        }
      }
    }
    return games;
  }

  namespace {
    /**
     * @brief The store-id numbering IGDB is using, asked of IGDB once per run.
     *
     * Not hardcoded, because the enum this integration was first written against has since
     * been retired, and a stale table does not announce itself: every lookup just stops
     * matching, which is indistinguishable from a library IGDB has never heard of.
     */
    policy::source_map_t ensure_sources(bool &legacy_out, std::string &error_out) {
      auto &s = state();
      {
        std::scoped_lock lock {s.mutex};
        if (s.sources_loaded) {
          legacy_out = s.sources_legacy;
          return s.sources;
        }
      }
      // request() takes the lock itself, so it is called with nothing held. Two callers
      // racing here cost one extra request and agree on the answer.
      auto sources = policy::source_map_t {};
      bool legacy = false;
      // Whether IGDB actually said something about its sources. A 400 or 404 says the endpoint
      // is not there, which is an answer; a timeout, a rate limit or a bad token says nothing.
      bool answered = false;
      if (const auto body = request("external_game_sources", policy::external_sources_query(), error_out)) {
        sources = policy::parse_external_sources(*body);
        answered = true;
      } else if (error_out.find("HTTP 400") != std::string::npos || error_out.find("HTTP 404") != std::string::npos) {
        answered = true;
      }
      if (sources.empty()) {
        // An older or mirrored IGDB. Matching on the retired numbering is still better than
        // giving up on store ids and matching every game by title.
        sources = policy::legacy_source_map();
        legacy = true;
        BOOST_LOG(warning) << "IGDB: could not read the external game sources, falling back to the "
                              "retired category numbering";
      } else {
        BOOST_LOG(info) << "IGDB: matched " << sources.size() << " store sources";
      }
      legacy_out = legacy;
      if (!answered) {
        // The fallback serves this lookup, and the next one asks again. Cached, a single
        // failure at startup would pin the retired numbering until the next restart.
        return sources;
      }
      std::scoped_lock lock {s.mutex};
      s.sources = sources;
      s.sources_legacy = legacy;
      s.sources_loaded = true;
      return sources;
    }
  }  // namespace

  std::string resolve_store_ids(const std::vector<metadata::store_id_t> &ids, std::string &error_out) {
    auto &s = state();
    {
      std::scoped_lock lock {s.mutex};
      if (!s.external_lookup_supported) {
        return {};
      }
    }
    bool legacy = false;
    const auto sources = ensure_sources(legacy, error_out);
    // A failure to read the sources is not itself a failure to match: the fallback map is in
    // hand either way, so the error from that request must not reach the caller as this
    // game's error.
    error_out.clear();

    const auto query = policy::external_lookup_query(ids, sources, legacy);
    if (query.empty()) {
      // No store IGDB indexes. Not an error: the caller falls back to a name search.
      return {};
    }
    const auto body = request("external_games", query, error_out);
    if (!body) {
      // Only the retired category field is expected to be refused outright, and then it will
      // be refused for every game. A 400 on the current field is about this query, not about
      // store-id matching as a whole. Either way save_secret and verify turn lookups back on.
      if (legacy && error_out.find("HTTP 400") != std::string::npos) {
        std::scoped_lock lock {s.mutex};
        s.external_lookup_supported = false;
        BOOST_LOG(warning) << "IGDB: external_games lookups rejected, falling back to name matching";
      }
      return {};
    }
    const auto matches = policy::parse_external_matches(*body);
    // Ids are handed over most specific first, so the first one IGDB answered for is the best
    // match. The query already constrained which source could answer for each uid, so
    // comparing the uid alone cannot pick up another store's game.
    for (const auto &id : ids) {
      for (const auto &match : matches) {
        if (match.store_id == id.id) {
          return match.igdb_id;
        }
      }
    }
    return matches.empty() ? std::string {} : matches.front().igdb_id;
  }

  std::string download_cover(const policy::game_t &game, const std::filesystem::path &covers_root) {
    return download_image(game.cover_image_id, "t_cover_big_2x",
                          covers_root / ("igdb_" + game.igdb_id + ".png"));
  }

  std::string download_background(const policy::game_t &game, const std::filesystem::path &covers_root) {
    return download_image(game.artwork_image_id, "t_1080p",
                          covers_root / ("igdb_bg_" + game.igdb_id + ".png"));
  }

  std::size_t clear_cache() {
    std::error_code error;
    const auto root = cache_root();
    if (root.empty() || !std::filesystem::exists(root, error)) {
      return 0;
    }
    std::size_t removed = 0;
    for (const auto &entry : std::filesystem::directory_iterator {root, error}) {
      if (entry.is_regular_file(error) && std::filesystem::remove(entry.path(), error)) {
        ++removed;
      }
    }
    auto &s = state();
    std::scoped_lock lock {s.mutex};
    reset_lookup_state_locked(s);
    return removed;
  }

}  // namespace igdb
