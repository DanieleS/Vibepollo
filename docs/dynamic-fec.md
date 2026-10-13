# Dynamic video FEC

By default the host protects every video frame with a fixed share of Reed-Solomon
parity, `fec_percentage` (20%). With dynamic FEC, a client that supports it reports its
packet loss once a second. The host then picks the lowest parity that keeps the
chance of losing a frame under 0.1% and takes that parity out of the session's
bitrate budget. On a clean link the encoder gets almost all of the bitrate. When
the link starts losing packets, protection rises within about a second.

The host side is ported from
[AlkaidLab/foundation-sunshine](https://github.com/AlkaidLab/foundation-sunshine)
(`src/streaming/fec.cpp`, commits 6470fbed and 46525886). The client side is in
DanieleS/moonlight-common-c and is based on qiin2333/moonlight-common-c 8e6a4a72.
The wire protocol matches both, so these clients and hosts work with each other.

## Negotiation

Nothing changes unless both sides opt in:

1. The host adds `a=x-ss-video[0].fecControlVersion:1` to every RTSP DESCRIBE.
   Clients that don't know the attribute ignore it.
2. A client that supports version 1, uses the encrypted control stream
   (`x-nv-general.useReliableUdp:13`) and is not streaming PyroWave may add two
   attributes to its ANNOUNCE:

   | Attribute | Value |
   |---|---|
   | `x-ml-video.fecSummaryVersion` | `1` |
   | `x-ml-video.fecPercentage` | `-2` host default, `-1` automatic, `0`-`100` fixed |

3. The host creates a per-session controller only for `-1` or `0`-`100`. If the
   attributes are missing, the value is `-2` or invalid, or the session is PyroWave,
   the session uses the fixed `fec_percentage` path. That path is unchanged:
   same bitrate arithmetic, same parity, same packets.

## Loss summary (control message `0x5510`)

Once a second the client sends three big-endian `uint32` fields (12 bytes) on the
encrypted control stream, unreliably:

| Field | Meaning |
|---|---|
| `sequence` | Increments once per summary period, including periods that sent nothing |
| `dataPackets` | Data shards in the FEC blocks the client finished, gave up on or lost whole during the period |
| `missingDataPackets` | Of those, shards missing before FEC recovery |

Each block counts once, including blocks that arrived complete. Blocks and frames
that never arrived at all count too: the client sees the jump in RTP sequence
numbers and counts every skipped packet as a missing data shard (parity included,
which slightly overstates loss). The host rejects a summary that is empty,
inconsistent, duplicated or older than the last one. A gap in `sequence`, or more
than 3 s without a summary, clears the host's loss history.

`0x5510` is not used by Moonlight, Sunshine, Apollo, Artemis or Vibepollo
(Sunshine's extensions currently end at `0x5504`).

## Choosing the percentage

- **Fixed** (`0`-`100`): the client's value for every frame.
- **Automatic** (`-1`): the host keeps ten one-second buckets of summaries and
  takes the worst second's loss rate. For each frame it chooses the lowest
  percentage, up to 50%, for which the frame's FEC blocks (laid out as the send
  path will lay them out, including the client's minimum parity) all survive
  independent losses at that rate with a combined failure chance of at most 0.1%. If
  50% is not enough, the frame gets the highest percentage that still fits the FEC
  blocks. The host uses `fec_percentage` when there is no feedback and when feedback
  is older than 3 s. Reported loss raises protection at once, but dropping to 0%
  needs 10 clean seconds of summaries, both at the start of a session and after a gap;
  until then the host keeps `fec_percentage`.

The model assumes independent losses. Bursty Wi-Fi loss can be worse than it predicts.

## Bitrate budget

For a dynamic session the host computes the video budget the same way as before:
the client's configured bitrate (capped by `max_bitrate`), minus the audio allowance
(at most 20%) and 500 kbps of overhead (at most 10%). It does not take the
`fec_percentage` share out of that. The encoder runs at
`budget * 100 / (100 + F)`, so data plus parity fit within the budget.

At most once a second, the host turns the peak percentage requested during that
second into a new encoder bitrate. Changes of one percentage point are ignored,
but moving to or from 0% always goes through. The encoder thread computes the
bitrate from the current budget when it applies the change. The change is applied only if the
encoder can reconfigure in place without a keyframe (NVENC with its rate-control
reset turned off for this purpose, or AMF). An encoder that would have to be
rebuilt keeps its starting bitrate. A change the encoder refuses is requested
again a second later. A frame never carries more parity than the
encoder's current bitrate leaves room for. To raise protection, the host first
lowers the encoder bitrate and adds parity to frames sent after the encoder confirms
the change. Frames already encoded at the old bitrate can still be in flight and get
the new parity, so the second after a rise can briefly exceed the budget by that
difference. NVENC also moves to a new bitrate gradually rather than at once. To lower
protection, the host removes parity at once and raises the encoder bitrate afterwards.

A runtime `/bitrate` request on a dynamic session sets the video budget, so parity
comes out of it. Like on other sessions, the value is used as given: audio and
overhead are not taken out of it, unlike the bitrate a client asks for at session
start. On other sessions it sets the encoder bitrate, as before.

## Logs

- `Client negotiated dynamic FEC: automatic, video budget N Kbps` at session start.
- `Dynamic FEC: F% for L% loss; encoder to E of B kbps` when the split changes.
- `Encoder refused an in-place bitrate change; dynamic FEC stays within its current budget`,
  once per encoder session, if the encoder cannot follow.
