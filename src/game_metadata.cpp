/**
 * @file src/game_metadata.cpp
 * @brief Provider-agnostic game metadata: apps.json (de)serialization and store identity.
 */

// local includes
#include "game_metadata.h"

// standard includes
#include <algorithm>
#include <array>
#include <cctype>

namespace metadata {
  namespace {
    // Canonical keys, and the Playnite-era spelling each one replaces. Reads accept both so an
    // apps.json written before the split keeps its metadata; writes emit only the first and
    // erase the second.
    struct key_pair_t {
      const char *current;
      const char *legacy;
    };

    constexpr key_pair_t k_description {"meta-description", "playnite-description"};
    constexpr key_pair_t k_genres {"meta-genres", "playnite-genres"};
    constexpr key_pair_t k_developers {"meta-developers", "playnite-developers"};
    constexpr key_pair_t k_publishers {"meta-publishers", "playnite-publishers"};
    constexpr key_pair_t k_release_date {"meta-release-date", "playnite-release-date"};
    constexpr key_pair_t k_community_score {"meta-community-score", "playnite-community-score"};
    constexpr key_pair_t k_critic_score {"meta-critic-score", "playnite-critic-score"};
    constexpr key_pair_t k_last_played {"meta-last-played", "playnite-last-played"};
    constexpr key_pair_t k_playtime {"meta-playtime-minutes", "playnite-playtime-minutes"};
    constexpr key_pair_t k_background {"meta-background", "playnite-background"};

    // These have no Playnite-era spelling; they arrived with the provider split.
    constexpr const char *k_source = "meta-source";
    constexpr const char *k_igdb_id = "meta-igdb-id";
    constexpr const char *k_locked = "meta-locked";

    const nlohmann::json *find_value(const nlohmann::json &app, const key_pair_t &keys) {
      if (auto it = app.find(keys.current); it != app.end() && !it->is_null()) {
        return &(*it);
      }
      if (auto it = app.find(keys.legacy); it != app.end() && !it->is_null()) {
        return &(*it);
      }
      return nullptr;
    }

    std::string read_string(const nlohmann::json &app, const key_pair_t &keys) {
      const auto *value = find_value(app, keys);
      return value && value->is_string() ? value->get<std::string>() : std::string {};
    }

    std::vector<std::string> read_list(const nlohmann::json &app, const key_pair_t &keys) {
      std::vector<std::string> out;
      const auto *value = find_value(app, keys);
      if (!value || !value->is_array()) {
        return out;
      }
      for (const auto &entry : *value) {
        if (entry.is_string()) {
          out.push_back(entry.get<std::string>());
        }
      }
      return out;
    }

    int read_score(const nlohmann::json &app, const key_pair_t &keys) {
      const auto *value = find_value(app, keys);
      return value && value->is_number() ? value->get<int>() : -1;
    }

    uint64_t read_uint(const nlohmann::json &app, const key_pair_t &keys) {
      const auto *value = find_value(app, keys);
      if (!value) {
        return 0;
      }
      if (value->is_number_unsigned()) {
        return value->get<uint64_t>();
      }
      // A node built in memory rather than parsed holds a positive count as a signed integer,
      // which is the same number and has to read back as one.
      if (value->is_number_integer()) {
        const auto signed_value = value->get<std::int64_t>();
        return signed_value > 0 ? static_cast<uint64_t>(signed_value) : 0;
      }
      return 0;
    }

    void erase_both(nlohmann::json &app, const key_pair_t &keys) {
      app.erase(keys.current);
      app.erase(keys.legacy);
    }

    template<class T>
    void set_or_erase(nlohmann::json &app, const key_pair_t &keys, const T &value, bool keep) {
      // The legacy key goes either way: keeping it would leave two spellings of one value, and
      // the next read would have to guess which one is current.
      app.erase(keys.legacy);
      if (keep) {
        app[keys.current] = value;
      } else {
        app.erase(keys.current);
      }
    }

    std::string lower(std::string text) {
      std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return text;
    }

    std::string app_string(const nlohmann::json &app, const char *key) {
      if (auto it = app.find(key); it != app.end() && it->is_string()) {
        return it->get<std::string>();
      }
      return {};
    }

    // Numbers are accepted as well as strings: steam-id has been written both ways over the
    // life of apps.json, and a store id read back as a number is still the same id.
    std::string app_id_string(const nlohmann::json &app, const char *key) {
      auto it = app.find(key);
      if (it == app.end()) {
        return {};
      }
      if (it->is_string()) {
        return it->get<std::string>();
      }
      if (it->is_number_unsigned()) {
        return std::to_string(it->get<std::uint64_t>());
      }
      if (it->is_number_integer()) {
        const auto value = it->get<std::int64_t>();
        return value >= 0 ? std::to_string(value) : std::string {};
      }
      return {};
    }

    void push_unique(std::vector<store_id_t> &out, store_id_t candidate) {
      if (candidate.store.empty() || candidate.id.empty()) {
        return;
      }
      if (std::find(out.begin(), out.end(), candidate) != out.end()) {
        return;
      }
      out.push_back(std::move(candidate));
    }
  }  // namespace

  bool has_any_value(const game_metadata_t &meta) {
    return !meta.description.empty() || !meta.genres.empty() || !meta.developers.empty() ||
           !meta.publishers.empty() || !meta.release_date.empty() || meta.community_score >= 0 ||
           meta.critic_score >= 0 || !meta.last_played.empty() || meta.playtime_minutes > 0 ||
           !meta.background_image_path.empty();
  }

  game_metadata_t read_from_app(const nlohmann::json &app) {
    game_metadata_t meta;
    if (!app.is_object()) {
      return meta;
    }
    meta.description = read_string(app, k_description);
    meta.genres = read_list(app, k_genres);
    meta.developers = read_list(app, k_developers);
    meta.publishers = read_list(app, k_publishers);
    meta.release_date = read_string(app, k_release_date);
    meta.community_score = read_score(app, k_community_score);
    meta.critic_score = read_score(app, k_critic_score);
    meta.last_played = read_string(app, k_last_played);
    meta.playtime_minutes = read_uint(app, k_playtime);
    meta.background_image_path = read_string(app, k_background);

    meta.source = app_string(app, k_source);
    meta.igdb_id = app_id_string(app, k_igdb_id);
    if (auto it = app.find(k_locked); it != app.end() && it->is_boolean()) {
      meta.locked = it->get<bool>();
    }
    // An app that predates the split carries Playnite's keys and no source; saying so here
    // means the resolver can tell "nobody claimed this" from "Playnite owns this".
    if (meta.source.empty() && has_any_value(meta) && app.contains(k_description.legacy)) {
      meta.source = "playnite";
    }
    meta.present = has_any_value(meta);
    return meta;
  }

  void write_to_app(nlohmann::json &app, const game_metadata_t &meta) {
    if (!app.is_object()) {
      return;
    }
    set_or_erase(app, k_description, meta.description, !meta.description.empty());
    set_or_erase(app, k_genres, meta.genres, !meta.genres.empty());
    set_or_erase(app, k_developers, meta.developers, !meta.developers.empty());
    set_or_erase(app, k_publishers, meta.publishers, !meta.publishers.empty());
    set_or_erase(app, k_release_date, meta.release_date, !meta.release_date.empty());
    set_or_erase(app, k_community_score, meta.community_score, meta.community_score >= 0);
    set_or_erase(app, k_critic_score, meta.critic_score, meta.critic_score >= 0);
    set_or_erase(app, k_last_played, meta.last_played, !meta.last_played.empty());
    // Zero playtime is the same as none for a client ordering a library, so it is erased rather
    // than written, keeping apps.json free of a key on every never-played game.
    set_or_erase(app, k_playtime, meta.playtime_minutes, meta.playtime_minutes > 0);
    set_or_erase(app, k_background, meta.background_image_path, !meta.background_image_path.empty());

    if (!meta.source.empty()) {
      app[k_source] = meta.source;
    } else {
      app.erase(k_source);
    }
    if (!meta.igdb_id.empty()) {
      app[k_igdb_id] = meta.igdb_id;
    } else {
      app.erase(k_igdb_id);
    }
    if (meta.locked) {
      app[k_locked] = true;
    } else {
      app.erase(k_locked);
    }
  }

  std::string normalize_store_name(const std::string &name) {
    const auto text = lower(name);
    if (text.empty()) {
      return {};
    }
    // Substring matching, because every layer spells these differently: Playnite reports the
    // plugin's display name ("GOG", "Epic Games Store"), Lutris its service slug ("gog",
    // "egs"), and neither is stable enough to match whole.
    const auto has = [&text](const char *needle) {
      return text.find(needle) != std::string::npos;
    };
    if (has("steam")) return "steam";
    if (has("gog")) return "gog";
    if (has("epic") || text == "egs") return "epic";
    if (has("ubisoft") || has("uplay")) return "ubisoft";
    if (has("origin") || has("ea app") || text == "ea") return "origin";
    if (has("battle") || has("blizzard")) return "battlenet";
    if (has("itch")) return "itch";
    if (has("xbox") || has("microsoft")) return "microsoft";
    if (has("amazon")) return "amazon";
    return {};
  }

  std::vector<store_id_t> store_ids_of(const nlohmann::json &app) {
    std::vector<store_id_t> out;
    if (!app.is_object()) {
      return out;
    }
    push_unique(out, {"steam", app_id_string(app, "steam-id")});

    // Lutris records what a game is on top of where it was installed from. The service id is
    // the store's own id, which is the only part IGDB can match; lutris-id identifies the
    // local install and means nothing outside this machine.
    const auto lutris_store = normalize_store_name(app_string(app, "lutris-service"));
    push_unique(out, {lutris_store, app_id_string(app, "lutris-service-id")});

    // Playnite games carry the owning library plugin's display name. A Playnite game id is
    // likewise local-only, so a game whose plugin we cannot place has no store identity.
    const auto playnite_store = normalize_store_name(app_string(app, "playnite-source"));
    push_unique(out, {playnite_store, app_id_string(app, "playnite-source-id")});
    return out;
  }

}  // namespace metadata
