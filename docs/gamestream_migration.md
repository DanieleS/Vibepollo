# GameStream Migration
Nvidia announced that their GameStream service for Nvidia Games clients will be discontinued in February 2023.
Luckily, Sunshine performance is now equal to or better than Nvidia GameStream.

## Migration
We have developed a simple migration tool to help you migrate your GameStream games and apps to Sunshine automatically.
Please check out our [GSMS](https://github.com/LizardByte/GSMS) project if you're interested in an automated
migration option. GSMS offers the ability to migrate your custom and auto-detected games and apps. The
working directory, command, and image are all set in Sunshine's `apps.json` file. The box-art image is also copied
to a specified directory.

## Internet Streaming
If you are using the Moonlight Internet Hosting Tool, you can remove it from your system when you migrate to Sunshine.
To stream over the Internet with Sunshine and a UPnP-capable router, enable the UPnP option in the Sunshine Web UI.

> [!NOTE]
> Running Sunshine together with versions of the Moonlight Internet Hosting Tool prior to v5.6 will cause UPnP
> port forwarding to become unreliable. Either uninstall the tool entirely or update it to v5.6 or later.

## App Identity
Sunshine reports each app with a stable `UUID` and a numeric `ID` in `/applist`.
Clients should treat `UUID` as the persistent app identity. The numeric `ID` is a GameStream transport and artwork
cache key, so it may change after an app's cover art changes.

Newer clients can also read the optional `ArtVersion` field from `/applist` to invalidate cached artwork without
changing their saved app identity. Sunshine keeps old numeric IDs as compatibility aliases for launch and artwork
requests, but client state should be keyed by `UUID` when possible.

## App Metadata
Clients that want more than a title and a cover can call `GET /appmetadata`. It returns
`{"apps": [...]}` with one entry per app that carries metadata, keyed by `uuid`, and lists only apps the same client's
`/applist` shows. Every field other than `uuid`, `id`, `name` and `source` is optional and omitted when unknown:
`description`, `genres`, `developers`, `publishers`, `release_date` (`YYYY-MM-DD`), `community_score` and
`critic_score` (0-100), `last_played` (ISO 8601), `playtime_minutes`, `has_background`, `igdb_id` and `playnite_id`.

`source` says who wrote the descriptive fields, and therefore how to read `description`:

| `source`   | `description`                                                                      |
|:-----------|:-----------------------------------------------------------------------------------|
| `playnite` | HTML, as Playnite stores it                                                        |
| `igdb`     | Plain text                                                                         |
| `manual`   | Plain text, typed by the user in the web UI                                        |
| `unknown`  | Written before the host recorded provenance, which only Playnite did; treat as HTML |

`last_played` and `playtime_minutes` come from whatever launches the game, whatever `source` says.

When `has_background` is true, `GET /appbackground?appid=<id>` (or `appuuid=<uuid>`) returns the hero image as a PNG.
It answers 404 when the app has no background, so clients can simply try it.

## Play Statistics and Achievements
On Windows hosts with Playnite, clients can show how much each game is played and which achievements were unlocked.
Playnite itself keeps only totals (playtime, play count, last played), which the connector reports with the library.
The history of sessions is read from the files of Playnite's [GameActivity](https://github.com/Lacro59/playnite-gameactivity-plugin)
extension (`<ExtensionsData>\afbb1a0d-04a1-4d0c-9afa-c6e42ca855b4\GameActivity\<game id>.json`), and achievements from
those of [SuccessStory](https://github.com/Lacro59/playnite-successstory-plugin); the connector tells the host where
both live. The host never contacts Steam, Xbox or any other service for them. Without GameActivity (not installed,
or turned off with `playnite_gameactivity`) there is no history: `activity` is `false` and only Playnite's totals are
filled in; without SuccessStory (or with `playnite_successstory` off) there are no achievements. The endpoints share
`/appmetadata`'s permission check and catalogue: games the caller's `/applist` doesn't show are counted in totals but
never named. They answer 404 on other platforms, and before the host has heard from the connector.

All keys are snake_case. Dates are `YYYY-MM-DD` in the host's local time, and instants are UTC ISO 8601
(`2024-03-11T18:00:00Z`). A day of play runs from 5:00 to 5:00 local time, so a session from 23:00 to 1:00 counts for
the evening it started, and a session across 5:00 is split between the two days.

`GET /appstats?range=week|month|year&offset=0` describes a period. Weeks run Monday to Monday; `week` and `month` have
one bucket per day, `year` one per month (dated the first of the month). `offset` steps back from the current period,
from `0` down to `-500`. `to` is the day after the last one. While the current period is under way,
`previous_total_seconds` covers the previous period only up to the same point (the first 5 days of last month on the
5th of this one).

| Key                      | Meaning                                                                                 |
|:-------------------------|:----------------------------------------------------------------------------------------|
| `activity`               | `true` when GameActivity's data was found; when `false`, `tracking_since` is `null`, the totals and `sessions` are 0, `buckets` are all zero and `top` is empty |
| `range`, `offset`        | The period asked for (an unknown range is a week)                                       |
| `from`, `to`, `today`    | The period's first day, the day after its last, and today                               |
| `tracking_since`         | The day of GameActivity's first session, or `null` when it has none                     |
| `total_seconds`          | Played in the period, every game                                                        |
| `previous_total_seconds` | Played in the previous period, from `previous_from`                                     |
| `sessions`               | Sessions that started in the period                                                     |
| `buckets`                | `[{date, seconds}]`                                                                     |
| `top`                    | `[{uuid, name, seconds}]`: the caller's 5 most played games in the period               |
| `library`                | `{playtime_seconds, games, installed, installed_never_played, played_this_year}`, from Playnite's totals of every game that isn't hidden |
| `resume`                 | `[{uuid, name, playtime_seconds, last_activity}]`: up to 6 installed games with an hour or more, untouched for 90 days, longest-played first |
| `achievements`           | `null` without SuccessStory, else `{unlocked, recent}`: unlocks in the period, and the 8 latest as in `/appachievements/recent` |

`GET /appstats?appuuid=<uuid>` describes one game: `{uuid, activity, playtime_seconds, play_count, last_activity,
sessions, average_seconds, weeks, last_session, tracking_since}`. `weeks` holds 12 values, the current week last;
`last_session` is `{start, seconds}` or `null`. `playtime_seconds`, `play_count` and `last_activity` are Playnite's
and always filled in; with `activity` `false` the rest is empty (0, all-zero weeks, `null`).

A game being played counts up to now in both forms, from the moment Playnite reported it started. GameActivity may
already hold an item for that run (it adds one when the game starts and fills in its length when it stops); any item
of that game starting less than about 2 minutes before that moment, or after it, is that run, and the live count
replaces it so the run is never counted twice. Items of 0 seconds are skipped, and GameActivity's own "ignore short
sessions" setting is not applied: it only filters GameActivity's views.

`GET /appachievements?appuuid=<uuid>` lists one game's achievements as `{uuid, total, unlocked, last_refresh, items}`,
unlocked ones first, newest first, then the locked ones, the most common first. It answers 404 when SuccessStory has
nothing on the game or was told to ignore it. Each item is:

| Key            | Meaning                                                                                       |
|:---------------|:----------------------------------------------------------------------------------------------|
| `id`           | The source's own name for it, or its position when there is none                             |
| `name`, `description`, `hidden` | As the game describes it; hidden ones should stay masked until the user asks  |
| `unlocked`, `unlocked_at` | `unlocked_at` is `null` while locked, and also when the source doesn't say when    |
| `percent`      | Players who have it, 0-100, or `null` when unknown                                            |
| `gamer_score`  | Xbox gamerscore, or `null`                                                                    |
| `icon`         | An absolute web URL, a `/appachievementicon?...` path on this host, or `null`                 |
| `locked_icon`  | The same for the locked icon, or `null` to grey out `icon` instead                            |

`GET /appachievements/recent?since=<unix seconds>&limit=40` returns `{"achievements": [...]}`: the latest unlocks with
a known date at or after `since` (`0` for all), newest first, across the caller's games. Each entry is an item as above
plus the game's `uuid` and its `game` name. `limit` defaults to 40 and is capped at 200.

`GET /appachievementicon?appuuid=<uuid>&index=<n>[&locked=1]` returns the bytes of an icon SuccessStory keeps on disk;
the `icon` and `locked_icon` URLs above already point at it. Web icons are linked directly and answer 404 here.

## Limitations
Sunshine does have some limitations, as compared to Nvidia GameStream.

* Automatic game/application list.
* Changing game settings automatically to optimize streaming.

<div class="section_buttons">

| Previous                                        |              Next |
|:------------------------------------------------|------------------:|
| [Third-party Packages](third_party_packages.md) | [Legal](legal.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
