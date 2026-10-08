/**
 * @file tests/unit/test_gameactivity.cpp
 * @brief GameActivity's files: the sessions read from them, and the folder they are cached from.
 *
 * The fixtures follow what GameActivity 3.x writes through Playnite's serializer (Newtonsoft): the
 * GameActivities object (Id, Name, Items, the legacy ItemsDetails block, SessionPlaytime...) with
 * one Activity per session, DateSession written from a UTC DateTime with a trailing Z.
 */
#include "src/gameactivity.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <random>

namespace {
  namespace fs = std::filesystem;

  constexpr std::int64_t k_hour = 3600;
  constexpr std::int64_t k_day = 86400;

  std::int64_t utc(int y, unsigned m, unsigned d, int h, int mi = 0, int s = 0) {
    using namespace std::chrono;
    return sys_days {year {y} / month {m} / day {d}}.time_since_epoch().count() * k_day + h * k_hour + mi * 60 + s;
  }

  const std::string k_hades = "c9a5d2f0-0d1e-4f4e-9a54-6b4d3f1f0e11";
  const std::string k_celeste = "5b8c4e2a-7f1d-4c3b-8e9a-0a1b2c3d4e5f";

  // A current file: three sessions, one still under way (0 seconds), hardware samples on one.
  const std::string k_current_file = R"json({
  "Items": [
    {
      "SourceID": "cb91dfc9-b977-43bf-8e70-55f46e410fab",
      "PlatformIDs": ["e2d6d4e3-6c5b-4b6f-9c8d-2f3a4b5c6d7e"],
      "GameActionName": "Play",
      "IdConfiguration": 0,
      "DateSession": "2024-03-11T18:00:00.1234567Z",
      "ElapsedSeconds": 3600,
      "Details": [
        {"Datelog": "2024-03-11T18:05:00.5Z", "FPS": 60, "FPS1PercentLow": 48, "FPS0Point1PercentLow": 0, "CPU": 35, "CPUT": 61, "CPUP": 0, "GPU": 80, "GPUT": 70, "GPUP": 0, "RAM": 42}
      ]
    },
    {
      "SourceID": "cb91dfc9-b977-43bf-8e70-55f46e410fab",
      "PlatformIDs": [],
      "GameActionName": "Play",
      "IdConfiguration": -1,
      "DateSession": "2024-03-12T21:30:15.0000001Z",
      "ElapsedSeconds": 7215,
      "Details": []
    },
    {
      "SourceID": "00000000-0000-0000-0000-000000000000",
      "PlatformIDs": [],
      "GameActionName": "Play",
      "IdConfiguration": -1,
      "DateSession": "2024-03-13T19:00:00Z",
      "ElapsedSeconds": 0,
      "Details": []
    }
  ],
  "ItemsDetails": null,
  "SessionPlaytime": 10815,
  "Id": "C9A5D2F0-0D1E-4F4E-9A54-6B4D3F1F0E11",
  "Name": "Hades",
  "DateLastRefresh": "0001-01-01T00:00:00",
  "SourcesLink": null,
  "GameExist": true
})json";

  // A file from before per-session Details: ItemsDetails keyed by session start, a DateSession
  // without zone (still UTC), and a default Activity GameActivity never filled in.
  const std::string k_legacy_file = R"json({
  "Items": [
    {"SourceID": "00000000-0000-0000-0000-000000000000", "PlatformIDs": null, "GameActionName": null, "IdConfiguration": -1, "DateSession": "2021-07-04T22:10:00", "ElapsedSeconds": 5400},
    {"SourceID": "00000000-0000-0000-0000-000000000000", "PlatformIDs": null, "GameActionName": null, "IdConfiguration": -1, "DateSession": "2021-07-05T20:00:00.000+02:00", "ElapsedSeconds": 60},
    {"SourceID": "00000000-0000-0000-0000-000000000000", "PlatformIDs": null, "GameActionName": null, "IdConfiguration": -1, "DateSession": "0001-01-01T00:00:00", "ElapsedSeconds": 120},
    {"DateSession": "not a date", "ElapsedSeconds": 10},
    {"DateSession": "2021-07-06T20:00:00Z"},
    {"DateSession": "2021-07-06T20:00:00Z", "ElapsedSeconds": "10"},
    42
  ],
  "ItemsDetails": {
    "Items": {
      "2021-07-04T22:10:00": [
        {"Datelog": "2021-07-04T22:15:00", "FPS": 144, "CPU": 20, "GPU": 95, "RAM": 50}
      ]
    }
  },
  "Id": "00000000-0000-0000-0000-000000000000",
  "Name": "Celeste"
})json";

  class TempDir {
  public:
    TempDir() {
      std::random_device rd;
      path_ = fs::temp_directory_path() / ("vibepollo-ga-test-" + std::to_string(rd()) + std::to_string(rd()));
      fs::create_directories(path_);
    }

    ~TempDir() {
      std::error_code ec;
      fs::remove_all(path_, ec);
    }

    const fs::path &path() const {
      return path_;
    }

  private:
    fs::path path_;
  };

  void write(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
  }
}  // namespace

TEST(GameActivityFileParse, ReadsTheSessionsOfACurrentFile) {
  const auto sessions = gameactivity::parse_game(k_current_file, "ignored");
  ASSERT_TRUE(sessions);
  // The run still under way (0 seconds) is left to the running-game count.
  ASSERT_EQ(sessions->size(), 2u);
  EXPECT_EQ((*sessions)[0].game, k_hades);
  EXPECT_EQ((*sessions)[0].start, utc(2024, 3, 11, 18));
  EXPECT_EQ((*sessions)[0].seconds, 3600);
  EXPECT_EQ((*sessions)[1].start, utc(2024, 3, 12, 21, 30, 15));
  EXPECT_EQ((*sessions)[1].seconds, 7215);
}

TEST(GameActivityFileParse, ALegacyFileUsesItsNameAndReadsAZonelessDateAsUtc) {
  const auto sessions = gameactivity::parse_game(k_legacy_file, "{5B8C4E2A-7F1D-4C3B-8E9A-0A1B2C3D4E5F}");
  ASSERT_TRUE(sessions);
  ASSERT_EQ(sessions->size(), 2u);
  EXPECT_EQ((*sessions)[0].game, k_celeste);
  EXPECT_EQ((*sessions)[0].start, utc(2021, 7, 4, 22, 10));
  EXPECT_EQ((*sessions)[0].seconds, 5400);
  EXPECT_EQ((*sessions)[1].start, utc(2021, 7, 5, 18));
  EXPECT_EQ((*sessions)[1].seconds, 60);
}

TEST(GameActivityFileParse, EmptyNullCutShortAndForeignFiles) {
  auto sessions = gameactivity::parse_game("\xEF\xBB\xBF{\"Items\":[],\"Id\":\"" + k_hades + "\",\"Name\":\"Hades\"}", "x");
  ASSERT_TRUE(sessions);
  EXPECT_TRUE(sessions->empty());
  sessions = gameactivity::parse_game("{\"Items\":null,\"Id\":\"" + k_hades + "\"}", "x");
  ASSERT_TRUE(sessions);
  EXPECT_TRUE(sessions->empty());
  EXPECT_FALSE(gameactivity::parse_game(k_current_file.substr(0, 200), k_hades));
  EXPECT_FALSE(gameactivity::parse_game("[1,2,3]", k_hades));
  EXPECT_FALSE(gameactivity::parse_game("{\"Items\":[]}", ""));
}

TEST(GameActivityFileStore, MissingFolderMeansNoGameActivity) {
  TempDir dir;
  gameactivity::store_t store(dir.path() / std::string(gameactivity::k_plugin_id), std::chrono::seconds(0));
  auto snap = store.snapshot();
  EXPECT_FALSE(snap.found);
  EXPECT_TRUE(snap.sessions.empty());

  // The folder alone, or files that aren't a game's (the running-session backups live next to
  // it, the migration archive and markers inside it), are still nothing.
  const auto root = dir.path() / std::string(gameactivity::k_plugin_id);
  write(root / ("SaveSession_" + k_hades + ".json"), "{\"Id\":\"" + k_hades + "\",\"ElapsedSeconds\":600,\"DateSession\":\"2024-03-13T19:00:00Z\"}");
  write(root / "GameActivity" / ".legacy-json-model-migration.done", "done");
  write(root / "GameActivity" / "Configurations.json", "[]");
  snap = store.snapshot();
  EXPECT_FALSE(snap.found);
  EXPECT_TRUE(snap.sessions.empty());
}

TEST(GameActivityFileStore, ReadsEveryGameAndFollowsChanges) {
  TempDir dir;
  const auto folder = dir.path() / "GameActivity";
  write(folder / (k_hades + ".json"), k_current_file);
  write(folder / (k_celeste + ".json"), k_legacy_file);
  gameactivity::store_t store(dir.path(), std::chrono::seconds(0));

  auto snap = store.snapshot();
  EXPECT_TRUE(snap.found);
  ASSERT_EQ(snap.sessions.size(), 4u);
  // Oldest first, whatever the folder's order.
  EXPECT_EQ(snap.sessions[0].game, k_celeste);
  EXPECT_EQ(snap.sessions[3].game, k_hades);
  EXPECT_EQ(snap.sessions[3].start, utc(2024, 3, 12, 21, 30, 15));

  // GameActivity rewrites a file when a session ends; a different size is enough to notice.
  write(folder / (k_celeste + ".json"), "{\"Items\":[{\"DateSession\":\"2024-03-13T19:00:00Z\",\"ElapsedSeconds\":7}],\"Id\":\"" + k_celeste + "\"}");
  snap = store.snapshot();
  ASSERT_EQ(snap.sessions.size(), 3u);
  EXPECT_EQ(snap.sessions[2].game, k_celeste);
  EXPECT_EQ(snap.sessions[2].seconds, 7);

  // Caught mid-write: the last good read stands.
  write(folder / (k_celeste + ".json"), "{\"Items\":[{\"DateSession\":\"2024-03-1");
  snap = store.snapshot();
  ASSERT_EQ(snap.sessions.size(), 3u);

  // A file removed takes its sessions with it; a game file with no sessions still means
  // GameActivity is there.
  fs::remove(folder / (k_hades + ".json"));
  write(folder / (k_celeste + ".json"), "{\"Items\":[],\"Id\":\"" + k_celeste + "\"}");
  snap = store.snapshot();
  EXPECT_TRUE(snap.found);
  EXPECT_TRUE(snap.sessions.empty());
}

TEST(GameActivityFileStore, BetweenScansTheLastResultIsServed) {
  TempDir dir;
  gameactivity::store_t store(dir.path(), std::chrono::hours(1));
  EXPECT_FALSE(store.snapshot().found);
  write(dir.path() / "GameActivity" / (k_hades + ".json"), k_current_file);
  EXPECT_FALSE(store.snapshot().found);
}

TEST(GameActivityFileStore, TheFolderComesFromTheConnectorOrNextToSuccessStorys) {
  EXPECT_EQ(gameactivity::resolve_data_dir("/data/ExtensionsData/afbb", "/data/ExtensionsData/cebe"), fs::path("/data/ExtensionsData/afbb"));
  EXPECT_EQ(gameactivity::resolve_data_dir("", "/data/ExtensionsData/cebe6d32-8c46-4459-b993-5a5189d60788"), fs::path("/data/ExtensionsData") / std::string(gameactivity::k_plugin_id));
  EXPECT_EQ(gameactivity::resolve_data_dir("", "/data/ExtensionsData/cebe6d32-8c46-4459-b993-5a5189d60788/"), fs::path("/data/ExtensionsData") / std::string(gameactivity::k_plugin_id));
  EXPECT_TRUE(gameactivity::resolve_data_dir("", "").empty());
}

TEST(GameActivityFileStore, DataFoundLooksForOneGameFile) {
  TempDir dir;
  EXPECT_FALSE(gameactivity::data_found({}));
  EXPECT_FALSE(gameactivity::data_found(dir.path()));
  write(dir.path() / "GameActivity" / "Configurations.json", "[]");
  write(dir.path() / ("SaveSession_" + k_hades + ".json"), "{}");
  EXPECT_FALSE(gameactivity::data_found(dir.path()));
  write(dir.path() / "GameActivity" / (k_hades + ".json"), "{\"Items\":[]}");
  EXPECT_TRUE(gameactivity::data_found(dir.path()));
}
