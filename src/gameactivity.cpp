/**
 * @file src/gameactivity.cpp
 * @brief Play sessions, read from GameActivity's data: where `/appstats` gets its history.
 */

// local includes
#include "gameactivity.h"

// standard includes
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <unordered_set>

// lib includes
#include <nlohmann/json.hpp>

namespace gameactivity {
  namespace fs = std::filesystem;

  namespace {
    // GameActivity writes its start as UTC; one without a zone is UTC too, not the host's time.
    std::int64_t utc_is_local(std::int64_t t) {
      return t;
    }

    std::string utf8(const fs::path &path) {
      const auto u = path.u8string();
      return std::string(u.begin(), u.end());
    }

    fs::path from_utf8(std::string_view text) {
      // A std::string is the ANSI code page to MSVC's std::filesystem; char8_t is UTF-8 everywhere.
      return fs::path(std::u8string(text.begin(), text.end()));
    }

    bool is_guid_like(std::string_view id) {
      if (id.size() != 36) {
        return false;
      }
      for (std::size_t i = 0; i < id.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? id[i] != '-' : !std::isxdigit(static_cast<unsigned char>(id[i]))) {
          return false;
        }
      }
      return true;
    }

    bool is_zero_guid(std::string_view id) {
      return std::all_of(id.begin(), id.end(), [](char c) {
        return c == '0' || c == '-';
      });
    }

    std::optional<std::int64_t> elapsed_of(const nlohmann::json &item) {
      const auto it = item.find("ElapsedSeconds");
      if (it == item.end()) {
        return std::nullopt;
      }
      if (it->is_number_unsigned()) {
        const auto v = it->get<std::uint64_t>();
        return static_cast<std::int64_t>(std::min<std::uint64_t>(v, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
      }
      if (it->is_number_integer()) {
        return it->get<std::int64_t>();
      }
      if (it->is_number_float()) {
        const auto v = it->get<double>();
        return v > 0 && v < 9e18 ? std::optional<std::int64_t> {static_cast<std::int64_t>(v)} : std::optional<std::int64_t> {0};
      }
      return std::nullopt;
    }
    bool is_game_file_name(const fs::path &path, std::string &id) {
      auto ext = utf8(path.extension());
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      if (ext != ".json") {
        return false;
      }
      id = play_stats::normalize_id(utf8(path.stem()));
      return is_guid_like(id);
    }
  }  // namespace

  fs::path resolve_data_dir(std::string_view reported, std::string_view success_story_data) {
    if (!reported.empty()) {
      return from_utf8(reported);
    }
    if (!success_story_data.empty()) {
      auto sibling = from_utf8(success_story_data);
      // "C:\...\ExtensionsData\cebe6d32-...\" has an empty last part: step over it first.
      if (!sibling.has_filename()) {
        sibling = sibling.parent_path();
      }
      return sibling.parent_path() / from_utf8(k_plugin_id);
    }
    return {};
  }

  bool data_found(const fs::path &data_dir) {
    if (data_dir.empty()) {
      return false;
    }
    std::error_code ec;
    fs::directory_iterator it(data_dir / "GameActivity", ec);
    const fs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
      std::string id;
      std::error_code file_ec;
      if (is_game_file_name(it->path(), id) && fs::is_regular_file(it->path(), file_ec)) {
        return true;
      }
    }
    return false;
  }

  std::optional<std::vector<play_stats::session_t>> parse_game(std::string_view text, std::string_view fallback_id) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
      text.remove_prefix(3);
    }
    const auto j = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      return std::nullopt;
    }
    std::string id;
    if (const auto it = j.find("Id"); it != j.end() && it->is_string()) {
      id = play_stats::normalize_id(it->get_ref<const std::string &>());
    }
    if (id.empty() || is_zero_guid(id)) {
      id = play_stats::normalize_id(fallback_id);
    }
    if (id.empty()) {
      return std::nullopt;
    }
    std::vector<play_stats::session_t> out;
    const auto items = j.find("Items");
    if (items == j.end() || !items->is_array()) {
      // A game GameActivity knows but never saw played writes `"Items": []`; null is the same.
      return out;
    }
    out.reserve(items->size());
    for (const auto &item : *items) {
      if (!item.is_object()) {
        continue;
      }
      const auto date = item.find("DateSession");
      if (date == item.end() || !date->is_string()) {
        continue;
      }
      const auto &text_date = date->get_ref<const std::string &>();
      // DateTime's default (year 1) is an Activity that was never started.
      if (play_stats::iso8601_year(text_date) <= 1970) {
        continue;
      }
      const auto start = play_stats::parse_iso8601(text_date, utc_is_local);
      const auto seconds = elapsed_of(item);
      if (!start || !seconds || *seconds <= 0) {
        continue;
      }
      out.push_back({id, *start, *seconds});
    }
    return out;
  }

  store_t::store_t(fs::path data_dir, std::chrono::steady_clock::duration rescan_interval):
      data_dir_(std::move(data_dir)),
      rescan_interval_(rescan_interval) {
  }

  void store_t::rescan_locked() {
    const auto now = std::chrono::steady_clock::now();
    if (scanned_ && now - scanned_at_ < rescan_interval_) {
      return;
    }
    scanned_ = true;
    scanned_at_ = now;
    std::unordered_set<std::string> seen;
    std::error_code ec;
    fs::directory_iterator it(data_dir_ / "GameActivity", ec);
    const fs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
      const auto &path = it->path();
      std::string id;
      if (!is_game_file_name(path, id)) {
        continue;
      }
      std::error_code file_ec;
      if (!fs::is_regular_file(path, file_ec)) {
        continue;
      }
      seen.insert(id);
      const auto written = fs::last_write_time(path, file_ec);
      if (file_ec) {
        continue;  // Keep what we had, if anything.
      }
      const auto size = fs::file_size(path, file_ec);
      const auto cached = cache_.find(id);
      if (cached != cache_.end() && cached->second.written == written && cached->second.size == size) {
        continue;
      }
      std::ifstream in(path, std::ios::binary);
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      std::optional<std::vector<play_stats::session_t>> parsed;
      if (!in.bad()) {
        parsed = parse_game(text, id);
      }
      if (!parsed) {
        // Mid-write (GameActivity rewrites the whole file when a session ends): keep what we had
        // and look again at the next scan, which a changed size or time will trigger.
        continue;
      }
      cache_[id] = cached_t {written, size, std::make_shared<const std::vector<play_stats::session_t>>(std::move(*parsed))};
    }
    found_ = !seen.empty();
    for (auto c = cache_.begin(); c != cache_.end();) {
      c = seen.contains(c->first) ? std::next(c) : cache_.erase(c);
    }
  }

  snapshot_t store_t::snapshot() {
    std::vector<std::shared_ptr<const std::vector<play_stats::session_t>>> files;
    snapshot_t out;
    {
      std::scoped_lock lk(mutex_);
      rescan_locked();
      out.found = found_;
      files.reserve(cache_.size());
      for (const auto &[id, cached] : cache_) {
        if (cached.sessions) {
          files.push_back(cached.sessions);
        }
      }
    }
    std::size_t count = 0;
    for (const auto &f : files) {
      count += f->size();
    }
    out.sessions.reserve(count);
    for (const auto &f : files) {
      out.sessions.insert(out.sessions.end(), f->begin(), f->end());
    }
    // A stable order whatever the cache's: by start, then game.
    std::sort(out.sessions.begin(), out.sessions.end(), [](const play_stats::session_t &a, const play_stats::session_t &b) {
      return a.start != b.start ? a.start < b.start : a.game < b.game;
    });
    return out;
  }

}  // namespace gameactivity
