/**
 * @file src/game_metadata_bulk.cpp
 * @brief Background job that fills app metadata from the IGDB mirror (LizardByte GameDB).
 */
#include "game_metadata_bulk.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include <curl/curl.h>

#include "config.h"
#include "confighttp.h"
#include "file_handler.h"
#include "game_metadata_policy.h"
#include "httpcommon.h"
#include "logging.h"
#include "platform/common.h"

using namespace std::literals;

namespace game_metadata::bulk {

  namespace {
    namespace fs = std::filesystem;

    // IGDB documents four requests per second for its API; the mirror gets the same courtesy.
    constexpr auto kRequestInterval = 250ms;
    constexpr long kConnectTimeoutSeconds = 10;
    constexpr long kTransferTimeoutSeconds = 30;
    constexpr const char *kUserAgent = "Vibepollo metadata fetch (+https://github.com/DanieleS/Vibepollo)";

    struct result_t {
      std::string uuid;
      std::string name;
      std::string status;  ///< matched | matched_ambiguous | unmatched | skipped | failed
      std::string igdb_id;
      std::string igdb_name;
      std::string reason;
      bool background {false};
      bool cover {false};
    };

    struct state_t {
      std::mutex mutex;
      std::condition_variable wake;
      std::thread worker;
      std::atomic_bool running {false};
      std::atomic_bool cancel_requested {false};
      options_t options;
      std::size_t total {0};
      std::size_t processed {0};
      std::size_t matched {0};
      std::size_t unmatched {0};
      std::size_t skipped {0};
      std::size_t failed {0};
      std::string current;
      std::string error;
      bool cancelled {false};
      std::chrono::system_clock::time_point started_at {};
      std::chrono::system_clock::time_point finished_at {};
      std::vector<result_t> results;
    };

    state_t &state() {
      static state_t instance;
      return instance;
    }

    // Waits between requests without holding the state mutex; returns false when cancelled.
    class throttle_t {
    public:
      bool wait() {
        auto &st = state();
        std::unique_lock lock(st.mutex);
        const auto next_allowed = last_request_ + kRequestInterval;
        st.wake.wait_until(lock, next_allowed, [&st] {
          return st.cancel_requested.load(std::memory_order_acquire);
        });
        if (st.cancel_requested.load(std::memory_order_acquire)) {
          return false;
        }
        last_request_ = std::chrono::steady_clock::now();
        return true;
      }

    private:
      std::chrono::steady_clock::time_point last_request_ {std::chrono::steady_clock::now() - kRequestInterval};
    };

    std::size_t write_to_string(char *ptr, std::size_t size, std::size_t nmemb, void *userdata) {
      auto *out = static_cast<std::string *>(userdata);
      out->append(ptr, size * nmemb);
      return size * nmemb;
    }

    enum class fetch_status_e {
      ok,
      not_found,
      error,
    };

    fetch_status_e fetch_text(const std::string &url, std::string &body) {
      body.clear();
      CURL *curl = curl_easy_init();  // NOSONAR
      if (!curl) {
        BOOST_LOG(error) << "Metadata fetch: unable to create a CURL handle.";
        return fetch_status_e::error;
      }
      http::configure_curl_tls(curl);
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
      curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);  // NOSONAR
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, kTransferTimeoutSeconds);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
      curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
#ifdef _WIN32
      curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif
      const CURLcode result = curl_easy_perform(curl);
      long http_code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
      curl_easy_cleanup(curl);
      if (result != CURLE_OK) {
        BOOST_LOG(warning) << "Metadata fetch: request to " << url << " failed (curl code " << result << ").";
        return fetch_status_e::error;
      }
      if (http_code == 404) {
        return fetch_status_e::not_found;
      }
      if (http_code < 200 || http_code >= 300) {
        BOOST_LOG(warning) << "Metadata fetch: request to " << url << " returned HTTP " << http_code << '.';
        return fetch_status_e::error;
      }
      return fetch_status_e::ok;
    }

    bool file_is_png(const fs::path &path) {
      std::ifstream file(path, std::ios::binary);
      if (!file) {
        return false;
      }
      unsigned char signature[8] {};
      file.read(reinterpret_cast<char *>(signature), sizeof(signature));
      static const unsigned char png_signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
      return file.gcount() == 8 && std::equal(std::begin(signature), std::end(signature), std::begin(png_signature));
    }

    // Downloads an IGDB image as PNG unless it is already on disk. Returns the path or empty.
    std::string fetch_image(throttle_t &throttle, const fs::path &destination, const char *size, const std::string &slug) {
      if (slug.empty()) {
        return {};
      }
      std::error_code ec;
      if (fs::exists(destination, ec) && file_is_png(destination)) {
        return destination.generic_string();
      }
      if (!throttle.wait()) {
        return {};
      }
      const auto url = image_url(size, slug);
      if (!http::download_file(url, destination.string())) {
        fs::remove(destination, ec);
        return {};
      }
      if (!file_is_png(destination)) {
        BOOST_LOG(warning) << "Metadata fetch: " << url << " did not return a PNG; discarding.";
        fs::remove(destination, ec);
        return {};
      }
      return destination.generic_string();
    }

    struct app_entry_t {
      std::string uuid;
      std::string name;
      bool has_metadata {false};
      std::string source;
      bool has_cover {false};
    };

    std::vector<app_entry_t> load_targets(const options_t &options, std::string &error) {
      std::vector<app_entry_t> targets;
      nlohmann::json root;
      try {
        root = nlohmann::json::parse(file_handler::read_file(config::stream.file_apps.c_str()));
      } catch (const std::exception &e) {
        error = std::string {"Unable to read the applications file: "} + e.what();
        return targets;
      }
      if (!root.contains("apps") || !root["apps"].is_array()) {
        return targets;
      }
      for (const auto &app : root["apps"]) {
        if (!app.is_object()) {
          continue;
        }
        app_entry_t entry;
        entry.uuid = app.value("uuid", std::string {});
        entry.name = app.value("name", std::string {});
        if (entry.uuid.empty() || entry.name.empty()) {
          continue;
        }
        if (!options.uuids.empty() && std::find(options.uuids.begin(), options.uuids.end(), entry.uuid) == options.uuids.end()) {
          continue;
        }
        entry.has_metadata = has_descriptive_metadata(app);
        entry.source = read_app_metadata(app).source;
        entry.has_cover = app.contains("image-path") && app["image-path"].is_string() && !app["image-path"].get<std::string>().empty();
        targets.push_back(std::move(entry));
      }
      return targets;
    }

    // Re-reads apps.json right before writing so edits made meanwhile are kept, applies the
    // change to the one app, and refreshes the running catalog.
    bool update_app(const std::string &uuid, const std::function<void(nlohmann::json &)> &mutate, std::string &error) {
      nlohmann::json root;
      try {
        root = nlohmann::json::parse(file_handler::read_file(config::stream.file_apps.c_str()));
      } catch (const std::exception &e) {
        error = std::string {"Unable to re-read the applications file: "} + e.what();
        return false;
      }
      if (!root.contains("apps") || !root["apps"].is_array()) {
        error = "The applications file has no app list.";
        return false;
      }
      for (auto &app : root["apps"]) {
        if (app.is_object() && app.value("uuid", std::string {}) == uuid) {
          mutate(app);
          if (!confighttp::refresh_client_apps_cache(root, false)) {
            error = "Unable to write the applications file.";
            return false;
          }
          return true;
        }
      }
      error = "The application was removed while the job was running.";
      return false;
    }

    void record(result_t result) {
      auto &st = state();
      std::lock_guard lock(st.mutex);
      if (result.status == "matched" || result.status == "matched_ambiguous") {
        ++st.matched;
      } else if (result.status == "unmatched") {
        ++st.unmatched;
      } else if (result.status == "skipped") {
        ++st.skipped;
      } else {
        ++st.failed;
      }
      ++st.processed;
      st.results.push_back(std::move(result));
    }

    void process_app(const app_entry_t &entry, const options_t &options, throttle_t &throttle, std::unordered_map<std::string, std::optional<std::vector<bucket_entry_t>>> &bucket_cache, const fs::path &covers_root) {
      result_t result;
      result.uuid = entry.uuid;
      result.name = entry.name;

      if (entry.has_metadata && !options.refresh_existing) {
        result.status = "skipped";
        result.reason = entry.source == source_playnite ? "playnite" : "existing";
        record(std::move(result));
        return;
      }

      const auto key = bucket_key(entry.name);
      auto cached = bucket_cache.find(key);
      if (cached == bucket_cache.end()) {
        if (!throttle.wait()) {
          return;
        }
        std::string body;
        const auto fetched = fetch_text(bucket_url(key), body);
        std::optional<std::vector<bucket_entry_t>> entries;
        if (fetched == fetch_status_e::ok) {
          try {
            entries = parse_bucket(nlohmann::json::parse(body));
          } catch (const std::exception &e) {
            BOOST_LOG(warning) << "Metadata fetch: bucket " << key << " is not valid JSON: " << e.what();
          }
        } else if (fetched == fetch_status_e::not_found) {
          entries = std::vector<bucket_entry_t> {};
        }
        cached = bucket_cache.emplace(key, std::move(entries)).first;
      }
      if (!cached->second) {
        result.status = "failed";
        result.reason = "bucket_unavailable";
        record(std::move(result));
        return;
      }

      const auto match = select_match(entry.name, *cached->second);
      if (match.kind == match_kind_e::none) {
        result.status = "unmatched";
        record(std::move(result));
        return;
      }
      result.igdb_id = match.id;
      result.igdb_name = match.name;

      if (!throttle.wait()) {
        return;
      }
      std::string body;
      const auto fetched = fetch_text(game_url(match.id), body);
      std::optional<igdb_game_t> game;
      if (fetched == fetch_status_e::ok) {
        try {
          game = parse_game(nlohmann::json::parse(body));
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "Metadata fetch: game " << match.id << " is not valid JSON: " << e.what();
        }
      }
      if (!game) {
        result.status = "failed";
        result.reason = fetched == fetch_status_e::not_found ? "game_missing" : "game_unavailable";
        record(std::move(result));
        return;
      }

      std::string background_path;
      if (options.download_background) {
        background_path = fetch_image(throttle, covers_root / ("igdb_bg_" + game->id + ".png"), igdb_background_size, game->artwork_slug);
        result.background = !background_path.empty();
      }
      std::string cover_path;
      if (options.download_cover && !entry.has_cover) {
        cover_path = fetch_image(throttle, covers_root / ("igdb_" + game->id + ".png"), igdb_cover_size, game->cover_slug);
        result.cover = !cover_path.empty();
      }
      if (state().cancel_requested.load(std::memory_order_acquire)) {
        return;
      }

      const auto fields = to_descriptive(*game, background_path);
      std::string error;
      const bool written = update_app(
        entry.uuid,
        [&](nlohmann::json &app) {
          write_descriptive(app, fields, source_igdb, game->id);
          if (!cover_path.empty()) {
            const bool still_uncovered = !app.contains("image-path") || !app["image-path"].is_string() || app["image-path"].get<std::string>().empty();
            if (still_uncovered) {
              app["image-path"] = cover_path;
            }
          }
        },
        error
      );
      if (!written) {
        result.status = "failed";
        result.reason = error;
        record(std::move(result));
        return;
      }
      result.status = match.kind == match_kind_e::exact && match.exact_candidates <= 1 ? "matched" : "matched_ambiguous";
      if (match.kind == match_kind_e::unique_prefix) {
        result.reason = "prefix";
      } else if (match.exact_candidates > 1) {
        result.reason = "multiple_exact";
      }
      BOOST_LOG(info) << "Metadata fetch: '" << entry.name << "' -> IGDB " << game->id << " (" << game->name << ").";
      record(std::move(result));
    }

    void run(options_t options) {
      auto &st = state();
      std::string error;
      auto targets = load_targets(options, error);
      {
        std::lock_guard lock(st.mutex);
        st.total = targets.size();
        st.error = error;
      }
      if (error.empty()) {
        const fs::path covers_root = platf::appdata() / "covers";
        file_handler::make_directory(covers_root.string());
        throttle_t throttle;
        std::unordered_map<std::string, std::optional<std::vector<bucket_entry_t>>> bucket_cache;
        for (const auto &entry : targets) {
          if (st.cancel_requested.load(std::memory_order_acquire)) {
            break;
          }
          {
            std::lock_guard lock(st.mutex);
            st.current = entry.name;
          }
          try {
            process_app(entry, options, throttle, bucket_cache, covers_root);
          } catch (const std::exception &e) {
            result_t result;
            result.uuid = entry.uuid;
            result.name = entry.name;
            result.status = "failed";
            result.reason = e.what();
            record(std::move(result));
          }
        }
      }
      {
        std::lock_guard lock(st.mutex);
        st.current.clear();
        st.cancelled = st.cancel_requested.load(std::memory_order_acquire);
        st.finished_at = std::chrono::system_clock::now();
        st.running.store(false, std::memory_order_release);
      }
      BOOST_LOG(info) << "Metadata fetch finished: " << st.matched << " matched, " << st.unmatched << " unmatched, "
                      << st.skipped << " skipped, " << st.failed << " failed" << (st.cancelled ? " (cancelled)." : ".");
    }

    std::string iso_time(std::chrono::system_clock::time_point time) {
      if (time == std::chrono::system_clock::time_point {}) {
        return {};
      }
      const auto seconds = std::chrono::floor<std::chrono::seconds>(time);
      return std::format("{:%FT%TZ}", seconds);
    }
  }  // namespace

  bool start(options_t options) {
    auto &st = state();
    std::lock_guard lock(st.mutex);
    if (st.running.load(std::memory_order_acquire)) {
      return false;
    }
    if (st.worker.joinable()) {
      st.worker.join();
    }
    st.options = options;
    st.total = st.processed = st.matched = st.unmatched = st.skipped = st.failed = 0;
    st.current.clear();
    st.error.clear();
    st.cancelled = false;
    st.results.clear();
    st.started_at = std::chrono::system_clock::now();
    st.finished_at = {};
    st.cancel_requested.store(false, std::memory_order_release);
    st.running.store(true, std::memory_order_release);
    st.worker = std::thread(run, std::move(options));
    return true;
  }

  bool cancel() {
    auto &st = state();
    std::lock_guard lock(st.mutex);
    if (!st.running.load(std::memory_order_acquire)) {
      return false;
    }
    st.cancel_requested.store(true, std::memory_order_release);
    st.wake.notify_all();
    return true;
  }

  nlohmann::json status() {
    auto &st = state();
    std::lock_guard lock(st.mutex);
    nlohmann::json out;
    out["running"] = st.running.load(std::memory_order_acquire);
    out["cancelled"] = st.cancelled;
    out["total"] = st.total;
    out["processed"] = st.processed;
    out["matched"] = st.matched;
    out["unmatched"] = st.unmatched;
    out["skipped"] = st.skipped;
    out["failed"] = st.failed;
    out["current"] = st.current;
    out["error"] = st.error;
    out["started_at"] = iso_time(st.started_at);
    out["finished_at"] = iso_time(st.finished_at);
    out["options"] = {
      {"refresh_existing", st.options.refresh_existing},
      {"download_background", st.options.download_background},
      {"download_cover", st.options.download_cover},
      {"uuids", st.options.uuids},
    };
    nlohmann::json results = nlohmann::json::array();
    for (const auto &result : st.results) {
      nlohmann::json node = {
        {"uuid", result.uuid},
        {"name", result.name},
        {"status", result.status},
      };
      if (!result.igdb_id.empty()) {
        node["igdb_id"] = result.igdb_id;
        node["igdb_name"] = result.igdb_name;
      }
      if (!result.reason.empty()) {
        node["reason"] = result.reason;
      }
      if (result.background) {
        node["background"] = true;
      }
      if (result.cover) {
        node["cover"] = true;
      }
      results.push_back(std::move(node));
    }
    out["results"] = std::move(results);
    return out;
  }

  void shutdown() {
    auto &st = state();
    {
      std::lock_guard lock(st.mutex);
      st.cancel_requested.store(true, std::memory_order_release);
      st.wake.notify_all();
    }
    if (st.worker.joinable()) {
      st.worker.join();
    }
  }

}  // namespace game_metadata::bulk
