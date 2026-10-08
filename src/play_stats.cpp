/**
 * @file src/play_stats.cpp
 * @brief How much is played, by day, week, month and year: the pure half of `/appstats`.
 */

// local includes
#include "play_stats.h"

// standard includes
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <map>

namespace play_stats {
  namespace {
    constexpr std::int64_t k_day = 86400;
    // A day of play runs from 5:00 to 5:00.
    constexpr std::int64_t k_day_starts_at = 5 * 3600;
    // The resume shelf: installed games with an hour or more that nobody touched for 90 days.
    constexpr std::int64_t k_resume_after = 90 * k_day;
    constexpr std::int64_t k_resume_min_seconds = 3600;
    constexpr std::size_t k_resume_count = 6;
    constexpr std::size_t k_top_count = 5;
    constexpr int k_offset_min = -500;

    std::int64_t floor_div(std::int64_t a, std::int64_t b) {
      std::int64_t q = a / b;
      if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --q;
      }
      return q;
    }

    // Howard Hinnant's civil calendar conversions: exact for the proleptic Gregorian calendar and
    // independent of the C library's notion of time zones.
    std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
      y -= m <= 2;
      const std::int64_t era = floor_div(y, 400);
      const auto yoe = static_cast<unsigned>(y - era * 400);
      const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
      const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
      return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
    }

    struct civil_t {
      std::int64_t year;
      unsigned month;
      unsigned day;
    };

    civil_t civil_from_days(std::int64_t z) {
      z += 719468;
      const std::int64_t era = floor_div(z, 146097);
      const auto doe = static_cast<unsigned>(z - era * 146097);
      const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
      const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
      const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
      const unsigned mp = (5 * doy + 2) / 153;
      const unsigned d = doy - (153 * mp + 2) / 5 + 1;
      const unsigned m = mp < 10 ? mp + 3 : mp - 9;
      return {y + (m <= 2), m, d};
    }

    std::int64_t first_of_month(std::int64_t day) {
      const auto c = civil_from_days(day);
      return days_from_civil(c.year, c.month, 1);
    }

    std::int64_t add_months(std::int64_t first_day, std::int64_t months) {
      const auto c = civil_from_days(first_day);
      const std::int64_t index = c.year * 12 + (c.month - 1) + months;
      return days_from_civil(floor_div(index, 12), static_cast<unsigned>(index - floor_div(index, 12) * 12) + 1, 1);
    }

    std::int64_t monday_of(std::int64_t day) {
      // 1970-01-01 was a Thursday: (day + 3) mod 7 is 0 on Mondays.
      const std::int64_t weekday = ((day + 3) % 7 + 7) % 7;
      return day - weekday;
    }

    bool read_digits(std::string_view text, std::size_t &pos, std::size_t count, int &out) {
      if (pos + count > text.size()) {
        return false;
      }
      int value = 0;
      for (std::size_t i = 0; i < count; ++i) {
        const char c = text[pos + i];
        if (c < '0' || c > '9') {
          return false;
        }
        value = value * 10 + (c - '0');
      }
      pos += count;
      out = value;
      return true;
    }

    bool expect(std::string_view text, std::size_t &pos, char c) {
      if (pos < text.size() && text[pos] == c) {
        ++pos;
        return true;
      }
      return false;
    }

    std::string_view trim(std::string_view s) {
      while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
      }
      while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
      }
      return s;
    }

    // One session cut at a 5:00 boundary: a night-long session counts for both days it spans.
    struct day_part_t {
      const std::string *game;
      std::int64_t day;
      std::int64_t seconds;
    };

    template<class F>
    void split(const std::vector<session_t> &sessions, const to_local_t &to_local, F &&visit) {
      for (const auto &s : sessions) {
        // Local start plus the length, as CouchPilot does: wall-clock arithmetic, so a session
        // across a DST change is cut where the clock on the wall says 5:00.
        std::int64_t start = to_local(s.start);
        const std::int64_t end = start + s.seconds;
        while (start < end) {
          const std::int64_t day = day_of(start);
          const std::int64_t boundary = (day + 1) * k_day + k_day_starts_at;
          const std::int64_t stop = std::min(end, boundary);
          visit(day_part_t {&s.game, day, stop - start});
          start = stop;
        }
      }
    }

    // GameActivity's sessions, plus whatever is being played right now, up to now.
    //
    // GameActivity adds a run's item when the game starts and fills in its length when it stops,
    // so while the game runs its file may already hold that item, with 0 seconds (which the
    // reader drops) or a partial count. Any item of the same game that starts within a couple of
    // minutes before the host's start of the run, or after it, is that run: the live count
    // replaces it, so the run is never counted twice. Without GameActivity nothing is counted,
    // not even the running game, so every per-period figure agrees that there is no history.
    std::vector<session_t> sessions_with_running(const input_t &input) {
      if (!input.activity) {
        return {};
      }
      std::vector<session_t> all = input.sessions;
      for (const auto &r : input.running) {
        const auto id = normalize_id(r.game);
        const std::int64_t seconds = input.now - r.started_at;
        if (id.empty() || seconds <= 0) {
          continue;
        }
        std::erase_if(all, [&](const session_t &s) {
          return s.game == id && s.start >= r.started_at - k_running_match_slack;
        });
        all.push_back({id, r.started_at, seconds});
      }
      return all;
    }

    // The first day GameActivity has a session for (the running one included: GameActivity
    // already has its start), or null.
    nlohmann::json tracking_since(const std::vector<session_t> &sessions, const to_local_t &to_local) {
      if (sessions.empty()) {
        return nullptr;
      }
      const auto first = std::min_element(sessions.begin(), sessions.end(), [](const auto &a, const auto &b) {
        return a.start < b.start;
      });
      return format_day(day_of(to_local(first->start)));
    }

    const game_t *find_game(const std::vector<game_t> &games, const std::string &id) {
      for (const auto &g : games) {
        if (normalize_id(g.id) == id) {
          return &g;
        }
      }
      return nullptr;
    }

    nlohmann::json library(const input_t &input) {
      // Calendar year here, not days of play: CouchPilot compares with January 1st at midnight.
      const auto today = civil_from_days(floor_div(input.to_local(input.now), k_day));
      const std::int64_t this_year = local_to_utc(days_from_civil(today.year, 1, 1) * k_day, input.to_local);
      std::int64_t playtime = 0;
      std::int64_t games = 0;
      std::int64_t installed = 0;
      std::int64_t never_played = 0;
      std::int64_t this_year_count = 0;
      for (const auto &g : input.games) {
        if (g.hidden) {
          continue;
        }
        ++games;
        playtime += g.playtime_seconds;
        if (g.installed) {
          ++installed;
          if (g.playtime_seconds == 0) {
            ++never_played;
          }
        }
        if (g.last_activity && *g.last_activity >= this_year) {
          ++this_year_count;
        }
      }
      return {
        {"playtime_seconds", playtime},
        {"games", games},
        {"installed", installed},
        {"installed_never_played", never_played},
        {"played_this_year", this_year_count},
      };
    }

    // Installed games you put real time into and then left: an hour or more, nothing for three
    // months. The longest-played first.
    nlohmann::json resume(const input_t &input, const catalogue_t &catalogue) {
      std::vector<const game_t *> picks;
      for (const auto &g : input.games) {
        if (!g.hidden && g.installed && g.playtime_seconds >= k_resume_min_seconds && g.last_activity &&
            input.now - *g.last_activity > k_resume_after) {
          picks.push_back(&g);
        }
      }
      std::stable_sort(picks.begin(), picks.end(), [](const game_t *a, const game_t *b) {
        return a->playtime_seconds > b->playtime_seconds;
      });
      nlohmann::json out = nlohmann::json::array();
      for (const auto *g : picks) {
        if (out.size() >= k_resume_count) {
          break;
        }
        const auto it = catalogue.find(normalize_id(g->id));
        if (it == catalogue.end()) {
          continue;
        }
        out.push_back({
          {"uuid", it->second.uuid},
          {"name", it->second.name},
          {"playtime_seconds", g->playtime_seconds},
          {"last_activity", format_utc(*g->last_activity)},
        });
      }
      return out;
    }
  }  // namespace

  std::int64_t os_utc_to_local(std::int64_t utc) {
    const auto t = static_cast<std::time_t>(utc);
    std::tm tm {};
#ifdef _WIN32
    if (localtime_s(&tm, &t) != 0) {
      return utc;
    }
#else
    if (localtime_r(&t, &tm) == nullptr) {
      return utc;
    }
#endif
    return days_from_civil(tm.tm_year + 1900, static_cast<unsigned>(tm.tm_mon + 1), static_cast<unsigned>(tm.tm_mday)) * k_day +
           tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
  }

  std::int64_t local_to_utc(std::int64_t local, const to_local_t &to_local) {
    // Guess with the offset at the local time read as UTC, then once more with the offset at
    // the guess, which settles it everywhere but in the hour a DST change skips.
    std::int64_t guess = local - (to_local(local) - local);
    guess = local - (to_local(guess) - guess);
    return guess;
  }

  std::optional<std::int64_t> parse_iso8601(std::string_view text, const to_local_t &to_local) {
    text = trim(text);
    std::size_t pos = 0;
    int year = 0;
    int month = 0;
    int day = 0;
    if (!read_digits(text, pos, 4, year) || !expect(text, pos, '-') || !read_digits(text, pos, 2, month) ||
        !expect(text, pos, '-') || !read_digits(text, pos, 2, day)) {
      return std::nullopt;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31) {
      return std::nullopt;
    }
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (pos < text.size() && (text[pos] == 'T' || text[pos] == 't' || text[pos] == ' ')) {
      ++pos;
      if (!read_digits(text, pos, 2, hour) || !expect(text, pos, ':') || !read_digits(text, pos, 2, minute)) {
        return std::nullopt;
      }
      if (expect(text, pos, ':') && !read_digits(text, pos, 2, second)) {
        return std::nullopt;
      }
      if (expect(text, pos, '.') || expect(text, pos, ',')) {
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
          ++pos;
        }
      }
    }
    if (hour > 23 || minute > 59 || second > 60) {
      return std::nullopt;
    }
    const std::int64_t wall = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * k_day +
                              hour * 3600 + minute * 60 + second;
    if (pos == text.size()) {
      return local_to_utc(wall, to_local);
    }
    if (text[pos] == 'Z' || text[pos] == 'z') {
      return pos + 1 == text.size() ? std::optional<std::int64_t> {wall} : std::nullopt;
    }
    if (text[pos] != '+' && text[pos] != '-') {
      return std::nullopt;
    }
    const int sign = text[pos] == '-' ? -1 : 1;
    ++pos;
    int off_h = 0;
    int off_m = 0;
    if (!read_digits(text, pos, 2, off_h)) {
      return std::nullopt;
    }
    expect(text, pos, ':');
    if (pos < text.size() && !read_digits(text, pos, 2, off_m)) {
      return std::nullopt;
    }
    if (pos != text.size()) {
      return std::nullopt;
    }
    return wall - sign * (off_h * 3600 + off_m * 60);
  }

  int iso8601_year(std::string_view text) {
    text = trim(text);
    std::size_t pos = 0;
    int year = 0;
    return read_digits(text, pos, 4, year) ? year : 0;
  }

  std::string format_utc(std::int64_t utc) {
    const std::int64_t day = floor_div(utc, k_day);
    const std::int64_t rest = utc - day * k_day;
    const auto c = civil_from_days(day);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ", static_cast<long long>(c.year), c.month, c.day, static_cast<long long>(rest / 3600), static_cast<long long>(rest / 60 % 60), static_cast<long long>(rest % 60));
    return buf;
  }

  std::string format_day(std::int64_t day) {
    const auto c = civil_from_days(day);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02u", static_cast<long long>(c.year), c.month, c.day);
    return buf;
  }

  std::int64_t day_of(std::int64_t local) {
    return floor_div(local - k_day_starts_at, k_day);
  }

  std::string normalize_id(std::string_view id) {
    id = trim(id);
    if (id.size() >= 2 && id.front() == '{' && id.back() == '}') {
      id = id.substr(1, id.size() - 2);
    }
    std::string out(id);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return out;
  }

  int parse_offset(std::string_view text) {
    text = trim(text);
    if (text.empty() || text.size() > 6) {
      return 0;
    }
    int sign = 1;
    std::size_t pos = 0;
    if (text[0] == '-' || text[0] == '+') {
      sign = text[0] == '-' ? -1 : 1;
      pos = 1;
    }
    if (pos == text.size()) {
      return 0;
    }
    int value = 0;
    for (; pos < text.size(); ++pos) {
      if (text[pos] < '0' || text[pos] > '9') {
        return 0;
      }
      value = value * 10 + (text[pos] - '0');
    }
    return std::clamp(sign * value, k_offset_min, 0);
  }

  period_t period(std::string_view range, int offset, std::int64_t now, const to_local_t &to_local) {
    period_t p;
    p.offset = offset;
    p.today = day_of(to_local(now));
    if (range == "month") {
      p.range = "month";
      p.from = add_months(first_of_month(p.today), offset);
      p.to = add_months(p.from, 1);
      p.previous_from = add_months(p.from, -1);
    } else if (range == "year") {
      p.range = "year";
      const auto c = civil_from_days(p.today);
      p.from = days_from_civil(c.year + offset, 1, 1);
      p.to = days_from_civil(c.year + offset + 1, 1, 1);
      p.previous_from = days_from_civil(c.year + offset - 1, 1, 1);
    } else {
      p.range = "week";
      p.from = monday_of(p.today) + 7 * static_cast<std::int64_t>(offset);
      p.to = p.from + 7;
      p.previous_from = p.from - 7;
    }
    // The period under way is compared with the previous one up to the same point (the first 5
    // days of last month for the 5th of this one), not with all of it.
    p.previous_to = p.from;
    if (p.today >= p.from && p.today < p.to) {
      const std::int64_t same_day = p.previous_from + (p.today - p.from) + 1;
      if (same_day < p.previous_to) {
        p.previous_to = same_day;
      }
    }
    p.from_utc = local_to_utc(p.from * k_day + k_day_starts_at, to_local);
    p.to_utc = local_to_utc(p.to * k_day + k_day_starts_at, to_local);
    return p;
  }

  nlohmann::json overview(const input_t &input, std::string_view range, int offset, const catalogue_t &catalogue, const achievements_fn &achievements) {
    const auto p = period(range, offset, input.now, input.to_local);
    const auto sessions = sessions_with_running(input);
    const bool by_month = p.range == "year";

    std::map<std::int64_t, std::int64_t> buckets;
    for (std::int64_t d = p.from; d < p.to; d = by_month ? add_months(d, 1) : d + 1) {
      buckets[d] = 0;
    }
    // Per game, in the order games first appear, so ties keep CouchPilot's order.
    std::vector<std::pair<std::string, std::int64_t>> per_game;
    std::unordered_map<std::string, std::size_t> per_game_index;
    std::int64_t total = 0;
    std::int64_t previous = 0;
    split(sessions, input.to_local, [&](const day_part_t &part) {
      if (part.day >= p.from && part.day < p.to) {
        total += part.seconds;
        buckets[by_month ? first_of_month(part.day) : part.day] += part.seconds;
        const auto [it, inserted] = per_game_index.try_emplace(*part.game, per_game.size());
        if (inserted) {
          per_game.emplace_back(*part.game, 0);
        }
        per_game[it->second].second += part.seconds;
      } else if (part.day >= p.previous_from && part.day < p.previous_to) {
        previous += part.seconds;
      }
    });

    std::int64_t session_count = 0;
    for (const auto &s : sessions) {
      const auto day = day_of(input.to_local(s.start));
      if (day >= p.from && day < p.to) {
        ++session_count;
      }
    }

    nlohmann::json bucket_list = nlohmann::json::array();
    for (const auto &[day, seconds] : buckets) {
      bucket_list.push_back({{"date", format_day(day)}, {"seconds", seconds}});
    }

    std::stable_sort(per_game.begin(), per_game.end(), [](const auto &a, const auto &b) {
      return a.second > b.second;
    });
    nlohmann::json top = nlohmann::json::array();
    for (const auto &[id, seconds] : per_game) {
      if (top.size() >= k_top_count) {
        break;
      }
      const auto it = catalogue.find(id);
      if (it == catalogue.end()) {
        continue;
      }
      top.push_back({{"uuid", it->second.uuid}, {"name", it->second.name}, {"seconds", seconds}});
    }

    nlohmann::json out = nlohmann::json::object();
    out["activity"] = input.activity;
    out["range"] = p.range;
    out["offset"] = p.offset;
    out["from"] = format_day(p.from);
    out["to"] = format_day(p.to);
    out["today"] = format_day(p.today);
    out["tracking_since"] = tracking_since(sessions, input.to_local);
    out["total_seconds"] = total;
    out["previous_total_seconds"] = previous;
    out["previous_from"] = format_day(p.previous_from);
    out["sessions"] = session_count;
    out["buckets"] = std::move(bucket_list);
    out["top"] = std::move(top);
    out["library"] = library(input);
    out["resume"] = resume(input, catalogue);
    out["achievements"] = achievements ? achievements(p.from_utc, p.to_utc) : nlohmann::json(nullptr);
    return out;
  }

  std::optional<nlohmann::json> game(const input_t &input, std::string_view playnite_id, std::string_view uuid) {
    const auto id = normalize_id(playnite_id);
    const auto *g = id.empty() ? nullptr : find_game(input.games, id);
    if (!g) {
      return std::nullopt;
    }
    const auto sessions = sessions_with_running(input);
    std::vector<session_t> mine;
    for (const auto &s : sessions) {
      if (s.game == id) {
        mine.push_back(s);
      }
    }

    const std::int64_t this_week = monday_of(day_of(input.to_local(input.now)));
    std::vector<std::int64_t> weeks(12, 0);
    split(mine, input.to_local, [&](const day_part_t &part) {
      const std::int64_t index = 11 - (this_week - monday_of(part.day)) / 7;
      if (index >= 0 && index < 12) {
        weeks[static_cast<std::size_t>(index)] += part.seconds;
      }
    });

    nlohmann::json last_session = nullptr;
    std::int64_t sum = 0;
    const session_t *last = nullptr;
    for (const auto &s : mine) {
      sum += s.seconds;
      if (!last || s.start > last->start) {
        last = &s;
      }
    }
    if (last) {
      last_session = {{"start", format_utc(last->start)}, {"seconds", last->seconds}};
    }

    nlohmann::json out = nlohmann::json::object();
    out["uuid"] = std::string(uuid);
    out["activity"] = input.activity;
    out["playtime_seconds"] = g->playtime_seconds;
    out["play_count"] = g->play_count;
    out["last_activity"] = g->last_activity ? nlohmann::json(format_utc(*g->last_activity)) : nlohmann::json(nullptr);
    out["sessions"] = static_cast<std::int64_t>(mine.size());
    out["average_seconds"] = mine.empty() ? 0 : sum / static_cast<std::int64_t>(mine.size());
    out["weeks"] = weeks;
    out["last_session"] = std::move(last_session);
    out["tracking_since"] = tracking_since(sessions, input.to_local);
    return out;
  }

}  // namespace play_stats
