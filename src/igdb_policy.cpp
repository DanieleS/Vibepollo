/**
 * @file src/igdb_policy.cpp
 * @brief Portable half of the IGDB integration: query bodies, response parsing, pacing.
 */

// local includes
#include "igdb_policy.h"

// standard includes
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <sstream>

// lib includes
#include <nlohmann/json.hpp>

namespace igdb::policy {
  namespace {
    // The retired external_games.category numbering, kept only as a fallback for a host that
    // cannot reach the sources endpoint. Only the stores a PC library actually draws from are
    // listed; the rest of the enum was consoles and video sites. Ubisoft Connect, the EA app
    // and Battle.net were never in it at all, which is why their games fall back to a title
    // search however the source ids are obtained.
    struct category_t {
      const char *store;
      int category;
    };

    constexpr std::array k_legacy_categories {
      category_t {"steam", 1},
      category_t {"gog", 5},
      category_t {"microsoft", 11},
      category_t {"epic", 26},
      category_t {"itch", 30},
      category_t {"amazon", 20},
    };

    // Trailing words that name an edition rather than a game. IGDB indexes the plain title, so
    // leaving these on would miss a match that is obviously right; the risk is the reverse,
    // matching a remaster to the original, which we accept because both carry the same
    // summary, developer and genres and only the cover would be slightly off.
    constexpr std::array k_edition_suffixes {
      "game of the year edition",
      "game of the year",
      "definitive edition",
      "complete edition",
      "enhanced edition",
      "ultimate edition",
      "deluxe edition",
      "special edition",
      "anniversary edition",
      "remastered edition",
      "directors cut",
      "remastered",
      "goty edition",
      "goty",
    };

    std::string escape_apicalypse(const std::string &value) {
      std::string out;
      out.reserve(value.size() + 8);
      for (const char c : value) {
        // Only the quote and the backslash can end or extend the string literal we are
        // building; everything else, including newlines, is harmless inside one.
        if (c == '"' || c == '\\') {
          out.push_back('\\');
        }
        out.push_back(c);
      }
      return out;
    }

    std::string json_string(const nlohmann::json &node, const char *key) {
      if (auto it = node.find(key); it != node.end() && it->is_string()) {
        return it->get<std::string>();
      }
      return {};
    }

    // IGDB returns ids as numbers but we carry them as strings everywhere else, because they
    // end up in apps.json next to store ids that were never numeric to begin with.
    std::string json_id(const nlohmann::json &node, const char *key) {
      auto it = node.find(key);
      if (it == node.end()) {
        return {};
      }
      if (it->is_number_unsigned()) {
        return std::to_string(it->get<std::uint64_t>());
      }
      if (it->is_number_integer()) {
        const auto value = it->get<std::int64_t>();
        return value >= 0 ? std::to_string(value) : std::string {};
      }
      if (it->is_string()) {
        return it->get<std::string>();
      }
      // An expanded reference: `game` comes back as an object when a query asks for its fields.
      if (it->is_object()) {
        return json_id(*it, "id");
      }
      return {};
    }

    int json_score(const nlohmann::json &node, const char *key) {
      auto it = node.find(key);
      if (it == node.end() || !it->is_number()) {
        return -1;
      }
      const auto value = it->get<double>();
      if (!std::isfinite(value) || value < 0.0) {
        return -1;
      }
      return static_cast<int>(std::lround(std::min(value, 100.0)));
    }

    std::string collapse_whitespace(const std::string &text) {
      std::string out;
      out.reserve(text.size());
      bool pending_space = false;
      for (const char c : text) {
        if (c == ' ') {
          pending_space = !out.empty();
          continue;
        }
        if (pending_space) {
          out.push_back(' ');
          pending_space = false;
        }
        out.push_back(c);
      }
      return out;
    }
  }  // namespace

  std::string external_sources_query() {
    // The list is short and fully enumerable; taking it whole means one request covers every
    // store a library might draw from, now and after IGDB adds another.
    return "fields id,name; limit 500;";
  }

  source_map_t parse_external_sources(const std::string &body) {
    source_map_t sources;
    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (!parsed.is_array()) {
      return sources;
    }
    for (const auto &entry : parsed) {
      if (!entry.is_object()) {
        continue;
      }
      const auto name = json_string(entry, "name");
      auto id = entry.find("id");
      if (name.empty() || id == entry.end() || !id->is_number_integer()) {
        continue;
      }
      // The same normalizer the providers go through, so "Epic Games Store" from IGDB and
      // "egs" from Lutris end up as the same slug without a second table to keep in step.
      const auto store = metadata::normalize_store_name(name);
      if (store.empty()) {
        continue;
      }
      // First wins: IGDB lists regional storefronts under similar names, and the lowest id is
      // the general one.
      sources.emplace(store, id->get<int>());
    }
    return sources;
  }

  source_map_t legacy_source_map() {
    source_map_t sources;
    for (const auto &entry : k_legacy_categories) {
      sources.emplace(entry.store, entry.category);
    }
    return sources;
  }

  std::string external_lookup_query(const std::vector<metadata::store_id_t> &ids,
                                    const source_map_t &sources,
                                    bool legacy_field) {
    std::ostringstream clauses;
    bool first = true;
    for (const auto &id : ids) {
      const auto source = sources.find(id.store);
      if (source == sources.end() || id.id.empty()) {
        continue;
      }
      if (!first) {
        clauses << " | ";
      }
      first = false;
      clauses << "(uid = \"" << escape_apicalypse(id.id) << "\" & "
              << (legacy_field ? "category" : "external_game_source") << " = " << source->second << ")";
    }
    if (first) {
      return {};
    }
    std::ostringstream query;
    query << "fields game,uid; where " << clauses.str() << "; limit 50;";
    return query.str();
  }

  std::string games_by_id_query(const std::vector<std::string> &igdb_ids) {
    std::ostringstream list;
    bool first = true;
    for (const auto &id : igdb_ids) {
      if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) {
        continue;
      }
      if (!first) {
        list << ',';
      }
      first = false;
      list << id;
    }
    if (first) {
      return {};
    }
    std::ostringstream query;
    query << "fields id,name,summary,first_release_date,rating,aggregated_rating,"
             "genres.name,cover.image_id,artworks.image_id,"
             "involved_companies.developer,involved_companies.publisher,involved_companies.company.name; "
          << "where id = (" << list.str() << "); limit 50;";
    return query.str();
  }

  std::string search_query(const std::string &name, int limit) {
    if (name.empty()) {
      return {};
    }
    const auto capped = std::clamp(limit, 1, 50);
    std::ostringstream query;
    // `search` already orders by relevance, so no sort clause. Versions and DLC are excluded:
    // a library entry is the game itself, never an expansion record that shares its name.
    query << "search \"" << escape_apicalypse(name) << "\"; "
          << "fields id,name,summary,first_release_date,rating,aggregated_rating,"
             "genres.name,cover.image_id,artworks.image_id,"
             "involved_companies.developer,involved_companies.publisher,involved_companies.company.name; "
          << "where version_parent = null; limit " << capped << ";";
    return query.str();
  }

  std::vector<match_t> parse_external_matches(const std::string &body) {
    std::vector<match_t> out;
    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (!parsed.is_array()) {
      return out;
    }
    for (const auto &entry : parsed) {
      if (!entry.is_object()) {
        continue;
      }
      match_t match;
      match.igdb_id = json_id(entry, "game");
      match.store_id = json_string(entry, "uid");
      if (match.igdb_id.empty()) {
        continue;
      }
      out.push_back(std::move(match));
    }
    return out;
  }

  std::vector<game_t> parse_games(const std::string &body) {
    std::vector<game_t> out;
    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (!parsed.is_array()) {
      return out;
    }
    for (const auto &entry : parsed) {
      if (!entry.is_object()) {
        continue;
      }
      game_t game;
      game.igdb_id = json_id(entry, "id");
      if (game.igdb_id.empty()) {
        continue;
      }
      game.name = json_string(entry, "name");
      game.summary = json_string(entry, "summary");
      if (auto it = entry.find("first_release_date"); it != entry.end() && it->is_number_integer()) {
        game.first_release_date = it->get<std::int64_t>();
      }
      // IGDB separates the players' score from the press'. We keep both, under the names the
      // rest of the host already uses for Playnite's equivalents.
      game.community_score = json_score(entry, "rating");
      game.critic_score = json_score(entry, "aggregated_rating");
      if (auto it = entry.find("genres"); it != entry.end() && it->is_array()) {
        for (const auto &genre : *it) {
          auto name = genre.is_object() ? json_string(genre, "name") : std::string {};
          if (!name.empty()) {
            game.genres.push_back(std::move(name));
          }
        }
      }
      if (auto it = entry.find("cover"); it != entry.end() && it->is_object()) {
        game.cover_image_id = json_string(*it, "image_id");
      }
      if (auto it = entry.find("artworks"); it != entry.end() && it->is_array()) {
        for (const auto &artwork : *it) {
          auto image = artwork.is_object() ? json_string(artwork, "image_id") : std::string {};
          if (!image.empty()) {
            game.artwork_image_id = std::move(image);
            break;
          }
        }
      }
      if (auto it = entry.find("involved_companies"); it != entry.end() && it->is_array()) {
        for (const auto &involved : *it) {
          if (!involved.is_object()) {
            continue;
          }
          std::string company;
          if (auto company_it = involved.find("company");
              company_it != involved.end() && company_it->is_object()) {
            company = json_string(*company_it, "name");
          }
          if (company.empty()) {
            continue;
          }
          const auto flag = [&involved](const char *key) {
            auto it = involved.find(key);
            return it != involved.end() && it->is_boolean() && it->get<bool>();
          };
          // A studio that both made and shipped a game appears in both lists, which is what
          // the Playnite-fed fields did too, so clients need no new handling.
          if (flag("developer")) {
            game.developers.push_back(company);
          }
          if (flag("publisher")) {
            game.publishers.push_back(company);
          }
        }
      }
      out.push_back(std::move(game));
    }
    return out;
  }

  std::string release_date_from_timestamp(std::int64_t seconds) {
    if (seconds == 0) {
      return {};
    }
    // Civil date from a day count, without touching the C library: gmtime is not portable
    // enough to be worth a platform shim in a file that is otherwise pure.
    auto days = seconds / 86400;
    if (seconds < 0 && seconds % 86400 != 0) {
      --days;
    }
    days += 719468;  // Shift the epoch to 0000-03-01, which makes leap years regular.
    const auto era = (days >= 0 ? days : days - 146096) / 146097;
    const auto day_of_era = static_cast<unsigned long long>(days - era * 146097);
    const auto year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const auto day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const auto shifted_month = (5 * day_of_year + 2) / 153;
    const auto day = static_cast<unsigned>(day_of_year - (153 * shifted_month + 2) / 5 + 1);
    const auto month = static_cast<unsigned>(shifted_month < 10 ? shifted_month + 3 : shifted_month - 9);
    const auto year = static_cast<long long>(year_of_era) + era * 400 + (month <= 2 ? 1 : 0);

    std::ostringstream out;
    out.fill('0');
    out.width(4);
    out << year << '-';
    out.width(2);
    out << month << '-';
    out.width(2);
    out << day;
    return out.str();
  }

  metadata::game_metadata_t to_metadata(const game_t &game) {
    metadata::game_metadata_t meta;
    meta.description = game.summary;
    meta.genres = game.genres;
    meta.developers = game.developers;
    meta.publishers = game.publishers;
    meta.release_date = release_date_from_timestamp(game.first_release_date);
    meta.community_score = game.community_score;
    meta.critic_score = game.critic_score;
    meta.igdb_id = game.igdb_id;
    meta.source = "igdb";
    meta.present = metadata::has_any_value(meta);
    return meta;
  }

  std::string image_url(const std::string &image_id, std::string_view size) {
    if (image_id.empty() || size.empty()) {
      return {};
    }
    std::string url = "https://images.igdb.com/igdb/image/upload/";
    url.append(size);
    url.push_back('/');
    url.append(image_id);
    url.append(".jpg");
    return url;
  }

  std::string normalize_title(const std::string &name) {
    std::string folded;
    folded.reserve(name.size());
    for (const unsigned char c : name) {
      if (std::isalnum(c)) {
        folded.push_back(static_cast<char>(std::tolower(c)));
      } else if (c >= 0x80) {
        // Non-ASCII bytes are dropped rather than transliterated: the alternative is a
        // Unicode table for the handful of accented titles it would rescue.
        continue;
      } else {
        folded.push_back(' ');
      }
    }
    folded = collapse_whitespace(folded);
    for (const char *suffix : k_edition_suffixes) {
      const std::string_view tail {suffix};
      if (folded.size() > tail.size() + 1 &&
          folded.compare(folded.size() - tail.size(), tail.size(), tail) == 0 &&
          folded[folded.size() - tail.size() - 1] == ' ') {
        folded.erase(folded.size() - tail.size() - 1);
        break;
      }
    }
    return folded;
  }

  std::optional<game_t> best_name_match(const std::vector<game_t> &hits, const std::string &name) {
    const auto wanted = normalize_title(name);
    if (wanted.empty()) {
      return std::nullopt;
    }
    for (const auto &hit : hits) {
      if (normalize_title(hit.name) == wanted) {
        return hit;
      }
    }
    return std::nullopt;
  }

  std::chrono::milliseconds pacing_delay(const std::vector<std::chrono::steady_clock::time_point> &recent,
                                         std::chrono::steady_clock::time_point now) {
    if (static_cast<int>(recent.size()) < k_requests_per_second) {
      return std::chrono::milliseconds {0};
    }
    // The request that has to age out is the one four slots back: once a full second has
    // passed since it was sent, sending now keeps us at four per second.
    const auto oldest = recent[recent.size() - k_requests_per_second];
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - oldest);
    const auto remaining = std::chrono::milliseconds {1000} - elapsed;
    return remaining > std::chrono::milliseconds {0} ? remaining : std::chrono::milliseconds {0};
  }

  bool cache_is_fresh(std::chrono::system_clock::time_point written,
                      std::chrono::system_clock::time_point now,
                      std::chrono::hours ttl) {
    if (written > now) {
      // A clock that moved backwards, or a file copied from another machine. Treating the
      // entry as stale costs one request; trusting it could pin bad data forever.
      return false;
    }
    return (now - written) < ttl;
  }

}  // namespace igdb::policy
