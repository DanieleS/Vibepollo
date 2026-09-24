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
