/**
 * @file src/game_metadata_policy.cpp
 * @brief Provider-neutral app metadata keys and the pure IGDB mirror matching rules.
 */
#include "game_metadata_policy.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unordered_set>

namespace game_metadata {

  namespace {
    // Keys written before the metadata became provider-neutral. Read for compatibility and
    // erased whenever a provider rewrites the corresponding field.
    constexpr std::array<std::pair<const char *, const char *>, 10> legacy_key_map {{
      {"playnite-description", keys::description},
      {"playnite-genres", keys::genres},
      {"playnite-developers", keys::developers},
      {"playnite-publishers", keys::publishers},
      {"playnite-release-date", keys::release_date},
      {"playnite-community-score", keys::community_score},
      {"playnite-critic-score", keys::critic_score},
      {"playnite-last-played", keys::last_played},
      {"playnite-playtime-minutes", keys::playtime_minutes},
      {"playnite-background", keys::background},
    }};

    constexpr std::array<const char *, 8> descriptive_keys {
      keys::description,
      keys::genres,
      keys::developers,
      keys::publishers,
      keys::release_date,
      keys::community_score,
      keys::critic_score,
      keys::background,
    };

    const char *legacy_key_for(const char *key) {
      for (const auto &[legacy, current] : legacy_key_map) {
        if (std::string_view {current} == key) {
          return legacy;
        }
      }
      return nullptr;
    }

    // Value of `key`, falling back to its legacy spelling. Records whether the value came from
    // a legacy key so the source can be attributed to Playnite.
    const nlohmann::json *find_value(const nlohmann::json &app, const char *key, bool &legacy_hit) {
      if (auto it = app.find(key); it != app.end() && !it->is_null()) {
        return &*it;
      }
      if (const char *legacy = legacy_key_for(key)) {
        if (auto it = app.find(legacy); it != app.end() && !it->is_null()) {
          legacy_hit = true;
          return &*it;
        }
      }
      return nullptr;
    }

    std::string read_string(const nlohmann::json &app, const char *key, bool &legacy_hit) {
      const auto *value = find_value(app, key, legacy_hit);
      return value && value->is_string() ? value->get<std::string>() : std::string {};
    }

    std::vector<std::string> read_list(const nlohmann::json &app, const char *key, bool &legacy_hit) {
      std::vector<std::string> out;
      const auto *value = find_value(app, key, legacy_hit);
      if (value && value->is_array()) {
        for (const auto &item : *value) {
          if (item.is_string()) {
            out.push_back(item.get<std::string>());
          }
        }
      }
      return out;
    }

    int read_score(const nlohmann::json &app, const char *key, bool &legacy_hit) {
      const auto *value = find_value(app, key, legacy_hit);
      if (value && value->is_number()) {
        const auto score = value->get<int>();
        return score >= 0 ? std::min(score, 100) : -1;
      }
      return -1;
    }

    uint64_t read_uint(const nlohmann::json &app, const char *key, bool &legacy_hit) {
      const auto *value = find_value(app, key, legacy_hit);
      if (!value || !value->is_number_integer()) {
        return 0;
      }
      const auto number = value->get<std::int64_t>();
      return number > 0 ? static_cast<uint64_t>(number) : 0;
    }

    void erase_key_and_legacy(nlohmann::json &app, const char *key) {
      app.erase(key);
      if (const char *legacy = legacy_key_for(key)) {
        app.erase(legacy);
      }
    }

    void set_or_erase(nlohmann::json &app, const char *key, const std::string &value) {
      erase_key_and_legacy(app, key);
      if (!value.empty()) {
        app[key] = value;
      }
    }

    void set_or_erase(nlohmann::json &app, const char *key, const std::vector<std::string> &values) {
      erase_key_and_legacy(app, key);
      if (!values.empty()) {
        app[key] = values;
      }
    }

    void set_or_erase_score(nlohmann::json &app, const char *key, int score) {
      erase_key_and_legacy(app, key);
      if (score >= 0) {
        app[key] = std::min(score, 100);
      }
    }

    bool is_ascii_alnum(unsigned char ch) {
      return std::isalnum(ch) != 0 && ch < 0x80;
    }

    std::string trim(std::string_view value) {
      const auto is_space = [](unsigned char ch) {
        return std::isspace(ch) != 0;
      };
      while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
      }
      while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
      }
      return std::string {value};
    }

    void push_unique(std::vector<std::string> &values, std::string value) {
      value = trim(value);
      if (value.empty() || std::find(values.begin(), values.end(), value) != values.end()) {
        return;
      }
      values.push_back(std::move(value));
    }

    int rating_to_score(const nlohmann::json &value) {
      if (!value.is_number()) {
        return -1;
      }
      const double rating = value.get<double>();
      if (!std::isfinite(rating) || rating < 0) {
        return -1;
      }
      return static_cast<int>(std::min<long>(100, std::lround(rating)));
    }
  }  // namespace

  metadata_t read_app_metadata(const nlohmann::json &app) {
    metadata_t meta;
    if (!app.is_object()) {
      return meta;
    }
    bool legacy_hit = false;
    meta.description = read_string(app, keys::description, legacy_hit);
    meta.genres = read_list(app, keys::genres, legacy_hit);
    meta.developers = read_list(app, keys::developers, legacy_hit);
    meta.publishers = read_list(app, keys::publishers, legacy_hit);
    meta.release_date = read_string(app, keys::release_date, legacy_hit);
    meta.community_score = read_score(app, keys::community_score, legacy_hit);
    meta.critic_score = read_score(app, keys::critic_score, legacy_hit);
    meta.last_played = read_string(app, keys::last_played, legacy_hit);
    meta.playtime_minutes = read_uint(app, keys::playtime_minutes, legacy_hit);
    meta.background_image_path = read_string(app, keys::background, legacy_hit);

    bool unused = false;
    meta.source = read_string(app, keys::source, unused);
    meta.igdb_id = read_string(app, keys::igdb_id, unused);
    if (meta.source.empty() && legacy_hit) {
      meta.source = source_playnite;
    }

    meta.present = !meta.description.empty() || !meta.genres.empty() ||
                   !meta.developers.empty() || !meta.publishers.empty() ||
                   !meta.release_date.empty() || meta.community_score >= 0 ||
                   meta.critic_score >= 0 || !meta.last_played.empty() ||
                   meta.playtime_minutes > 0 || !meta.background_image_path.empty();
    return meta;
  }

  void write_descriptive(nlohmann::json &app, const descriptive_t &fields, std::string_view source, std::string_view igdb_id) {
    set_or_erase(app, keys::description, fields.description);
    set_or_erase(app, keys::genres, fields.genres);
    set_or_erase(app, keys::developers, fields.developers);
    set_or_erase(app, keys::publishers, fields.publishers);
    set_or_erase(app, keys::release_date, fields.release_date);
    set_or_erase_score(app, keys::community_score, fields.community_score);
    set_or_erase_score(app, keys::critic_score, fields.critic_score);
    set_or_erase(app, keys::background, fields.background_path);
    set_or_erase(app, keys::source, std::string {source});
    set_or_erase(app, keys::igdb_id, std::string {igdb_id});
  }

  void write_activity(nlohmann::json &app, const std::string &last_played, uint64_t playtime_minutes) {
    set_or_erase(app, keys::last_played, last_played);
    // Zero playtime is the same as none for a client ordering a library, so it is erased rather
    // than written, keeping apps.json free of a key on every never-played game.
    erase_key_and_legacy(app, keys::playtime_minutes);
    if (playtime_minutes > 0) {
      app[keys::playtime_minutes] = playtime_minutes;
    }
  }

  bool has_descriptive_metadata(const nlohmann::json &app) {
    if (!app.is_object()) {
      return false;
    }
    for (const char *key : descriptive_keys) {
      bool unused = false;
      if (find_value(app, key, unused)) {
        return true;
      }
    }
    return false;
  }

  std::string bucket_key(std::string_view title) {
    std::string key;
    std::size_t consumed = 0;
    for (const unsigned char ch : title) {
      if (consumed++ >= 2) {
        break;
      }
      if (is_ascii_alnum(ch)) {
        key.push_back(static_cast<char>(std::tolower(ch)));
      }
    }
    return key.empty() ? std::string {"@"} : key;
  }

  std::string bucket_url(std::string_view key) {
    return std::string {gamedb_base_url} + "/buckets/" + std::string {key} + ".json";
  }

  std::string game_url(std::string_view id) {
    return std::string {gamedb_base_url} + "/games/" + std::string {id} + ".json";
  }

  std::string image_url(std::string_view size, std::string_view slug) {
    return std::string {igdb_image_base_url} + '/' + std::string {size} + '/' + std::string {slug} + ".png";
  }

  std::string normalize_title(std::string_view title) {
    std::string normalized;
    normalized.reserve(title.size());
    for (const unsigned char ch : title) {
      if (is_ascii_alnum(ch)) {
        normalized.push_back(static_cast<char>(std::tolower(ch)));
      }
    }
    return normalized;
  }

  std::string strip_trailing_parenthetical(std::string_view title) {
    const auto trimmed = trim(title);
    if (trimmed.empty() || trimmed.back() != ')') {
      return trimmed;
    }
    const auto open = trimmed.rfind('(');
    if (open == std::string::npos || open == 0) {
      return trimmed;
    }
    return trim(std::string_view {trimmed}.substr(0, open));
  }

  std::vector<bucket_entry_t> parse_bucket(const nlohmann::json &bucket) {
    std::vector<bucket_entry_t> entries;
    if (!bucket.is_object()) {
      return entries;
    }
    for (const auto &[id, value] : bucket.items()) {
      if (!value.is_object()) {
        continue;
      }
      const auto name = value.find("name");
      if (name == value.end() || !name->is_string() || name->get<std::string>().empty()) {
        continue;
      }
      entries.push_back({id, name->get<std::string>()});
    }
    return entries;
  }

  namespace {
    bool id_less(const std::string &lhs, const std::string &rhs) {
      // IGDB ids are decimal; a shorter numeric string is the smaller id.
      if (lhs.size() != rhs.size()) {
        return lhs.size() < rhs.size();
      }
      return lhs < rhs;
    }

    // Lower-cased alphanumeric words of a title, in order.
    std::vector<std::string> title_words(std::string_view title) {
      std::vector<std::string> words;
      std::string current;
      for (const unsigned char ch : title) {
        if (is_ascii_alnum(ch)) {
          current.push_back(static_cast<char>(std::tolower(ch)));
        } else if (!current.empty()) {
          words.push_back(std::move(current));
          current.clear();
        }
      }
      if (!current.empty()) {
        words.push_back(std::move(current));
      }
      return words;
    }

    std::optional<match_t> match_exact(std::string_view title, const std::vector<bucket_entry_t> &entries) {
      const auto wanted = normalize_title(title);
      if (wanted.empty()) {
        return std::nullopt;
      }
      const bucket_entry_t *best = nullptr;
      bool best_is_verbatim = false;
      std::size_t exact_candidates = 0;
      for (const auto &entry : entries) {
        if (normalize_title(entry.name) != wanted) {
          continue;
        }
        ++exact_candidates;
        const bool verbatim = trim(entry.name) == trim(title);
        if (!best || (verbatim && !best_is_verbatim) || (verbatim == best_is_verbatim && id_less(entry.id, best->id))) {
          best = &entry;
          best_is_verbatim = verbatim;
        }
      }
      if (!best) {
        return std::nullopt;
      }
      return match_t {match_kind_e::exact, best->id, best->name, exact_candidates};
    }
  }  // namespace

  match_t select_match(std::string_view title, const std::vector<bucket_entry_t> &entries) {
    if (auto exact = match_exact(title, entries)) {
      return *exact;
    }
    const auto stripped = strip_trailing_parenthetical(title);
    if (stripped != trim(title)) {
      if (auto exact = match_exact(stripped, entries)) {
        return *exact;
      }
    }

    // The prefix rule works on words so "Hades I" cannot pick "Hades II".
    const auto wanted = title_words(stripped);
    if (wanted.empty()) {
      return {};
    }
    const bucket_entry_t *only = nullptr;
    for (const auto &entry : entries) {
      const auto words = title_words(entry.name);
      if (words.size() <= wanted.size() || !std::equal(wanted.begin(), wanted.end(), words.begin())) {
        continue;
      }
      if (only) {
        return {};
      }
      only = &entry;
    }
    if (!only) {
      return {};
    }
    return match_t {match_kind_e::unique_prefix, only->id, only->name, 0};
  }

  std::string image_slug(std::string_view url) {
    const auto query = url.find_first_of("?#");
    if (query != std::string_view::npos) {
      url = url.substr(0, query);
    }
    const auto slash = url.find_last_of('/');
    if (slash != std::string_view::npos) {
      url = url.substr(slash + 1);
    }
    const auto dot = url.find_last_of('.');
    if (dot != std::string_view::npos) {
      url = url.substr(0, dot);
    }
    // Slugs are path segments we later place in a URL and a file name; refuse anything else.
    for (const unsigned char ch : url) {
      if (!is_ascii_alnum(ch) && ch != '_' && ch != '-') {
        return {};
      }
    }
    return std::string {url};
  }

  std::string format_release_date(std::int64_t unix_seconds) {
    const std::chrono::sys_seconds time {std::chrono::seconds {unix_seconds}};
    const std::chrono::year_month_day ymd {std::chrono::floor<std::chrono::days>(time)};
    if (!ymd.ok()) {
      return {};
    }
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u", static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
    return buffer;
  }

  std::optional<igdb_game_t> parse_game(const nlohmann::json &game) {
    if (!game.is_object()) {
      return std::nullopt;
    }
    igdb_game_t out;
    if (const auto id = game.find("id"); id != game.end()) {
      if (id->is_number_integer()) {
        out.id = std::to_string(id->get<std::int64_t>());
      } else if (id->is_string()) {
        out.id = id->get<std::string>();
      }
    }
    if (const auto name = game.find("name"); name != game.end() && name->is_string()) {
      out.name = trim(name->get<std::string>());
    }
    if (out.id.empty() || out.name.empty()) {
      return std::nullopt;
    }

    if (const auto summary = game.find("summary"); summary != game.end() && summary->is_string()) {
      out.summary = trim(summary->get<std::string>());
    }
    if (const auto genres = game.find("genres"); genres != game.end() && genres->is_array()) {
      for (const auto &genre : *genres) {
        if (genre.is_object() && genre.contains("name") && genre["name"].is_string()) {
          push_unique(out.genres, genre["name"].get<std::string>());
        }
      }
    }
    // The mirror only carries the `developer` flag of an involvement, so every other involved
    // company is treated as a publisher.
    if (const auto companies = game.find("involved_companies"); companies != game.end() && companies->is_array()) {
      for (const auto &involvement : *companies) {
        if (!involvement.is_object()) {
          continue;
        }
        const auto company = involvement.find("company");
        if (company == involvement.end() || !company->is_object() || !company->contains("name") || !(*company)["name"].is_string()) {
          continue;
        }
        const bool developer = involvement.value("developer", false);
        push_unique(developer ? out.developers : out.publishers, (*company)["name"].get<std::string>());
      }
    }

    std::optional<std::int64_t> earliest_date;
    std::optional<int> earliest_year;
    if (const auto dates = game.find("release_dates"); dates != game.end() && dates->is_array()) {
      for (const auto &release : *dates) {
        if (!release.is_object()) {
          continue;
        }
        if (const auto date = release.find("date"); date != release.end() && date->is_number_integer()) {
          const auto value = date->get<std::int64_t>();
          if (!earliest_date || value < *earliest_date) {
            earliest_date = value;
          }
        }
        if (const auto year = release.find("y"); year != release.end() && year->is_number_integer()) {
          const auto value = year->get<int>();
          if (!earliest_year || value < *earliest_year) {
            earliest_year = value;
          }
        }
      }
    }
    if (earliest_date) {
      out.release_date = format_release_date(*earliest_date);
    }
    if (out.release_date.empty() && earliest_year && *earliest_year > 0) {
      out.release_date = std::to_string(*earliest_year);
    }

    out.critic_score = rating_to_score(game.value("aggregated_rating", nlohmann::json {}));
    out.community_score = rating_to_score(game.value("rating", nlohmann::json {}));

    if (const auto cover = game.find("cover"); cover != game.end() && cover->is_object() && cover->contains("url") && (*cover)["url"].is_string()) {
      out.cover_slug = image_slug((*cover)["url"].get<std::string>());
    }
    for (const char *collection : {"artworks", "screenshots"}) {
      const auto images = game.find(collection);
      if (images == game.end() || !images->is_array()) {
        continue;
      }
      for (const auto &image : *images) {
        if (image.is_object() && image.contains("url") && image["url"].is_string()) {
          out.artwork_slug = image_slug(image["url"].get<std::string>());
          if (!out.artwork_slug.empty()) {
            break;
          }
        }
      }
      if (!out.artwork_slug.empty()) {
        break;
      }
    }
    return out;
  }

  descriptive_t to_descriptive(const igdb_game_t &game, std::string background_path) {
    descriptive_t fields;
    fields.description = game.summary;
    fields.genres = game.genres;
    fields.developers = game.developers;
    fields.publishers = game.publishers;
    fields.release_date = game.release_date;
    fields.community_score = game.community_score;
    fields.critic_score = game.critic_score;
    fields.background_path = std::move(background_path);
    return fields;
  }

}  // namespace game_metadata
