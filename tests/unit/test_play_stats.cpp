/**
 * @file tests/unit/test_play_stats.cpp
 * @brief Play statistics: the session log, the 5:00 day, periods, top, resume and per-game stats.
 */
#include "src/play_stats.h"

#include <chrono>
#include <cstdlib>
#include <gtest/gtest.h>

namespace {
  constexpr std::int64_t k_hour = 3600;
  constexpr std::int64_t k_day = 86400;

  // A host two hours ahead of UTC, all year: what the tests call "local".
  constexpr std::int64_t k_offset = 2 * k_hour;

  std::int64_t plus_two(std::int64_t utc) {
    return utc + k_offset;
  }

  std::int64_t day_number(int y, unsigned m, unsigned d) {
    using namespace std::chrono;
    return sys_days {year {y} / month {m} / day {d}}.time_since_epoch().count();
  }

  // A local wall-clock time on the test host, as the UTC instant it is.
  std::int64_t local(int y, unsigned m, unsigned d, int h, int mi = 0) {
    return day_number(y, m, d) * k_day + h * k_hour + mi * 60 - k_offset;
  }

  const std::string k_hades = "11111111-1111-1111-1111-111111111111";
  const std::string k_celeste = "22222222-2222-2222-2222-222222222222";
  const std::string k_secret = "33333333-3333-3333-3333-333333333333";

  play_stats::input_t base_input(std::int64_t now) {
    play_stats::input_t in;
    in.now = now;
    in.to_local = plus_two;
    return in;
  }

  play_stats::catalogue_t catalogue() {
    return {
      {k_hades, {"uuid-hades", "Hades"}},
      {k_celeste, {"uuid-celeste", "Celeste"}},
    };
  }

  std::int64_t bucket(const nlohmann::json &stats, const std::string &date) {
    for (const auto &b : stats["buckets"]) {
      if (b["date"] == date) {
        return b["seconds"].get<std::int64_t>();
      }
    }
    ADD_FAILURE() << "no bucket " << date;
    return -1;
  }
}  // namespace

TEST(PlayStatsLog, ReadsCouchPilotLinesAndSkipsTheBrokenOnes) {
  const std::string log =
    "{\"Game\":\"11111111-1111-1111-1111-111111111111\",\"Start\":\"2024-03-11T18:00:00.1234567Z\",\"Seconds\":3600}\r\n"
    "\n"
    "{\"Game\":\"22222222-2222-2222-2222-222222222222\",\"Start\":\"2024-03-11T20:00:00Z\",\"Seconds\":0}\n"
    "{\"Game\":\"22222222-2222-2222-2222-222222222222\",\"Start\":\"2024-03-11T20:00:00+01:00\",\"Seconds\":60}\n"
    "{\"Game\":\"{33333333-3333-3333-3333-33333333333F}\",\"Start\":\"2024-03-12T20:00:00Z\",\"Seconds\":-5}\n"
    "{\"Game\":\"33333333-3333-3333-3333-333333333333\",\"Start\":\"2024-03-1";  // cut short mid-write
  const auto sessions = play_stats::parse_session_log(log, plus_two);
  ASSERT_EQ(sessions.size(), 2u);
  EXPECT_EQ(sessions[0].game, k_hades);
  EXPECT_EQ(sessions[0].start, day_number(2024, 3, 11) * k_day + 18 * k_hour);
  EXPECT_EQ(sessions[0].seconds, 3600);
  EXPECT_EQ(sessions[1].game, k_celeste);
  EXPECT_EQ(sessions[1].start, day_number(2024, 3, 11) * k_day + 19 * k_hour);
}

TEST(PlayStatsLog, NormalizesIdsAndToleratesABom) {
  const auto sessions = play_stats::parse_session_log("\xEF\xBB\xBF{\"Game\":\"{AAAAAAAA-1111-1111-1111-111111111111}\",\"Start\":\"2024-03-11T18:00:00Z\",\"Seconds\":5}", plus_two);
  ASSERT_EQ(sessions.size(), 1u);
  EXPECT_EQ(sessions[0].game, "aaaaaaaa-1111-1111-1111-111111111111");
}

TEST(PlayStatsTime, ParsesIso8601TheWayDotNetWritesIt) {
  const auto base = day_number(2024, 3, 11) * k_day + 18 * k_hour;
  EXPECT_EQ(play_stats::parse_iso8601("2024-03-11T18:00:00Z", plus_two), base);
  EXPECT_EQ(play_stats::parse_iso8601("2024-03-11T18:00:00.1234567Z", plus_two), base);
  EXPECT_EQ(play_stats::parse_iso8601("2024-03-11T20:00:00.0000000+02:00", plus_two), base);
  EXPECT_EQ(play_stats::parse_iso8601("2024-03-11T13:00:00-05:00", plus_two), base);
  // No zone: a local time.
  EXPECT_EQ(play_stats::parse_iso8601("2024-03-11T20:00:00", plus_two), base);
  EXPECT_FALSE(play_stats::parse_iso8601("yesterday", plus_two));
  EXPECT_FALSE(play_stats::parse_iso8601("2024-13-11T20:00:00Z", plus_two));
  EXPECT_EQ(play_stats::format_utc(base), "2024-03-11T18:00:00Z");
  EXPECT_EQ(play_stats::format_day(day_number(2024, 2, 29)), "2024-02-29");
}

TEST(PlayStatsTime, TheDayStartsAtFive) {
  EXPECT_EQ(play_stats::day_of(day_number(2024, 3, 12) * k_day + 4 * k_hour + 3599), day_number(2024, 3, 11));
  EXPECT_EQ(play_stats::day_of(day_number(2024, 3, 12) * k_day + 5 * k_hour), day_number(2024, 3, 12));
}

TEST(PlayStatsTime, OffsetIsAnIntegerFromMinus500ToZero) {
  EXPECT_EQ(play_stats::parse_offset("0"), 0);
  EXPECT_EQ(play_stats::parse_offset("-3"), -3);
  EXPECT_EQ(play_stats::parse_offset("4"), 0);
  EXPECT_EQ(play_stats::parse_offset("-9999"), -500);
  EXPECT_EQ(play_stats::parse_offset("abc"), 0);
  EXPECT_EQ(play_stats::parse_offset(""), 0);
}

TEST(PlayStatsOverview, ALateSessionBelongsToTheEveningItStarted) {
  // Wednesday 13 March 2024, midday.
  auto in = base_input(local(2024, 3, 13, 12));
  // Monday 23:00 to Tuesday 1:00: all Monday's.
  in.sessions.push_back({k_hades, local(2024, 3, 11, 23), 2 * k_hour});
  // Tuesday 4:00 to 6:00: an hour each side of the 5:00 boundary.
  in.sessions.push_back({k_hades, local(2024, 3, 12, 4), 2 * k_hour});
  const auto stats = play_stats::overview(in, "week", 0, catalogue());
  EXPECT_EQ(bucket(stats, "2024-03-11"), 3 * k_hour);
  EXPECT_EQ(bucket(stats, "2024-03-12"), 1 * k_hour);
  EXPECT_EQ(stats["total_seconds"], 4 * k_hour);
  EXPECT_EQ(stats["sessions"], 2);
}

TEST(PlayStatsOverview, AWeekRunsMondayToMondayAndOffsetsStepBack) {
  auto in = base_input(local(2024, 3, 13, 12));
  in.sessions.push_back({k_hades, local(2024, 3, 11, 20), k_hour});
  in.sessions.push_back({k_celeste, local(2024, 3, 6, 20), 30 * 60});
  in.sessions.push_back({k_celeste, local(2024, 3, 17, 23), 30 * 60});  // Sunday night

  const auto now = play_stats::overview(in, "week", 0, catalogue());
  EXPECT_EQ(now["range"], "week");
  EXPECT_EQ(now["from"], "2024-03-11");
  EXPECT_EQ(now["to"], "2024-03-18");
  EXPECT_EQ(now["today"], "2024-03-13");
  ASSERT_EQ(now["buckets"].size(), 7u);
  EXPECT_EQ(now["buckets"][0]["date"], "2024-03-11");
  EXPECT_EQ(bucket(now, "2024-03-17"), 30 * 60);
  EXPECT_EQ(now["total_seconds"], k_hour + 30 * 60);

  const auto before = play_stats::overview(in, "week", -1, catalogue());
  EXPECT_EQ(before["offset"], -1);
  EXPECT_EQ(before["from"], "2024-03-04");
  EXPECT_EQ(before["to"], "2024-03-11");
  EXPECT_EQ(before["total_seconds"], 30 * 60);
  EXPECT_EQ(before["previous_from"], "2024-02-26");

  // An unknown range is a week.
  EXPECT_EQ(play_stats::overview(in, "fortnight", 0, catalogue())["range"], "week");
}

TEST(PlayStatsOverview, BeforeFiveOnMondayIsStillLastWeek) {
  auto in = base_input(local(2024, 3, 18, 3));
  const auto stats = play_stats::overview(in, "week", 0, catalogue());
  EXPECT_EQ(stats["today"], "2024-03-17");
  EXPECT_EQ(stats["from"], "2024-03-11");
}

TEST(PlayStatsOverview, AMonthHasDayBucketsAndIsComparedUpToTheSameDay) {
  // The 5th of March: the previous period is the first 5 days of February.
  auto in = base_input(local(2024, 3, 5, 12));
  in.sessions.push_back({k_hades, local(2024, 3, 2, 20), k_hour});
  in.sessions.push_back({k_hades, local(2024, 2, 5, 20), 600});  // 5th of February: counted
  in.sessions.push_back({k_hades, local(2024, 2, 6, 20), 900});  // 6th: not yet
  const auto stats = play_stats::overview(in, "month", 0, catalogue());
  EXPECT_EQ(stats["from"], "2024-03-01");
  EXPECT_EQ(stats["to"], "2024-04-01");
  EXPECT_EQ(stats["buckets"].size(), 31u);
  EXPECT_EQ(stats["previous_from"], "2024-02-01");
  EXPECT_EQ(stats["previous_total_seconds"], 600);

  // A past month is compared with the whole month before it.
  const auto february = play_stats::overview(in, "month", -1, catalogue());
  EXPECT_EQ(february["from"], "2024-02-01");
  EXPECT_EQ(february["to"], "2024-03-01");
  EXPECT_EQ(february["buckets"].size(), 29u);
  EXPECT_EQ(february["total_seconds"], 1500);
  EXPECT_EQ(february["previous_from"], "2024-01-01");

  // Offsets cross years.
  const auto last_year = play_stats::overview(in, "month", -3, catalogue());
  EXPECT_EQ(last_year["from"], "2023-12-01");
  EXPECT_EQ(last_year["to"], "2024-01-01");
}

TEST(PlayStatsOverview, AYearHasMonthBuckets) {
  auto in = base_input(local(2024, 3, 5, 12));
  in.sessions.push_back({k_hades, local(2024, 1, 10, 20), k_hour});
  in.sessions.push_back({k_hades, local(2024, 1, 31, 23), k_hour});  // still January's
  in.sessions.push_back({k_hades, local(2024, 3, 1, 20), 120});
  in.sessions.push_back({k_hades, local(2023, 2, 1, 20), 50});  // previous year, before the cut
  in.sessions.push_back({k_hades, local(2023, 6, 1, 20), 70});  // previous year, after the cut
  const auto stats = play_stats::overview(in, "year", 0, catalogue());
  EXPECT_EQ(stats["from"], "2024-01-01");
  EXPECT_EQ(stats["to"], "2025-01-01");
  ASSERT_EQ(stats["buckets"].size(), 12u);
  EXPECT_EQ(bucket(stats, "2024-01-01"), 2 * k_hour);
  EXPECT_EQ(bucket(stats, "2024-03-01"), 120);
  EXPECT_EQ(stats["previous_total_seconds"], 50);

  const auto previous = play_stats::overview(in, "year", -1, catalogue());
  EXPECT_EQ(previous["from"], "2023-01-01");
  EXPECT_EQ(previous["total_seconds"], 120);
}

TEST(PlayStatsOverview, TopListsOnlyTheCallersGamesButTotalsCountEverything) {
  auto in = base_input(local(2024, 3, 13, 12));
  in.sessions.push_back({k_secret, local(2024, 3, 11, 20), 3 * k_hour});
  in.sessions.push_back({k_celeste, local(2024, 3, 11, 10), k_hour});
  in.sessions.push_back({k_hades, local(2024, 3, 12, 10), 2 * k_hour});
  for (int i = 0; i < 6; ++i) {
    const std::string id = "4444444" + std::to_string(i) + "-0000-0000-0000-000000000000";
    in.sessions.push_back({id, local(2024, 3, 12, 12), 60 + i});
  }
  auto cat = catalogue();
  for (int i = 0; i < 6; ++i) {
    const std::string id = "4444444" + std::to_string(i) + "-0000-0000-0000-000000000000";
    cat[id] = {"uuid-" + std::to_string(i), "Game " + std::to_string(i)};
  }
  const auto stats = play_stats::overview(in, "week", 0, cat);
  const auto &top = stats["top"];
  ASSERT_EQ(top.size(), 5u);
  EXPECT_EQ(top[0]["uuid"], "uuid-hades");
  EXPECT_EQ(top[0]["name"], "Hades");
  EXPECT_EQ(top[0]["seconds"], 2 * k_hour);
  EXPECT_EQ(top[1]["uuid"], "uuid-celeste");
  EXPECT_EQ(top[2]["uuid"], "uuid-5");
  for (const auto &entry : top) {
    EXPECT_NE(entry["name"], "");
  }
  EXPECT_EQ(stats["total_seconds"], 6 * k_hour + 6 * 60 + 15);
  EXPECT_EQ(stats["sessions"], 9);
}

TEST(PlayStatsOverview, LibraryAndResumeComeFromPlayniteTotals) {
  const auto now = local(2024, 3, 13, 12);
  auto in = base_input(now);
  // Long-played, installed, untouched for months: on the shelf.
  in.games.push_back({k_hades, "Hades", 40 * k_hour, 10, now - 200 * k_day, true, false});
  // Same, but outside the caller's catalogue: counted, never named.
  in.games.push_back({k_secret, "Secret", 90 * k_hour, 10, now - 200 * k_day, true, false});
  // Played this year: not on the shelf.
  in.games.push_back({k_celeste, "Celeste", 10 * k_hour, 3, now - 10 * k_day, true, false});
  // Installed, never played.
  in.games.push_back({"55555555-0000-0000-0000-000000000000", "New", 0, 0, std::nullopt, true, false});
  // Not installed.
  in.games.push_back({"66666666-0000-0000-0000-000000000000", "Old", 5 * k_hour, 1, local(2023, 12, 31, 23), false, false});
  // Hidden: not part of the library at all.
  in.games.push_back({"77777777-0000-0000-0000-000000000000", "Hidden", 100 * k_hour, 1, now - 200 * k_day, true, true});
  const auto stats = play_stats::overview(in, "week", 0, catalogue());
  const auto &lib = stats["library"];
  EXPECT_EQ(lib["games"], 5);
  EXPECT_EQ(lib["playtime_seconds"], 145 * k_hour);
  EXPECT_EQ(lib["installed"], 4);
  EXPECT_EQ(lib["installed_never_played"], 1);
  EXPECT_EQ(lib["played_this_year"], 1);

  const auto &resume = stats["resume"];
  ASSERT_EQ(resume.size(), 1u);
  EXPECT_EQ(resume[0]["uuid"], "uuid-hades");
  EXPECT_EQ(resume[0]["name"], "Hades");
  EXPECT_EQ(resume[0]["playtime_seconds"], 40 * k_hour);
  EXPECT_EQ(resume[0]["last_activity"], play_stats::format_utc(now - 200 * k_day));
}

TEST(PlayStatsOverview, ResumeHoldsAtMostSixLongestFirst) {
  const auto now = local(2024, 3, 13, 12);
  auto in = base_input(now);
  play_stats::catalogue_t cat;
  for (int i = 0; i < 8; ++i) {
    const std::string id = "8888888" + std::to_string(i) + "-0000-0000-0000-000000000000";
    in.games.push_back({id, "G", (i + 1) * k_hour, 1, now - 100 * k_day, true, false});
    cat[id] = {"uuid-" + std::to_string(i), "G" + std::to_string(i)};
  }
  const auto resume = play_stats::overview(in, "week", 0, cat)["resume"];
  ASSERT_EQ(resume.size(), 6u);
  EXPECT_EQ(resume[0]["uuid"], "uuid-7");
  EXPECT_EQ(resume[5]["uuid"], "uuid-2");
}

TEST(PlayStatsOverview, ARunningGameCountsUpToNow) {
  const auto now = local(2024, 3, 13, 21);
  auto in = base_input(now);
  in.running.push_back({k_hades, now - k_hour});
  const auto stats = play_stats::overview(in, "week", 0, catalogue());
  EXPECT_EQ(stats["total_seconds"], k_hour);
  EXPECT_EQ(stats["sessions"], 1);
  EXPECT_EQ(stats["top"][0]["uuid"], "uuid-hades");
  EXPECT_EQ(stats["tracking_since"], "2024-03-13");
}

TEST(PlayStatsOverview, ARunningGameWhoseStopTheLogHasIsNotCountedTwice) {
  const auto now = local(2024, 3, 13, 21);
  auto in = base_input(now);
  // The host missed the stop; the connector logged it.
  in.running.push_back({k_hades, now - 3 * k_hour});
  in.sessions.push_back({k_hades, now - 3 * k_hour - 120, 2 * k_hour});
  const auto stats = play_stats::overview(in, "week", 0, catalogue());
  EXPECT_EQ(stats["total_seconds"], 2 * k_hour);
  EXPECT_EQ(stats["sessions"], 1);
}

TEST(PlayStatsOverview, NothingLoggedYet) {
  auto in = base_input(local(2024, 3, 13, 21));
  const auto stats = play_stats::overview(in, "week", 0, catalogue());
  EXPECT_TRUE(stats["tracking_since"].is_null());
  EXPECT_TRUE(stats["achievements"].is_null());
  EXPECT_EQ(stats["total_seconds"], 0);
  EXPECT_TRUE(stats["top"].empty());
  EXPECT_TRUE(stats["resume"].empty());
}

TEST(PlayStatsOverview, AchievementsAreAskedForTheDaysOfPlay) {
  auto in = base_input(local(2024, 3, 13, 21));
  std::int64_t from = 0;
  std::int64_t to = 0;
  const auto stats = play_stats::overview(in, "week", 0, catalogue(), [&](std::int64_t f, std::int64_t t) {
    from = f;
    to = t;
    return nlohmann::json {{"unlocked", 3}, {"recent", nlohmann::json::array()}};
  });
  EXPECT_EQ(from, local(2024, 3, 11, 5));
  EXPECT_EQ(to, local(2024, 3, 18, 5));
  EXPECT_EQ(stats["achievements"]["unlocked"], 3);
}

TEST(PlayStatsGame, SessionsWeeksAndTheLastOne) {
  const auto now = local(2024, 3, 13, 21);
  auto in = base_input(now);
  in.games.push_back({k_hades, "Hades", 50 * k_hour, 12, local(2024, 3, 12, 22), true, false});
  in.sessions.push_back({k_celeste, local(2024, 1, 2, 20), k_hour});  // the log's first line
  in.sessions.push_back({k_hades, local(2024, 3, 12, 20), 2 * k_hour});  // this week
  in.sessions.push_back({k_hades, local(2024, 3, 4, 20), k_hour});  // last week
  in.sessions.push_back({k_hades, local(2023, 3, 4, 20), k_hour});  // a year ago: no bucket
  const auto stats = play_stats::game(in, "{11111111-1111-1111-1111-111111111111}", "uuid-hades");
  ASSERT_TRUE(stats);
  const auto &s = *stats;
  EXPECT_EQ(s["uuid"], "uuid-hades");
  EXPECT_EQ(s["playtime_seconds"], 50 * k_hour);
  EXPECT_EQ(s["play_count"], 12);
  EXPECT_EQ(s["last_activity"], play_stats::format_utc(local(2024, 3, 12, 22)));
  EXPECT_EQ(s["sessions"], 3);
  EXPECT_EQ(s["average_seconds"], (4 * k_hour) / 3);
  ASSERT_EQ(s["weeks"].size(), 12u);
  EXPECT_EQ(s["weeks"][11], 2 * k_hour);
  EXPECT_EQ(s["weeks"][10], k_hour);
  EXPECT_EQ(s["weeks"][0], 0);
  EXPECT_EQ(s["last_session"]["start"], play_stats::format_utc(local(2024, 3, 12, 20)));
  EXPECT_EQ(s["last_session"]["seconds"], 2 * k_hour);
  EXPECT_EQ(s["tracking_since"], "2023-03-04");
}

TEST(PlayStatsGame, AGameNeverLoggedOrUnknown) {
  auto in = base_input(local(2024, 3, 13, 21));
  in.games.push_back({k_celeste, "Celeste", 0, 0, std::nullopt, true, false});
  const auto stats = play_stats::game(in, k_celeste, "uuid-celeste");
  ASSERT_TRUE(stats);
  EXPECT_TRUE((*stats)["last_session"].is_null());
  EXPECT_TRUE((*stats)["last_activity"].is_null());
  EXPECT_EQ((*stats)["average_seconds"], 0);
  EXPECT_FALSE(play_stats::game(in, k_hades, "uuid-hades"));
}

TEST(PlayStatsGame, ARunningSessionIsTheLastOne) {
  const auto now = local(2024, 3, 13, 21);
  auto in = base_input(now);
  in.games.push_back({k_hades, "Hades", 50 * k_hour, 12, std::nullopt, true, false});
  in.sessions.push_back({k_hades, local(2024, 3, 12, 20), 2 * k_hour});
  in.running.push_back({k_hades, now - 600});
  const auto stats = play_stats::game(in, k_hades, "uuid-hades");
  ASSERT_TRUE(stats);
  EXPECT_EQ((*stats)["sessions"], 2);
  EXPECT_EQ((*stats)["last_session"]["seconds"], 600);
}

TEST(PlayStatsTime, TheOsZoneIsWithinAFewHoursOfUtcAndRoundTrips) {
  const std::int64_t utc = day_number(2024, 7, 1) * k_day + 12 * k_hour;
  const auto local = play_stats::os_utc_to_local(utc);
  EXPECT_LE(std::abs(local - utc), 14 * k_hour);
  EXPECT_EQ(play_stats::local_to_utc(local, play_stats::os_utc_to_local), utc);
}
