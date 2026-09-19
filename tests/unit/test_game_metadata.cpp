#include "src/game_metadata.h"

#include <gtest/gtest.h>

namespace {
  nlohmann::json playnite_era_app() {
    return nlohmann::json {
      {"name", "Hades II"},
      {"uuid", "aaaa"},
      {"playnite-id", "1234"},
      {"playnite-description", "A rogue-like."},
      {"playnite-genres", nlohmann::json::array({"Action", "Roguelike"})},
      {"playnite-developers", nlohmann::json::array({"Supergiant Games"})},
      {"playnite-release-date", "2024-05-06"},
      {"playnite-community-score", 92},
      {"playnite-playtime-minutes", 1462},
      {"playnite-background", "C:/covers/bg.png"},
    };
  }
}  // namespace

TEST(GameMetadata, ReadsTheLegacyPlayniteKeys) {
  const auto meta = metadata::read_from_app(playnite_era_app());
  EXPECT_TRUE(meta.present);
  EXPECT_EQ(meta.description, "A rogue-like.");
  EXPECT_EQ(meta.genres.size(), 2u);
  EXPECT_EQ(meta.developers.front(), "Supergiant Games");
  EXPECT_EQ(meta.release_date, "2024-05-06");
  EXPECT_EQ(meta.community_score, 92);
  EXPECT_EQ(meta.critic_score, -1);
  EXPECT_EQ(meta.playtime_minutes, 1462u);
  EXPECT_EQ(meta.background_image_path, "C:/covers/bg.png");
}

TEST(GameMetadata, AnAppWithOnlyLegacyKeysIsAttributedToPlaynite) {
  // Without this, a pre-upgrade app would look unclaimed and the resolver would overwrite
  // metadata Playnite is still maintaining.
  EXPECT_EQ(metadata::read_from_app(playnite_era_app()).source, "playnite");
}

TEST(GameMetadata, WritingReplacesTheLegacySpelling) {
  auto app = playnite_era_app();
  auto meta = metadata::read_from_app(app);
  meta.source = "igdb";
  meta.igdb_id = "1020";
  metadata::write_to_app(app, meta);

  EXPECT_FALSE(app.contains("playnite-description"));
  EXPECT_FALSE(app.contains("playnite-background"));
  EXPECT_EQ(app["meta-description"], "A rogue-like.");
  EXPECT_EQ(app["meta-source"], "igdb");
  EXPECT_EQ(app["meta-igdb-id"], "1020");
  // playnite-id is identity, not metadata, and must survive.
  EXPECT_EQ(app["playnite-id"], "1234");
}

TEST(GameMetadata, RoundTripsThroughTheCanonicalKeys) {
  nlohmann::json app = nlohmann::json::object();
  metadata::game_metadata_t meta;
  meta.description = "Desc";
  meta.genres = {"RPG"};
  meta.publishers = {"Pub"};
  meta.critic_score = 77;
  meta.last_played = "2026-09-17T21:04:00Z";
  meta.playtime_minutes = 10;
  meta.source = "manual";
  meta.locked = true;
  metadata::write_to_app(app, meta);

  const auto read = metadata::read_from_app(app);
  EXPECT_EQ(read.description, "Desc");
  EXPECT_EQ(read.genres, meta.genres);
  EXPECT_EQ(read.publishers, meta.publishers);
  EXPECT_EQ(read.critic_score, 77);
  EXPECT_EQ(read.community_score, -1);
  EXPECT_EQ(read.last_played, meta.last_played);
  EXPECT_EQ(read.playtime_minutes, 10u);
  EXPECT_EQ(read.source, "manual");
  EXPECT_TRUE(read.locked);
}

TEST(GameMetadata, AbsentValuesLeaveNoKeysBehind) {
  nlohmann::json app {{"meta-description", "old"}, {"meta-genres", nlohmann::json::array({"x"})},
                      {"meta-locked", true}, {"meta-playtime-minutes", 5}};
  metadata::write_to_app(app, metadata::game_metadata_t {});
  EXPECT_FALSE(app.contains("meta-description"));
  EXPECT_FALSE(app.contains("meta-genres"));
  EXPECT_FALSE(app.contains("meta-locked"));
  EXPECT_FALSE(app.contains("meta-playtime-minutes"));
}

TEST(GameMetadata, StoreIdsComeFromEveryProvider) {
  nlohmann::json app {
    {"steam-id", "570"},
    {"lutris-service", "gog"},
    {"lutris-service-id", "1207658924"},
    {"playnite-source", "Epic Games Store"},
    {"playnite-source-id", "abc123"},
  };
  const auto ids = metadata::store_ids_of(app);
  ASSERT_EQ(ids.size(), 3u);
  EXPECT_EQ(ids[0].store, "steam");
  EXPECT_EQ(ids[0].id, "570");
  EXPECT_EQ(ids[1].store, "gog");
  EXPECT_EQ(ids[2].store, "epic");
  EXPECT_EQ(ids[2].id, "abc123");
}

TEST(GameMetadata, LocalOnlyIdsAreNotStoreIdentity) {
  // A Lutris id and a Playnite GUID mean nothing to an external database, so offering them
  // would only produce wrong matches.
  nlohmann::json app {{"lutris-id", "42"}, {"playnite-id", "6f0b"}};
  EXPECT_TRUE(metadata::store_ids_of(app).empty());
}

TEST(GameMetadata, NumericStoreIdsReadTheSameAsStrings) {
  nlohmann::json app {{"steam-id", 570}};
  const auto ids = metadata::store_ids_of(app);
  ASSERT_EQ(ids.size(), 1u);
  EXPECT_EQ(ids[0].id, "570");
}

TEST(GameMetadata, StoreNamesNormalizeAcrossProviders) {
  EXPECT_EQ(metadata::normalize_store_name("GOG"), "gog");
  EXPECT_EQ(metadata::normalize_store_name("Epic Games Store"), "epic");
  EXPECT_EQ(metadata::normalize_store_name("egs"), "epic");
  EXPECT_EQ(metadata::normalize_store_name("Ubisoft Connect"), "ubisoft");
  EXPECT_EQ(metadata::normalize_store_name("Humble Bundle"), "");
}
