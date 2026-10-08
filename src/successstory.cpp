/**
 * @file src/successstory.cpp
 * @brief Achievements, read from SuccessStory's data: the pure half of `/appachievements`.
 */

// local includes
#include "successstory.h"

// standard includes
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <unordered_set>

namespace successstory {
  namespace fs = std::filesystem;

  namespace {
    // SuccessStory writes 1982 for "unlocked, no idea when". The dates can only be trusted as far
    // as the source gives them.
    constexpr int k_unknown_year = 1982;
    // Steam's locked icon is often its generic placeholder, a short CDN URL: then the client greys
    // out the real one instead.
    constexpr std::string_view k_steam_generic_host = "steamcdn-a.akamaihd.net";
    constexpr std::size_t k_steam_generic_max_length = 75;

    std::string lower(std::string_view s) {
      std::string out(s);
      std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return out;
    }

    bool is_web(std::string_view url) {
      const auto l = lower(url.substr(0, 8));
      return l.starts_with("http://") || l.starts_with("https://");
    }

    bool is_blank(std::string_view s) {
      return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isspace(c);
      });
    }

    std::string string_field(const nlohmann::json &j, const char *key) {
      const auto it = j.find(key);
      return it != j.end() && it->is_string() ? it->get<std::string>() : std::string {};
    }

    bool bool_field(const nlohmann::json &j, const char *key) {
      const auto it = j.find(key);
      return it != j.end() && it->is_boolean() && it->get<bool>();
    }

    float number_field(const nlohmann::json &j, const char *key, float fallback) {
      const auto it = j.find(key);
      return it != j.end() && it->is_number() ? it->get<float>() : fallback;
    }

    // A GUID-shaped id: hex digits and dashes only. Anything else is never turned into a path.
    bool is_guid_like(std::string_view id) {
      if (id.empty() || id.size() > 64) {
        return false;
      }
      return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isxdigit(c) || c == '-';
      });
    }

    bool is_zero_guid(std::string_view id) {
      return std::all_of(id.begin(), id.end(), [](char c) {
        return c == '0' || c == '-';
      });
    }

    // The path as this OS spells it. SuccessStory writes Windows paths; on other systems (the
    // tests) its backslashes become slashes so the same paths resolve the same way.
    fs::path native_path(std::string_view url) {
      std::string s(url);
#ifndef _WIN32
      std::replace(s.begin(), s.end(), '\\', '/');
#endif
      return utf8_path(s);
    }

    fs::path clean(const fs::path &p) {
      std::error_code ec;
      fs::path out = fs::weakly_canonical(p, ec);
      if (ec) {
        out = p.lexically_normal();
      }
      if (out.has_filename() == false && out.has_parent_path() && out != out.root_path()) {
        out = out.parent_path();
      }
      return out;
    }

    std::vector<std::string> components(const fs::path &p) {
      std::vector<std::string> out;
      for (const auto &part : p) {
        auto text = utf8_string(part);
        if (!text.empty()) {
          out.push_back(lower(text));
        }
      }
      return out;
    }

    // Component by component, ignoring case as Windows does: "C:\data2" is not inside "C:\data",
    // and the root itself is not a file inside it.
    bool is_inside(const fs::path &root, const fs::path &candidate) {
      const auto r = components(root);
      const auto c = components(candidate);
      if (r.empty() || c.size() <= r.size() || !std::equal(r.begin(), r.end(), c.begin())) {
        return false;
      }
      return std::none_of(c.begin() + static_cast<std::ptrdiff_t>(r.size()), c.end(), [](const std::string &part) {
        return part == "..";
      });
    }
  }  // namespace

  std::optional<game_t> parse_game(std::string_view text, std::string_view fallback_id, const play_stats::to_local_t &to_local) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
      text.remove_prefix(3);
    }
    const auto j = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      return std::nullopt;
    }
    game_t game;
    game.id = play_stats::normalize_id(string_field(j, "Id"));
    if (game.id.empty() || is_zero_guid(game.id)) {
      game.id = play_stats::normalize_id(fallback_id);
    }
    if (game.id.empty()) {
      return std::nullopt;
    }
    game.name = string_field(j, "Name");
    game.ignored = bool_field(j, "IsIgnored");
    const auto refreshed = string_field(j, "DateLastRefresh");
    if (play_stats::iso8601_year(refreshed) > 1) {
      game.last_refresh = play_stats::parse_iso8601(refreshed, to_local);
    }
    const auto items = j.find("Items");
    if (items != j.end() && items->is_array()) {
      game.items.reserve(items->size());
      for (const auto &item : *items) {
        if (!item.is_object()) {
          continue;
        }
        achievement_t a;
        a.name = string_field(item, "Name");
        a.api_name = string_field(item, "ApiName");
        a.description = string_field(item, "Description");
        a.url_unlocked = string_field(item, "UrlUnlocked");
        a.url_locked = string_field(item, "UrlLocked");
        a.hidden = bool_field(item, "IsHidden");
        a.percent = number_field(item, "Percent", 100);
        a.gamer_score = number_field(item, "GamerScore", 0);
        a.no_rarety = bool_field(item, "NoRarety");
        // Unlocked when it carries a real date; 0001-01-01 is .NET's "no date". The year is read
        // as written, before any zone conversion, as SuccessStory compares it.
        const auto date = string_field(item, "DateUnlocked");
        const int year = play_stats::iso8601_year(date);
        a.unlocked = year > 1;
        if (a.unlocked && year != k_unknown_year) {
          a.unlocked_at = play_stats::parse_iso8601(date, to_local);
        }
        game.items.push_back(std::move(a));
      }
    }
    return game;
  }

  std::string icon_url(std::string_view url, std::string_view uuid, std::size_t index, bool locked) {
    if (is_blank(url)) {
      return {};
    }
    if (is_web(url)) {
      return std::string(url);
    }
    std::string out = "/appachievementicon?appuuid=";
    out += uuid;
    out += "&index=";
    out += std::to_string(index);
    if (locked) {
      out += "&locked=1";
    }
    return out;
  }

  nlohmann::json item_json(const achievement_t &a, std::size_t index, std::string_view uuid) {
    nlohmann::json out = nlohmann::json::object();
    out["id"] = is_blank(a.api_name) ? std::to_string(index) : a.api_name;
    out["name"] = a.name;
    out["description"] = a.description;
    out["unlocked"] = a.unlocked;
    out["unlocked_at"] = a.unlocked_at ? nlohmann::json(play_stats::format_utc(*a.unlocked_at)) : nlohmann::json(nullptr);
    out["hidden"] = a.hidden;
    // 100 % is SuccessStory's default for "nobody said", so it means unknown too.
    if (a.no_rarety || a.percent >= 100) {
      out["percent"] = nullptr;
    } else {
      out["percent"] = std::round(static_cast<double>(a.percent) * 10.0) / 10.0;
    }
    out["gamer_score"] = a.gamer_score > 0 ? nlohmann::json(static_cast<double>(a.gamer_score)) : nlohmann::json(nullptr);
    const auto icon = icon_url(a.url_unlocked, uuid, index, false);
    out["icon"] = icon.empty() ? nlohmann::json(nullptr) : nlohmann::json(icon);
    std::string_view locked = a.url_locked;
    if (is_blank(locked) || locked == a.url_unlocked ||
        (locked.find(k_steam_generic_host) != std::string_view::npos && locked.size() < k_steam_generic_max_length)) {
      locked = {};
    }
    const auto locked_icon = icon_url(locked, uuid, index, true);
    out["locked_icon"] = locked_icon.empty() ? nlohmann::json(nullptr) : nlohmann::json(locked_icon);
    return out;
  }

  std::optional<nlohmann::json> game_json(const game_t &game, std::string_view uuid) {
    if (game.items.empty() || game.ignored) {
      return std::nullopt;
    }

    struct row_t {
      const achievement_t *a;
      std::size_t index;
    };

    std::vector<row_t> rows;
    rows.reserve(game.items.size());
    std::int64_t unlocked = 0;
    for (std::size_t i = 0; i < game.items.size(); ++i) {
      rows.push_back({&game.items[i], i});
      unlocked += game.items[i].unlocked ? 1 : 0;
    }
    // Unlocked first, newest first (undated ones after the dated); then the locked ones, the most
    // common first, an unknown rarity counting as rarest.
    auto rarity = [](const achievement_t &a) {
      return a.unlocked || a.no_rarety || a.percent >= 100 ? 0.0 : std::round(static_cast<double>(a.percent) * 10.0) / 10.0;
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const row_t &x, const row_t &y) {
      if (x.a->unlocked != y.a->unlocked) {
        return x.a->unlocked;
      }
      const auto xw = x.a->unlocked_at.value_or(std::numeric_limits<std::int64_t>::min());
      const auto yw = y.a->unlocked_at.value_or(std::numeric_limits<std::int64_t>::min());
      if (xw != yw) {
        return xw > yw;
      }
      return rarity(*x.a) > rarity(*y.a);
    });
    nlohmann::json items = nlohmann::json::array();
    for (const auto &row : rows) {
      items.push_back(item_json(*row.a, row.index, uuid));
    }
    nlohmann::json out = nlohmann::json::object();
    out["uuid"] = std::string(uuid);
    out["total"] = static_cast<std::int64_t>(game.items.size());
    out["unlocked"] = unlocked;
    out["last_refresh"] = game.last_refresh ? nlohmann::json(play_stats::format_utc(*game.last_refresh)) : nlohmann::json(nullptr);
    out["items"] = std::move(items);
    return out;
  }

  nlohmann::json unlocks_json(const std::vector<unlock_t> &unlocks, const play_stats::catalogue_t &catalogue, std::size_t limit) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto &u : unlocks) {
      if (out.size() >= limit) {
        break;
      }
      if (!u.game || u.index >= u.game->items.size()) {
        continue;
      }
      const auto it = catalogue.find(u.game->id);
      if (it == catalogue.end()) {
        continue;
      }
      auto item = item_json(u.game->items[u.index], u.index, it->second.uuid);
      item["uuid"] = it->second.uuid;
      item["game"] = it->second.name;
      out.push_back(std::move(item));
    }
    return out;
  }

  std::optional<fs::path> resolve_icon(std::string_view url, const std::vector<fs::path> &roots) {
    if (is_blank(url) || is_web(url)) {
      return std::nullopt;
    }
    // A leading separator means "from the root" to SuccessStory, never the root of the drive.
    const auto first = url.find_first_not_of("\\/");
    if (first == std::string_view::npos) {
      return std::nullopt;
    }
    const fs::path relative = native_path(url.substr(first));
    // "C:icon.png" is relative to a drive's current directory: never anything we can vouch for.
    if (relative.has_root_name() && !relative.is_absolute()) {
      return std::nullopt;
    }
    for (const auto &root : roots) {
      if (root.empty()) {
        continue;
      }
      const fs::path clean_root = clean(root);
      const fs::path candidate = clean(relative.is_absolute() ? relative : clean_root / relative);
      if (!is_inside(clean_root, candidate)) {
        continue;
      }
      std::error_code ec;
      if (fs::is_regular_file(candidate, ec)) {
        return candidate;
      }
    }
    return std::nullopt;
  }

  fs::path utf8_path(std::string_view text) {
    // A std::string is the ANSI code page to MSVC's std::filesystem; char8_t is UTF-8 everywhere.
    std::u8string u8;
    u8.reserve(text.size());
    for (const char c : text) {
      u8.push_back(static_cast<char8_t>(c));
    }
    return fs::path(u8);
  }

  std::string utf8_string(const fs::path &path) {
    const auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
  }

  std::string content_type_for(const fs::path &path) {
    const auto ext = lower(utf8_string(path.extension()));
    if (ext == ".png") {
      return "image/png";
    }
    if (ext == ".jpg" || ext == ".jpeg") {
      return "image/jpeg";
    }
    if (ext == ".gif") {
      return "image/gif";
    }
    if (ext == ".webp") {
      return "image/webp";
    }
    if (ext == ".bmp") {
      return "image/bmp";
    }
    if (ext == ".ico") {
      return "image/x-icon";
    }
    if (ext == ".svg") {
      return "image/svg+xml";
    }
    return "application/octet-stream";
  }

  store_t::store_t(fs::path data_dir, fs::path resources_dir, play_stats::to_local_t to_local, std::chrono::steady_clock::duration rescan_interval):
      data_dir_(std::move(data_dir)),
      resources_dir_(std::move(resources_dir)),
      to_local_(std::move(to_local)),
      rescan_interval_(rescan_interval) {
  }

  fs::path store_t::folder() const {
    return data_dir_ / "SuccessStory";
  }

  std::shared_ptr<const game_t> store_t::load_locked(const std::string &id, const fs::path &path) {
    std::error_code ec;
    const bool exists = fs::is_regular_file(path, ec);
    if (!exists) {
      cache_.erase(id);
      return nullptr;
    }
    const auto written = fs::last_write_time(path, ec);
    if (ec) {
      const auto it = cache_.find(id);
      return it == cache_.end() ? nullptr : it->second.data;
    }
    const auto size = fs::file_size(path, ec);
    const auto it = cache_.find(id);
    if (it != cache_.end() && it->second.written == written && it->second.size == size) {
      return it->second.data;
    }
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::optional<game_t> parsed;
    if (!in.bad()) {
      parsed = parse_game(text, id, to_local_);
    }
    if (!parsed) {
      // Mid-write, or not one of SuccessStory's files: keep what we had and look again next time.
      return it == cache_.end() ? nullptr : it->second.data;
    }
    auto data = std::make_shared<const game_t>(std::move(*parsed));
    cache_[id] = cached_t {written, size, data};
    return data;
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
    fs::directory_iterator it(folder(), ec);
    const fs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
      const auto &path = it->path();
      if (lower(utf8_string(path.extension())) != ".json") {
        continue;
      }
      const auto id = play_stats::normalize_id(utf8_string(path.stem()));
      if (!is_guid_like(id)) {
        continue;
      }
      seen.insert(id);
      load_locked(id, path);
    }
    any_files_ = !seen.empty();
    for (auto c = cache_.begin(); c != cache_.end();) {
      c = seen.contains(c->first) ? std::next(c) : cache_.erase(c);
    }
  }

  bool store_t::available() {
    std::scoped_lock lk(mutex_);
    rescan_locked();
    return any_files_;
  }

  std::shared_ptr<const game_t> store_t::game(std::string_view playnite_id) {
    const auto id = play_stats::normalize_id(playnite_id);
    if (!is_guid_like(id)) {
      return nullptr;
    }
    std::scoped_lock lk(mutex_);
    return load_locked(id, folder() / (id + ".json"));
  }

  std::vector<unlock_t> store_t::unlocked(std::int64_t from_utc, std::int64_t to_utc) {
    std::vector<std::shared_ptr<const game_t>> games;
    {
      std::scoped_lock lk(mutex_);
      rescan_locked();
      games.reserve(cache_.size());
      for (const auto &[id, cached] : cache_) {
        if (cached.data) {
          games.push_back(cached.data);
        }
      }
    }
    std::vector<unlock_t> out;
    for (const auto &g : games) {
      if (g->ignored) {
        continue;
      }
      for (std::size_t i = 0; i < g->items.size(); ++i) {
        const auto &when = g->items[i].unlocked_at;
        if (!when || *when < from_utc || *when >= to_utc) {
          continue;
        }
        out.push_back({g, i, *when});
      }
    }
    // Newest first; the game and position keep the order stable whatever the cache's order.
    std::sort(out.begin(), out.end(), [](const unlock_t &a, const unlock_t &b) {
      if (a.when != b.when) {
        return a.when > b.when;
      }
      if (a.game->id != b.game->id) {
        return a.game->id < b.game->id;
      }
      return a.index < b.index;
    });
    return out;
  }

  std::optional<fs::path> store_t::icon_path(std::string_view playnite_id, std::size_t index, bool locked) {
    const auto data = game(playnite_id);
    if (!data || index >= data->items.size()) {
      return std::nullopt;
    }
    const auto &item = data->items[index];
    // RPCS3's and ShadPS4's icons live under the data folder, a few games' under the extension's
    // own Resources. Either way nothing outside those two.
    return resolve_icon(locked ? item.url_locked : item.url_unlocked, {data_dir_, resources_dir_});
  }

}  // namespace successstory
