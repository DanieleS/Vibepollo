/**
 * @file tests/unit/test_game_metadata_policy.cpp
 * @brief Tests for the provider-neutral app metadata keys and the IGDB mirror matching rules.
 */

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <src/game_metadata_policy.h>

using namespace game_metadata;

namespace {
  nlohmann::json gamedb_record() {
    return nlohmann::json::parse(R"({
      "id": 1020,
      "name": "Grand Theft Auto V",
      "summary": "Los Santos.",
      "storyline": "Longer text.",
      "aggregated_rating": 88.137,
      "rating": 89.647,
      "cover": {"id": 120937, "url": "//images.igdb.com/igdb/image/upload/t_thumb/co2lbd.jpg"},
      "artworks": [{"id": 1, "url": "//images.igdb.com/igdb/image/upload/t_thumb/ar3m56.jpg"}],
      "screenshots": [{"id": 2, "url": "//images.igdb.com/igdb/image/upload/t_thumb/scxyz.jpg"}],
      "genres": [{"id": 5, "name": "Shooter"}, {"id": 31, "name": "Adventure"}, {"id": 5, "name": "Shooter"}],
      "involved_companies": [
        {"id": 1, "company": {"id": 139, "name": "Take-Two Interactive"}, "developer": false},
        {"id": 2, "company": {"id": 365, "name": "Rockstar North"}, "developer": true},
        {"id": 3, "company": {"id": 29, "name": "Rockstar Games"}, "developer": false}
      ],
      "release_dates": [
        {"id": 10, "date": 1428883200, "y": 2015},
        {"id": 11, "date": 1416268800, "y": 2014},
        {"id": 12, "y": 2013}
      ]
    })");
  }
}  // namespace

TEST(GameMetadataKeys, ReadPrefersCurrentKeysAndFallsBackToLegacyPlayniteKeys) {
  nlohmann::json legacy = {
    {"name", "Old"},
    {"playnite-description", "From Playnite"},
    {"playnite-genres", {"Indie"}},
    {"playnite-critic-score", 77},
    {"playnite-playtime-minutes", 90},
    {"playnite-background", "C:/covers/bg.png"},
  };
  const auto from_legacy = read_app_metadata(legacy);
  EXPECT_TRUE(from_legacy.present);
  EXPECT_EQ(from_legacy.description, "From Playnite");
  ASSERT_EQ(from_legacy.genres.size(), 1u);
  EXPECT_EQ(from_legacy.critic_score, 77);
  EXPECT_EQ(from_legacy.playtime_minutes, 90u);
  EXPECT_EQ(from_legacy.background_image_path, "C:/covers/bg.png");
  // Legacy keys could only have been written by the Playnite sync.
  EXPECT_EQ(from_legacy.source, source_playnite);

  nlohmann::json current = legacy;
  current[keys::description] = "From IGDB";
  current[keys::source] = source_igdb;
  current[keys::igdb_id] = "1020";
  const auto from_current = read_app_metadata(current);
  EXPECT_EQ(from_current.description, "From IGDB");
  EXPECT_EQ(from_current.source, source_igdb);
  EXPECT_EQ(from_current.igdb_id, "1020");

  EXPECT_FALSE(read_app_metadata(nlohmann::json {{"name", "Bare"}}).present);
  EXPECT_FALSE(read_app_metadata(nlohmann::json::array()).present);
}

TEST(GameMetadataKeys, WriteDescriptiveSetsErasesAndMigratesLegacyKeys) {
  nlohmann::json app = {
    {"name", "Game"},
    {"playnite-description", "old"},
    {"playnite-genres", {"Old"}},
    {"playnite-background", "old.png"},
    {"playnite-last-played", "2024-01-01T00:00:00Z"},
    {keys::critic_score, 50},
  };
  descriptive_t fields;
  fields.description = "new";
  fields.developers = {"Dev"};
  fields.release_date = "2020-03-04";
  fields.community_score = 250;  // out of range values are clamped
  write_descriptive(app, fields, source_igdb, "42");

  EXPECT_EQ(app[keys::description], "new");
  EXPECT_EQ(app[keys::developers], nlohmann::json({"Dev"}));
  EXPECT_EQ(app[keys::release_date], "2020-03-04");
  EXPECT_EQ(app[keys::community_score], 100);
  EXPECT_EQ(app[keys::source], source_igdb);
  EXPECT_EQ(app[keys::igdb_id], "42");
  // Empty fields erase both spellings of their key.
  EXPECT_FALSE(app.contains(keys::genres));
  EXPECT_FALSE(app.contains("playnite-genres"));
  EXPECT_FALSE(app.contains(keys::critic_score));
  EXPECT_FALSE(app.contains(keys::background));
  EXPECT_FALSE(app.contains("playnite-background"));
  EXPECT_FALSE(app.contains("playnite-description"));
  // Activity data is not descriptive and is left alone.
  EXPECT_EQ(app["playnite-last-played"], "2024-01-01T00:00:00Z");
  EXPECT_EQ(app["name"], "Game");

  // A second write with an empty source and id erases them again.
  write_descriptive(app, descriptive_t {}, "", "");
  EXPECT_FALSE(app.contains(keys::source));
  EXPECT_FALSE(app.contains(keys::igdb_id));
  EXPECT_FALSE(app.contains(keys::description));
}

TEST(GameMetadataKeys, WriteActivityErasesZeroPlaytimeAndLegacyKeys) {
  nlohmann::json app = {{"playnite-last-played", "x"}, {"playnite-playtime-minutes", 5}};
  write_activity(app, "2025-01-01T10:00:00Z", 120);
  EXPECT_EQ(app[keys::last_played], "2025-01-01T10:00:00Z");
  EXPECT_EQ(app[keys::playtime_minutes], 120);
  EXPECT_FALSE(app.contains("playnite-last-played"));
  EXPECT_FALSE(app.contains("playnite-playtime-minutes"));

  write_activity(app, "", 0);
  EXPECT_FALSE(app.contains(keys::last_played));
  EXPECT_FALSE(app.contains(keys::playtime_minutes));
}

TEST(GameMetadataKeys, HasDescriptiveMetadataIgnoresActivityOnly) {
  EXPECT_FALSE(has_descriptive_metadata(nlohmann::json {{keys::last_played, "x"}, {keys::playtime_minutes, 3}}));
  EXPECT_TRUE(has_descriptive_metadata(nlohmann::json {{"playnite-genres", {"Indie"}}}));
  EXPECT_TRUE(has_descriptive_metadata(nlohmann::json {{keys::background, "bg.png"}}));
}

TEST(GameDbMatching, BucketKeyMirrorsWebUiCoverSearch) {
  EXPECT_EQ(bucket_key("Hades"), "ha");
  EXPECT_EQ(bucket_key("DOOM Eternal"), "do");
  EXPECT_EQ(bucket_key("A Hat in Time"), "a");  // the space is dropped, only two characters are looked at
  EXPECT_EQ(bucket_key("7 Days to Die"), "7");
  EXPECT_EQ(bucket_key("!!"), "@");
  EXPECT_EQ(bucket_key(""), "@");
  EXPECT_EQ(bucket_url("ha"), "https://raw.githubusercontent.com/LizardByte/GameDB/gh-pages/buckets/ha.json");
  EXPECT_EQ(game_url("1020"), "https://raw.githubusercontent.com/LizardByte/GameDB/gh-pages/games/1020.json");
  EXPECT_EQ(image_url(igdb_background_size, "ar3m56"), "https://images.igdb.com/igdb/image/upload/t_1080p/ar3m56.png");
}

TEST(GameDbMatching, NormalizeTitleDropsPunctuationCaseAndTrademarks) {
  EXPECT_EQ(normalize_title("DOOM: Eternal"), "doometernal");
  EXPECT_EQ(normalize_title("Doom Eternal\xE2\x84\xA2"), "doometernal");
  EXPECT_EQ(normalize_title("  Half-Life 2  "), "halflife2");
  EXPECT_EQ(strip_trailing_parenthetical("Prey (2017)"), "Prey");
  EXPECT_EQ(strip_trailing_parenthetical("(Not) stripped"), "(Not) stripped");
  EXPECT_EQ(strip_trailing_parenthetical("Plain"), "Plain");
}

TEST(GameDbMatching, ParseBucketSkipsEntriesWithoutNames) {
  const auto bucket = nlohmann::json::parse(R"({"1": {"name": "Hades"}, "2": {}, "3": "bad", "4": {"name": ""}})");
  const auto entries = parse_bucket(bucket);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].id, "1");
  EXPECT_EQ(entries[0].name, "Hades");
  EXPECT_TRUE(parse_bucket(nlohmann::json::array()).empty());
}

TEST(GameDbMatching, SelectMatchPrefersExactThenStrippedThenUniquePrefix) {
  const std::vector<bucket_entry_t> entries {
    {"113112", "Hades"},
    {"200000", "Hades II"},
    {"300000", "Hades: Special Edition"},
    {"7346", "Prey"},
    {"9999", "Halo Infinite"},
  };

  auto exact = select_match("HADES", entries);
  EXPECT_EQ(exact.kind, match_kind_e::exact);
  EXPECT_EQ(exact.id, "113112");
  EXPECT_EQ(exact.exact_candidates, 1u);

  auto stripped = select_match("Prey (2017)", entries);
  EXPECT_EQ(stripped.kind, match_kind_e::exact);
  EXPECT_EQ(stripped.id, "7346");

  auto prefix = select_match("Halo", entries);
  EXPECT_EQ(prefix.kind, match_kind_e::unique_prefix);
  EXPECT_EQ(prefix.id, "9999");

  // "Hades I" shares letters with "Hades II" but not words, so nothing is guessed.
  EXPECT_EQ(select_match("Hades I", entries).kind, match_kind_e::none);
  // A word prefix never matches inside a word.
  EXPECT_EQ(select_match("Hal", entries).kind, match_kind_e::none);
  // "Ha" starts two or more names, so nothing is guessed.
  EXPECT_EQ(select_match("Ha", entries).kind, match_kind_e::none);
  EXPECT_EQ(select_match("", entries).kind, match_kind_e::none);
  EXPECT_EQ(select_match("!!!", entries).kind, match_kind_e::none);
}

TEST(GameDbMatching, SelectMatchTieBreaksVerbatimNameThenLowestId) {
  const std::vector<bucket_entry_t> entries {
    {"2", "DOOM"},
    {"1", "Doom"},
    {"3", "D.O.O.M"},
  };
  auto verbatim = select_match("DOOM", entries);
  EXPECT_EQ(verbatim.kind, match_kind_e::exact);
  EXPECT_EQ(verbatim.id, "2");
  EXPECT_EQ(verbatim.exact_candidates, 3u);

  auto lowest = select_match("doom", entries);
  EXPECT_EQ(lowest.id, "1");

  // Ids compare numerically, not lexicographically.
  const std::vector<bucket_entry_t> numeric {{"10", "Same"}, {"9", "Same"}};
  EXPECT_EQ(select_match("same", numeric).id, "9");
}

TEST(GameDbParsing, ParseGameMapsCompaniesDatesRatingsAndImages) {
  const auto game = parse_game(gamedb_record());
  ASSERT_TRUE(game.has_value());
  EXPECT_EQ(game->id, "1020");
  EXPECT_EQ(game->name, "Grand Theft Auto V");
  EXPECT_EQ(game->summary, "Los Santos.");
  EXPECT_EQ(game->genres, (std::vector<std::string> {"Shooter", "Adventure"}));
  EXPECT_EQ(game->developers, (std::vector<std::string> {"Rockstar North"}));
  EXPECT_EQ(game->publishers, (std::vector<std::string> {"Take-Two Interactive", "Rockstar Games"}));
  // Earliest dated release wins over a bare year.
  EXPECT_EQ(game->release_date, "2014-11-18");
  EXPECT_EQ(game->critic_score, 88);
  EXPECT_EQ(game->community_score, 90);
  EXPECT_EQ(game->cover_slug, "co2lbd");
  EXPECT_EQ(game->artwork_slug, "ar3m56");
}

TEST(GameDbParsing, ParseGameHandlesSparseRecords) {
  auto record = nlohmann::json::parse(R"({"id": "55", "name": " Minimal ", "release_dates": [{"y": 1999}], "screenshots": [{"url": "//images.igdb.com/igdb/image/upload/t_thumb/sc1.jpg"}]})");
  const auto game = parse_game(record);
  ASSERT_TRUE(game.has_value());
  EXPECT_EQ(game->id, "55");
  EXPECT_EQ(game->name, "Minimal");
  EXPECT_EQ(game->release_date, "1999");
  EXPECT_EQ(game->critic_score, -1);
  EXPECT_EQ(game->community_score, -1);
  EXPECT_TRUE(game->cover_slug.empty());
  EXPECT_EQ(game->artwork_slug, "sc1");

  EXPECT_FALSE(parse_game(nlohmann::json::parse(R"({"name": "No id"})")).has_value());
  EXPECT_FALSE(parse_game(nlohmann::json::parse(R"({"id": 1})")).has_value());
  EXPECT_FALSE(parse_game(nlohmann::json::array()).has_value());
}

TEST(GameDbParsing, ImageSlugRejectsUnsafeValues) {
  EXPECT_EQ(image_slug("//images.igdb.com/igdb/image/upload/t_thumb/co2lbd.jpg"), "co2lbd");
  EXPECT_EQ(image_slug("https://images.igdb.com/igdb/image/upload/t_thumb/ar3m56.png?x=1"), "ar3m56");
  EXPECT_EQ(image_slug("//host/dir/"), "");
  EXPECT_EQ(image_slug("//host/a b.jpg"), "");
  EXPECT_EQ(format_release_date(0), "1970-01-01");
  EXPECT_EQ(format_release_date(1416268800), "2014-11-18");
}

TEST(GameDbParsing, ToDescriptiveCarriesEveryField) {
  const auto game = parse_game(gamedb_record());
  ASSERT_TRUE(game.has_value());
  const auto fields = to_descriptive(*game, "/covers/igdb_bg_1020.png");
  EXPECT_EQ(fields.description, "Los Santos.");
  EXPECT_EQ(fields.genres.size(), 2u);
  EXPECT_EQ(fields.developers.size(), 1u);
  EXPECT_EQ(fields.publishers.size(), 2u);
  EXPECT_EQ(fields.release_date, "2014-11-18");
  EXPECT_EQ(fields.community_score, 90);
  EXPECT_EQ(fields.critic_score, 88);
  EXPECT_EQ(fields.background_path, "/covers/igdb_bg_1020.png");

  nlohmann::json app = {{"name", "GTA V"}};
  write_descriptive(app, fields, source_igdb, game->id);
  const auto meta = read_app_metadata(app);
  EXPECT_TRUE(meta.present);
  EXPECT_EQ(meta.source, source_igdb);
  EXPECT_EQ(meta.igdb_id, "1020");
  EXPECT_EQ(meta.background_image_path, "/covers/igdb_bg_1020.png");
  EXPECT_EQ(meta.publishers.size(), 2u);
}
