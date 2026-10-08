/**
 * @file tests/unit/test_successstory.cpp
 * @brief SuccessStory's files: parsing, the DTOs CouchPilot shows, the cache and icon paths.
 */
#include "src/successstory.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <random>

namespace fs = std::filesystem;

namespace {
  std::int64_t utc(std::int64_t utc) {
    return utc;
  }

  const std::string k_hades = "11111111-1111-1111-1111-111111111111";
  const std::string k_celeste = "22222222-2222-2222-2222-222222222222";
  const std::string k_secret = "33333333-3333-3333-3333-333333333333";

  constexpr std::int64_t k_2024_03_11_18h = 1710180000;  // 2024-03-11T18:00:00Z

  nlohmann::json item(const std::string &name, const nlohmann::json &date, float percent = 50) {
    return {
      {"Name", name},
      {"ApiName", "API_" + name},
      {"Description", name + " description"},
      {"UrlUnlocked", "https://cdn.example.com/" + name + ".jpg"},
      {"UrlLocked", "https://cdn.example.com/" + name + "_locked.jpg"},
      {"DateUnlocked", date},
      {"IsHidden", false},
      {"Percent", percent},
      {"GamerScore", 0},
      {"NoRarety", false},
    };
  }

  nlohmann::json game_file(const std::string &id, const std::vector<nlohmann::json> &items, bool ignored = false) {
    return {
      {"Id", id},
      {"Name", "Some game"},
      {"Items", nlohmann::json(items)},
      {"DateLastRefresh", "2024-03-11T18:00:00Z"},
      {"IsIgnored", ignored},
      {"SourcesLink", nullptr},
    };
  }

  successstory::game_t parse(const nlohmann::json &j, const std::string &id = k_hades) {
    auto parsed = successstory::parse_game(j.dump(), id, utc);
    EXPECT_TRUE(parsed);
    return parsed.value_or(successstory::game_t {});
  }

  class temp_dir_t {
  public:
    temp_dir_t() {
      std::random_device rd;
      path_ = fs::temp_directory_path() / ("vibepollo_successstory_" + std::to_string(rd()) + std::to_string(rd()));
      fs::create_directories(path_);
    }

    ~temp_dir_t() {
      std::error_code ec;
      fs::remove_all(path_, ec);
    }

    const fs::path &path() const {
      return path_;
    }

  private:
    fs::path path_;
  };

  void write(const fs::path &p, const std::string &content) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << content;
  }
}  // namespace

TEST(SuccessStoryParse, ReadsTheDatesTheWaySuccessStoryWritesThem) {
  const auto g = parse(game_file(k_hades, {
                                            item("dated", "2024-03-11T18:00:00Z"),
                                            item("unknown_date", "1982-12-15T00:00:00"),
                                            item("locked_null", nullptr),
                                            item("locked_zero", "0001-01-01T00:00:00"),
                                            item("offset", "2024-03-11T20:00:00+02:00"),
                                          }));
  ASSERT_EQ(g.items.size(), 5u);
  EXPECT_TRUE(g.items[0].unlocked);
  EXPECT_EQ(g.items[0].unlocked_at, k_2024_03_11_18h);
  // 1982: unlocked, but nobody knows when.
  EXPECT_TRUE(g.items[1].unlocked);
  EXPECT_FALSE(g.items[1].unlocked_at);
  EXPECT_FALSE(g.items[2].unlocked);
  EXPECT_FALSE(g.items[3].unlocked);
  EXPECT_EQ(g.items[4].unlocked_at, k_2024_03_11_18h);
  EXPECT_EQ(g.last_refresh, k_2024_03_11_18h);
  EXPECT_EQ(g.id, k_hades);
}

TEST(SuccessStoryParse, TakesTheIdFromTheFileNameWhenTheFileHasNone) {
  auto j = game_file("00000000-0000-0000-0000-000000000000", {});
  EXPECT_EQ(parse(j, k_celeste).id, k_celeste);
  j.erase("Id");
  EXPECT_EQ(parse(j, k_celeste).id, k_celeste);
  EXPECT_FALSE(successstory::parse_game("{\"Items\": [", k_hades, utc));
  EXPECT_FALSE(successstory::parse_game("[]", k_hades, utc));
  // A BOM, as .NET sometimes writes one.
  EXPECT_TRUE(successstory::parse_game("\xEF\xBB\xBF" + game_file(k_hades, {}).dump(), k_hades, utc));
}

TEST(SuccessStoryDto, MapsAnItemLikeCouchPilot) {
  auto a = parse(game_file(k_hades, {item("dated", "2024-03-11T18:00:00Z", 12.345f)})).items[0];
  auto j = successstory::item_json(a, 0, "uuid-hades");
  EXPECT_EQ(j["id"], "API_dated");
  EXPECT_EQ(j["name"], "dated");
  EXPECT_EQ(j["description"], "dated description");
  EXPECT_EQ(j["unlocked"], true);
  EXPECT_EQ(j["unlocked_at"], "2024-03-11T18:00:00Z");
  EXPECT_EQ(j["hidden"], false);
  EXPECT_DOUBLE_EQ(j["percent"].get<double>(), 12.3);
  EXPECT_TRUE(j["gamer_score"].is_null());
  EXPECT_EQ(j["icon"], "https://cdn.example.com/dated.jpg");
  EXPECT_EQ(j["locked_icon"], "https://cdn.example.com/dated_locked.jpg");
}

TEST(SuccessStoryDto, UnknownRarityHiddenAndGamerScore) {
  auto raw = item("x", nullptr, 100);
  raw["IsHidden"] = true;
  raw["GamerScore"] = 15;
  auto j = successstory::item_json(parse(game_file(k_hades, {raw})).items[0], 0, "u");
  // Percent 100 is SuccessStory's default: unknown.
  EXPECT_TRUE(j["percent"].is_null());
  EXPECT_EQ(j["hidden"], true);
  EXPECT_DOUBLE_EQ(j["gamer_score"].get<double>(), 15.0);
  EXPECT_TRUE(j["unlocked_at"].is_null());
  EXPECT_EQ(j["unlocked"], false);

  auto no_rarety = item("y", nullptr, 4);
  no_rarety["NoRarety"] = true;
  EXPECT_TRUE(successstory::item_json(parse(game_file(k_hades, {no_rarety})).items[0], 0, "u")["percent"].is_null());
}

TEST(SuccessStoryDto, TheIdFallsBackToThePosition) {
  auto raw = item("x", nullptr);
  raw["ApiName"] = "";
  EXPECT_EQ(successstory::item_json(parse(game_file(k_hades, {raw})).items[0], 7, "u")["id"], "7");
  raw.erase("ApiName");
  EXPECT_EQ(successstory::item_json(parse(game_file(k_hades, {raw})).items[0], 3, "u")["id"], "3");
}

TEST(SuccessStoryDto, SteamsGenericLockedIconIsDropped) {
  auto raw = item("x", nullptr);
  raw["UrlUnlocked"] = "https://steamcdn-a.akamaihd.net/steamcommunity/public/images/apps/1145360/0123456789abcdef0123456789abcdef01234567.jpg";
  raw["UrlLocked"] = "https://steamcdn-a.akamaihd.net/steam/apps/lock.jpg";
  auto j = successstory::item_json(parse(game_file(k_hades, {raw})).items[0], 0, "u");
  EXPECT_TRUE(j["locked_icon"].is_null());
  // The same URL twice is no locked icon either.
  raw["UrlLocked"] = raw["UrlUnlocked"];
  EXPECT_TRUE(successstory::item_json(parse(game_file(k_hades, {raw})).items[0], 0, "u")["locked_icon"].is_null());
  // A real, long Steam locked icon is kept.
  raw["UrlLocked"] = "https://steamcdn-a.akamaihd.net/steamcommunity/public/images/apps/1145360/fedcba9876543210fedcba9876543210fedcba98.jpg";
  EXPECT_EQ(successstory::item_json(parse(game_file(k_hades, {raw})).items[0], 0, "u")["locked_icon"], raw["UrlLocked"]);
}

TEST(SuccessStoryDto, LocalIconsGoThroughTheHost) {
  auto raw = item("x", nullptr);
  raw["UrlUnlocked"] = "rpcs3\\NPWR00001\\TROP001.PNG";
  raw["UrlLocked"] = "rpcs3\\NPWR00001\\TROP001_locked.PNG";
  auto j = successstory::item_json(parse(game_file(k_hades, {raw})).items[0], 4, "uuid-x");
  EXPECT_EQ(j["icon"], "/appachievementicon?appuuid=uuid-x&index=4");
  EXPECT_EQ(j["locked_icon"], "/appachievementicon?appuuid=uuid-x&index=4&locked=1");
  raw["UrlUnlocked"] = "";
  EXPECT_TRUE(successstory::item_json(parse(game_file(k_hades, {raw})).items[0], 4, "uuid-x")["icon"].is_null());
}

TEST(SuccessStoryDto, AGameIsSortedLikeCouchPilot) {
  const auto g = parse(game_file(k_hades, {
                                            item("locked_common", nullptr, 80),
                                            item("unlocked_old", "2024-01-01T10:00:00Z"),
                                            item("unlocked_undated", "1982-01-01T00:00:00"),
                                            item("locked_rare", nullptr, 1.5f),
                                            item("unlocked_new", "2024-03-01T10:00:00Z"),
                                            item("locked_unknown", nullptr, 100),
                                          }));
  const auto j = successstory::game_json(g, "uuid-hades");
  ASSERT_TRUE(j);
  EXPECT_EQ((*j)["uuid"], "uuid-hades");
  EXPECT_EQ((*j)["total"], 6);
  EXPECT_EQ((*j)["unlocked"], 3);
  EXPECT_EQ((*j)["last_refresh"], "2024-03-11T18:00:00Z");
  std::vector<std::string> order;
  for (const auto &i : (*j)["items"]) {
    order.push_back(i["name"]);
  }
  EXPECT_EQ(order, (std::vector<std::string> {"unlocked_new", "unlocked_old", "unlocked_undated", "locked_common", "locked_rare", "locked_unknown"}));
  // The icon URL keeps the item's position in the file, not in the sorted list.
  auto local_icon = item("local", nullptr);
  local_icon["UrlUnlocked"] = "icons/a.png";
  const auto with_local = successstory::game_json(parse(game_file(k_hades, {item("a", "2024-03-01T10:00:00Z"), local_icon, item("b", "2024-03-02T10:00:00Z")})), "u");
  EXPECT_EQ((*with_local)["items"][2]["icon"], "/appachievementicon?appuuid=u&index=1");
}

TEST(SuccessStoryDto, NoneOrIgnoredIsNotFound) {
  EXPECT_FALSE(successstory::game_json(parse(game_file(k_hades, {})), "u"));
  EXPECT_FALSE(successstory::game_json(parse(game_file(k_hades, {item("x", nullptr)}, true)), "u"));
}

TEST(SuccessStoryStore, ReadsCachesAndNoticesChanges) {
  temp_dir_t tmp;
  successstory::store_t store(tmp.path(), {}, utc, std::chrono::seconds(0));
  EXPECT_FALSE(store.available());
  EXPECT_FALSE(store.game(k_hades));

  const auto file = tmp.path() / "SuccessStory" / (k_hades + ".json");
  write(file, game_file(k_hades, {item("a", nullptr)}).dump());
  EXPECT_TRUE(store.available());
  auto first = store.game(k_hades);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->items.size(), 1u);
  // Unchanged: the same cached object.
  EXPECT_EQ(store.game(k_hades), first);

  write(file, game_file(k_hades, {item("a", nullptr), item("b", "2024-03-11T18:00:00Z")}).dump());
  fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(5));
  auto second = store.game(k_hades);
  ASSERT_TRUE(second);
  EXPECT_EQ(second->items.size(), 2u);

  // Cut short mid-write: keep what we had.
  write(file, "{\"Items\": [");
  fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(10));
  EXPECT_EQ(store.game(k_hades), second);

  fs::remove(file);
  EXPECT_FALSE(store.game(k_hades));
  // Ids that are not GUIDs are never turned into paths.
  EXPECT_FALSE(store.game("..\\..\\boot"));
}

TEST(SuccessStoryStore, UnlocksInARangeNewestFirstSkippingIgnoredGames) {
  temp_dir_t tmp;
  const auto folder = tmp.path() / "SuccessStory";
  write(folder / (k_hades + ".json"), game_file(k_hades, {
                                                           item("h1", "2024-03-10T10:00:00Z"),
                                                           item("h2", "2024-03-12T10:00:00Z"),
                                                           item("h_undated", "1982-01-01T00:00:00"),
                                                           item("h_locked", nullptr),
                                                         })
                                        .dump());
  write(folder / (k_celeste + ".json"), game_file(k_celeste, {item("c1", "2024-03-11T10:00:00Z")}).dump());
  write(folder / (k_secret + ".json"), game_file(k_secret, {item("s1", "2024-03-11T12:00:00Z")}).dump());
  write(folder / "33333333-3333-3333-3333-333333333334.json", game_file("33333333-3333-3333-3333-333333333334", {item("ignored", "2024-03-11T11:00:00Z")}, true).dump());
  write(folder / "not-a-guid.json.bak", "junk");

  successstory::store_t store(tmp.path(), {}, utc, std::chrono::seconds(0));
  const auto all = store.unlocked(0, std::numeric_limits<std::int64_t>::max());
  ASSERT_EQ(all.size(), 4u);
  EXPECT_EQ(all[0].game->items[all[0].index].name, "h2");
  EXPECT_EQ(all[1].game->items[all[1].index].name, "s1");
  EXPECT_EQ(all[2].game->items[all[2].index].name, "c1");
  EXPECT_EQ(all[3].game->items[all[3].index].name, "h1");

  // `to` is exclusive.
  const auto some = store.unlocked(1710064800 /* 03-10 10:00Z */, 1710151200 /* 03-11 10:00Z */);
  ASSERT_EQ(some.size(), 1u);
  EXPECT_EQ(some[0].game->items[some[0].index].name, "h1");

  // Only the caller's games are named; at most `limit`.
  const play_stats::catalogue_t catalogue {{k_hades, {"uuid-hades", "Hades"}}, {k_celeste, {"uuid-celeste", "Celeste"}}};
  const auto listed = successstory::unlocks_json(all, catalogue, 40);
  ASSERT_EQ(listed.size(), 3u);
  EXPECT_EQ(listed[0]["name"], "h2");
  EXPECT_EQ(listed[0]["uuid"], "uuid-hades");
  EXPECT_EQ(listed[0]["game"], "Hades");
  EXPECT_EQ(listed[1]["uuid"], "uuid-celeste");
  EXPECT_EQ(successstory::unlocks_json(all, catalogue, 1).size(), 1u);

  // A game that disappears is dropped on the next look.
  fs::remove(folder / (k_secret + ".json"));
  EXPECT_EQ(store.unlocked(0, std::numeric_limits<std::int64_t>::max()).size(), 3u);
}

TEST(SuccessStoryIcons, ResolvesUnderTheRootsAndNowhereElse) {
  temp_dir_t tmp;
  const auto data = tmp.path() / "data";
  const auto resources = tmp.path() / "plugin" / "Resources";
  write(data / "rpcs3" / "TROP001.PNG", "png");
  write(resources / "PlayStation" / "bronze.png", "png");
  write(tmp.path() / "secret.png", "secret");
  write(tmp.path() / "data2" / "x.png", "sibling");
  const std::vector<fs::path> roots {data, resources};

  const auto in_data = successstory::resolve_icon("rpcs3\\TROP001.PNG", roots);
  ASSERT_TRUE(in_data);
  EXPECT_EQ(in_data->filename(), "TROP001.PNG");
  // A leading separator is "from the root", as SuccessStory means it.
  EXPECT_TRUE(successstory::resolve_icon("\\rpcs3\\TROP001.PNG", roots));
  EXPECT_TRUE(successstory::resolve_icon("PlayStation/bronze.png", roots));

  EXPECT_FALSE(successstory::resolve_icon("..\\secret.png", roots));
  EXPECT_FALSE(successstory::resolve_icon("rpcs3\\..\\..\\secret.png", roots));
  EXPECT_FALSE(successstory::resolve_icon("..\\data2\\x.png", roots));
  EXPECT_FALSE(successstory::resolve_icon("rpcs3\\missing.png", roots));
  EXPECT_FALSE(successstory::resolve_icon("rpcs3", roots));  // a folder
  EXPECT_FALSE(successstory::resolve_icon("https://example.com/x.png", roots));
  EXPECT_FALSE(successstory::resolve_icon("", roots));

  // A symlink inside the root that points out of it.
  std::error_code ec;
  fs::create_symlink(tmp.path() / "secret.png", data / "link.png", ec);
  if (!ec) {
    EXPECT_FALSE(successstory::resolve_icon("link.png", roots));
  }
}

TEST(SuccessStoryIcons, TheStoreServesOnlyLocalIconsOfKnownItems) {
  temp_dir_t tmp;
  write(tmp.path() / "rpcs3" / "a.png", "png");
  auto local_item = item("local", nullptr);
  local_item["UrlUnlocked"] = "rpcs3\\a.png";
  local_item["UrlLocked"] = "..\\..\\..\\etc\\passwd";
  write(tmp.path() / "SuccessStory" / (k_hades + ".json"), game_file(k_hades, {item("web", nullptr), local_item}).dump());
  successstory::store_t store(tmp.path(), {}, utc, std::chrono::seconds(0));
  EXPECT_FALSE(store.icon_path(k_hades, 0, false));  // on the web
  const auto path = store.icon_path(k_hades, 1, false);
  ASSERT_TRUE(path);
  EXPECT_EQ(successstory::content_type_for(*path), "image/png");
  EXPECT_FALSE(store.icon_path(k_hades, 1, true));
  EXPECT_FALSE(store.icon_path(k_hades, 2, false));
  EXPECT_FALSE(store.icon_path(k_celeste, 0, false));
}
