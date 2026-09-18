#include "src/igdb_policy.h"

#include <gtest/gtest.h>

TEST(IgdbPolicy, ReadsTheSourceNumberingFromIgdbRatherThanAssumingIt) {
  const std::string body = R"([{"id": 1, "name": "Steam"}, {"id": 5, "name": "GOG"},
                               {"id": 26, "name": "Epic Games Store"},
                               {"id": 82, "name": "Some Console Store"}])";
  const auto sources = igdb::policy::parse_external_sources(body);
  EXPECT_EQ(sources.at("steam"), 1);
  EXPECT_EQ(sources.at("gog"), 5);
  EXPECT_EQ(sources.at("epic"), 26);
  // A source that is not a PC storefront we can produce an id for is dropped rather than
  // guessed into a slug.
  EXPECT_EQ(sources.size(), 3u);
}

TEST(IgdbPolicy, TheLowestIdWinsWhenTwoSourcesNormalizeTheSame) {
  const auto sources = igdb::policy::parse_external_sources(
    R"([{"id": 1, "name": "Steam"}, {"id": 54, "name": "Steam China"}])");
  EXPECT_EQ(sources.at("steam"), 1);
}

TEST(IgdbPolicy, ExternalLookupOnlyAsksAboutStoresIgdbIndexes) {
  const auto sources = igdb::policy::parse_external_sources(
    R"([{"id": 1, "name": "Steam"}, {"id": 5, "name": "GOG"}])");
  const auto query = igdb::policy::external_lookup_query(
    {
      {"steam", "570"},
      {"ubisoft", "anno-1800"},
      {"gog", "1207658924"},
    },
    sources);
  EXPECT_NE(query.find("uid = \"570\" & external_game_source = 1"), std::string::npos);
  EXPECT_NE(query.find("uid = \"1207658924\" & external_game_source = 5"), std::string::npos);
  // Ubisoft Connect is in no source list, so asking about it would only return nothing.
  EXPECT_EQ(query.find("anno-1800"), std::string::npos);
}

TEST(IgdbPolicy, TheLegacyFieldIsStillReachableForAnOlderIgdb) {
  const auto query = igdb::policy::external_lookup_query(
    {{"steam", "570"}}, igdb::policy::legacy_source_map(), true);
  EXPECT_NE(query.find("uid = \"570\" & category = 1"), std::string::npos);
  EXPECT_EQ(query.find("external_game_source"), std::string::npos);
}

TEST(IgdbPolicy, NoIndexedStoreMeansNoQueryAtAll) {
  const auto sources = igdb::policy::legacy_source_map();
  EXPECT_TRUE(igdb::policy::external_lookup_query({{"ubisoft", "x"}, {"battlenet", "y"}}, sources).empty());
  EXPECT_TRUE(igdb::policy::external_lookup_query({}, sources).empty());
  // No sources at all: everything falls through to a title search rather than a broken query.
  EXPECT_TRUE(igdb::policy::external_lookup_query({{"steam", "570"}}, {}).empty());
}

TEST(IgdbPolicy, QuotesInATitleCannotEndTheQueryString) {
  const auto query = igdb::policy::search_query("Sid Meier\"s; drop", 5);
  EXPECT_NE(query.find("Sid Meier\\\"s; drop"), std::string::npos);
}

TEST(IgdbPolicy, GamesByIdRejectsAnythingThatIsNotAnId) {
  EXPECT_TRUE(igdb::policy::games_by_id_query({"12a", ""}).empty());
  const auto query = igdb::policy::games_by_id_query({"1020", "bad", "77"});
  EXPECT_NE(query.find("where id = (1020,77)"), std::string::npos);
}

TEST(IgdbPolicy, ParsesAGameRecord) {
  const std::string body = R"([{
    "id": 1020,
    "name": "Grand Theft Auto V",
    "summary": "A crime story.",
    "first_release_date": 1379635200,
    "rating": 87.4,
    "aggregated_rating": 92.6,
    "genres": [{"id": 5, "name": "Shooter"}, {"id": 31, "name": "Adventure"}],
    "cover": {"id": 1, "image_id": "co2lbd"},
    "artworks": [{"id": 9, "image_id": "ar1x2"}],
    "involved_companies": [
      {"id": 1, "company": {"id": 2, "name": "Rockstar North"}, "developer": true, "publisher": false},
      {"id": 2, "company": {"id": 3, "name": "Rockstar Games"}, "developer": false, "publisher": true}
    ]
  }])";
  const auto games = igdb::policy::parse_games(body);
  ASSERT_EQ(games.size(), 1u);
  const auto &game = games.front();
  EXPECT_EQ(game.igdb_id, "1020");
  EXPECT_EQ(game.name, "Grand Theft Auto V");
  EXPECT_EQ(game.genres.size(), 2u);
  EXPECT_EQ(game.developers, std::vector<std::string> {"Rockstar North"});
  EXPECT_EQ(game.publishers, std::vector<std::string> {"Rockstar Games"});
  EXPECT_EQ(game.community_score, 87);
  EXPECT_EQ(game.critic_score, 93);
  EXPECT_EQ(game.cover_image_id, "co2lbd");
  EXPECT_EQ(game.artwork_image_id, "ar1x2");
}

TEST(IgdbPolicy, AStudioThatBothMadeAndShippedAGameAppearsInBothLists) {
  const std::string body = R"([{"id": 7, "name": "Hades",
    "involved_companies": [{"company": {"name": "Supergiant Games"}, "developer": true, "publisher": true}]}])";
  const auto games = igdb::policy::parse_games(body);
  ASSERT_EQ(games.size(), 1u);
  EXPECT_EQ(games[0].developers, std::vector<std::string> {"Supergiant Games"});
  EXPECT_EQ(games[0].publishers, std::vector<std::string> {"Supergiant Games"});
}

TEST(IgdbPolicy, MalformedResponsesYieldNothingRatherThanThrowing) {
  EXPECT_TRUE(igdb::policy::parse_games("not json").empty());
  EXPECT_TRUE(igdb::policy::parse_games("{}").empty());
  EXPECT_TRUE(igdb::policy::parse_games("[3, null, {\"name\": \"no id\"}]").empty());
  EXPECT_TRUE(igdb::policy::parse_external_matches("nope").empty());
}

TEST(IgdbPolicy, ParsesExternalMatches) {
  const std::string body = R"([{"id": 1, "game": 1020, "uid": "271590", "category": 1},
                               {"id": 2, "game": {"id": 7}, "uid": "1207658924", "category": 5}])";
  const auto matches = igdb::policy::parse_external_matches(body);
  ASSERT_EQ(matches.size(), 2u);
  EXPECT_EQ(matches[0].igdb_id, "1020");
  EXPECT_EQ(matches[0].store_id, "271590");
  EXPECT_EQ(matches[1].igdb_id, "7");
  EXPECT_EQ(matches[1].store_id, "1207658924");
}

TEST(IgdbPolicy, ScoresOutsideZeroToOneHundredAreNotReported) {
  const auto games = igdb::policy::parse_games(R"([{"id": 1, "rating": -4, "aggregated_rating": 130.2}])");
  ASSERT_EQ(games.size(), 1u);
  EXPECT_EQ(games[0].community_score, -1);
  EXPECT_EQ(games[0].critic_score, 100);
}

TEST(IgdbPolicy, RendersReleaseDates) {
  EXPECT_EQ(igdb::policy::release_date_from_timestamp(1379635200), "2013-09-20");
  EXPECT_EQ(igdb::policy::release_date_from_timestamp(0), "");
  EXPECT_EQ(igdb::policy::release_date_from_timestamp(946684800), "2000-01-01");
  // Before the epoch: a handful of IGDB records predate 1970.
  EXPECT_EQ(igdb::policy::release_date_from_timestamp(-86400), "1969-12-31");
}

TEST(IgdbPolicy, ToMetadataLeavesPlaytimeToWhoeverLaunchesTheGame) {
  igdb::policy::game_t game;
  game.igdb_id = "1020";
  game.name = "GTA V";
  game.summary = "Crime.";
  game.first_release_date = 1379635200;
  const auto meta = igdb::policy::to_metadata(game);
  EXPECT_EQ(meta.description, "Crime.");
  EXPECT_EQ(meta.release_date, "2013-09-20");
  EXPECT_EQ(meta.source, "igdb");
  EXPECT_EQ(meta.igdb_id, "1020");
  EXPECT_TRUE(meta.last_played.empty());
  EXPECT_EQ(meta.playtime_minutes, 0u);
}

TEST(IgdbPolicy, NormalizesTitlesForComparison) {
  EXPECT_EQ(igdb::policy::normalize_title("The Witcher 3: Wild Hunt"), "the witcher 3 wild hunt");
  EXPECT_EQ(igdb::policy::normalize_title("DOOM\xC2\xAE"), "doom");
  EXPECT_EQ(igdb::policy::normalize_title("Skyrim - Special Edition"), "skyrim");
  EXPECT_EQ(igdb::policy::normalize_title("Dark Souls: Remastered"), "dark souls");
}

TEST(IgdbPolicy, ANameMatchHasToBeExactAfterNormalizing) {
  std::vector<igdb::policy::game_t> hits;
  igdb::policy::game_t near_miss;
  near_miss.igdb_id = "1";
  near_miss.name = "Half-Life 2: Episode One";
  igdb::policy::game_t exact;
  exact.igdb_id = "2";
  exact.name = "Half-Life 2";
  hits = {near_miss, exact};

  const auto match = igdb::policy::best_name_match(hits, "Half-Life 2");
  ASSERT_TRUE(match.has_value());
  EXPECT_EQ(match->igdb_id, "2");
  // A fuzzy hit alone is not enough: writing the wrong summary onto a library is worse than
  // leaving the game undescribed.
  EXPECT_FALSE(igdb::policy::best_name_match({near_miss}, "Half-Life 2").has_value());
}

TEST(IgdbPolicy, PacingHoldsAtFourRequestsASecond) {
  using namespace std::chrono;
  const auto now = steady_clock::now();
  EXPECT_EQ(igdb::policy::pacing_delay({now - milliseconds {10}}, now).count(), 0);

  const std::vector<steady_clock::time_point> four {
    now - milliseconds {300}, now - milliseconds {200},
    now - milliseconds {100}, now - milliseconds {10}};
  EXPECT_EQ(igdb::policy::pacing_delay(four, now).count(), 700);

  const std::vector<steady_clock::time_point> old_four {
    now - milliseconds {2000}, now - milliseconds {1900},
    now - milliseconds {1800}, now - milliseconds {1700}};
  EXPECT_EQ(igdb::policy::pacing_delay(old_four, now).count(), 0);
}

TEST(IgdbPolicy, CacheFreshness) {
  using namespace std::chrono;
  const auto now = system_clock::now();
  EXPECT_TRUE(igdb::policy::cache_is_fresh(now - hours {24}, now, hours {720}));
  EXPECT_FALSE(igdb::policy::cache_is_fresh(now - hours {721}, now, hours {720}));
  // A file from the future means a clock that moved, not a fresher record.
  EXPECT_FALSE(igdb::policy::cache_is_fresh(now + hours {1}, now, hours {720}));
}

TEST(IgdbPolicy, BuildsImageUrls) {
  EXPECT_EQ(igdb::policy::image_url("co2lbd", "t_cover_big_2x"),
            "https://images.igdb.com/igdb/image/upload/t_cover_big_2x/co2lbd.jpg");
  EXPECT_TRUE(igdb::policy::image_url("", "t_1080p").empty());
}
