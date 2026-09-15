# Finagotchi BLE Protocol

Device advertises as **`Finagotchi`**.

- Service: `0000f1a0-0000-1000-8000-00805f9b34fb`
- Characteristic: `0000f1a1-0000-1000-8000-00805f9b34fb` (READ + NOTIFY + WRITE + WRITE_NR — both write-with-response and write-without-response are accepted)
- Provisioning characteristic: `0000f1a2-0000-1000-8000-00805f9b34fb` (WRITE + WRITE_NR, **encrypted writes only** — see below)

## Pairing (required before Wi-Fi provisioning)

The device uses BLE Secure Connections with bonding. It has a screen, so it
acts as "display only" (`ESP_IO_CAP_OUT`): when the app initiates pairing,
the device shows a 6-digit passkey on its screen and the app must perform
**passkey entry**. After bonding, the link is encrypted.

The pet/state characteristic stays open (stats are not sensitive); only the
provisioning characteristic enforces encryption.

## Wi-Fi provisioning (first-time setup)

Write to the provisioning characteristic:

```
<ssid>\n<passphrase>
```

UTF-8, newline separator (a newline appears in neither a WPA passphrase nor
a sane SSID). Constraints: ssid 1–32 chars, passphrase 8–63 chars (empty
passphrase = open network). The write is rejected by the stack unless the
link is encrypted, so pair first.

On receipt the device validates, persists the credentials in NVS (they
override the compile-time `config.h` defaults from then on), and immediately
reconnects Wi-Fi. A status overlay on the screen reports success/failure,
and `PROV:` lines appear on the serial monitor.

## Device -> App (read / notify)

Value is a UTF-8 string: `<stage>:<streak>:<mood>:<item>:<points>:<happy>`

Example: `coinling:3:1:2:12500:87`

Sent on connect and whenever any field changes, plus when the day-based
streak rolls over at midnight.

> The string can exceed 20 bytes — the firmware requests MTU 128; the app
> must still complete the MTU exchange (automatic on iOS, `requestMTU` on
> Android) or notifications will truncate at 20 bytes.

### Field ids

**stage**: `egg` | `coinling` | `hodler` | `whale`

**mood** (emotion — order matches `expressions.ts`):

| id | mood |
|---|---|
| 0 | calm |
| 1 | happy |
| 2 | excited |
| 3 | waiting |
| 4 | sleepy |
| 5 | sad |

**item** (collectible — matches `PetAccessory.tsx`):

| id | item |
|---|---|
| 0 | none |
| 1 | crown |
| 2 | glasses |
| 3 | bowtie |
| 4 | halo |
| 5 | diamond |
| 6 | tshirt |

Unknown item ids degrade to `none` (0) rather than misrendering.

## App -> Device (write)

Write one command per write, or several separated by `;`.
The firmware requests MTU 128 on connect (automatic on iOS; call
`requestMTU(128)` on Android). If no MTU exchange happened, keep every
write ≤ 20 bytes — in particular send `points:` / `happy:` as their own
writes instead of batching them into the connect snapshot, or they will be
truncated away.

| Command | Effect |
|---|---|
| `stage:egg` / `coinling` / `hodler` / `whale` | Morph to stage (easeOutQuint) |
| `stage:<1-5>` | Numeric stage (app store mapping: 1 egg, 2/3 coinling, 4 hodler, 5 whale) |
| `mood:<0-5>` | Blend to emotion (0.45 s ease) |
| `item:<0-5>` | Equip collectible (instant) |
| `look:<yaw>,<pitch>` | Steer gaze, degrees (yaw ±30, pitch ±25) |
| `look:off` | Resume idle gaze wander |
| `react:jump` / `spin` / `glow` / `dance` | Play a reaction animation |
| `points:<n>` | Set points (persisted in NVS, shown in the stats bar) |
| `happy:<0-100>` | Set happiness (persisted in NVS, shown in the stats bar) |
| `streak:<n>` | Set displayed streak (persisted in NVS; also stamps the day so the offline day check keeps it) |
| `<stage>:<streak>:<mood>:<item>:<points>:<happy>` | Full state snapshot (same shape as the notify string) — sets everything at once |
| `dca:count:<n>` | Declares that `n` (0–4) `dca:plan:` writes follow; all previous plan slots are wiped from NVS first |
| `dca:plan:<i>:<enabled>:<next_buy_epoch>:<amount>:<TICKER>:<buys>:<holdings>[:<price_usd>]` | Write plan slot `i` (0–3): enabled 0/1, next-buy unix epoch, amount in SOL (float), ticker (clamped to 6 chars), completed buys, holdings held. The optional 8th field is the token's unit price in USD (drives the price/valuation display). Persisted in NVS, shown in the carousel |
| `dca:clear` | Wipe all plan slots (RAM + NVS) |
| `dca:hit:<n>:<TICKER>` | A buy just executed: "+n TICKER" gain toast + dance reaction + sparkle burst |
| `solusd:<rate>` | SOL/USD rate (float) for the positions-page amount toggle; persisted in NVS (`finagotchi`/`solUsd`) |

Writes may use write-with-response or write-without-response — the
characteristic exposes both properties. Writes with an unrecognized payload
are logged as `BLE: unknown command` on the serial monitor.

## DCA plan tracking

The device mirrors up to **4 DCA plans** from the app, persisted in NVS
(`finagotchi` namespace: `dcaCount` + `dca0`..`dca3` blobs) so they survive
reboots. When plans exist, a carousel line rotates one plan every 4 s in the
stats-bar area (0.45 s slide/fade blend, like the mood blends):

```
TICKER  0.25 SOL  in 2d 14h
```

with a small progress ring on the left filling toward `next_buy_epoch`. If a
plan is past its epoch with no new buys it is marked **overdue**: amber ring
+ "overdue" text. If the wall clock was never synced, the countdown shows
`--` instead of garbage. Button 2 long-press pins a plan (30 s), a
double-press resumes auto-rotate.

A second, full-screen **DCA positions page** shows one plan per card: the
actual token logo (official xStocks icons embedded at build time — see
`tools/convert_token_logos.py`; unknown tickers get a procedural monogram
chip), the token's USD unit price in a large font, a stats grid (buy amount
alternating SOL/USD every 3 s — USD needs the `solusd:` rate — buys, held,
held value), and the next-buy countdown with a progress bar. Button 1
(action) pages through plans with a 0.45 s slide transition; overdue cards
pulse amber. While offline, the device fetches prices itself over HTTPS
(xStocks `price-data` + CoinGecko SOL/USD) on the poll cadence. This page is
local UI only — it never leaves the device. Navigation: button 2 (navigate)
short-press switches pages.

The notify/read snapshot gains an **optional 7th field** — the number of
active plan slots:

```
<stage>:<streak>:<mood>:<item>:<points>:<happy>[:<dcaCount>]
```

New firmware tolerates its absence (6-field writes still parse); old
firmware ignores the extra field. The count is device-owned — the app may
echo it back in a snapshot write, where it is parsed but ignored.

### Standalone mode (app disconnected)

While no app is connected, the device polls the read-only relay every
30 min over Wi-Fi (see `README.md` for the endpoint contract) and matches
CSV rows to cached plans by ticker:

- remote `buys` > cached `buys` → "+<delta×amount> TICKER" gain toast
  (delta capped at 9) and the cache updates
- a plan past `next_buy_epoch` with unchanged buys → slot marked overdue
  (amber ring); ≥1 overdue plan nudges the mood toward `waiting`, ≥2 toward
  `sad`, reverting when resolved
- after every successful poll the wall clock is re-synced via NTP

On connect the app becomes authoritative again: polls pause and the app may
resend `dca:count:` / `dca:plan:` plus any missed `dca:hit:` events. The
device never calls Titan or any signing API — the relay is read-only and the
URL/device id are compile-time `config.h` defines.

### Gain toasts

`dca:hit:` (connected) and offline buy detection (standalone) spawn a gain
toast: "+n TICKER" in mint cyan (120,255,214), font 2, centered, spawning at
the stats bar, floating up ~40 px with easeOutCubic over 1.2 s, holding
0.6 s, then fading 0.7 s by lerping the text color toward the scene navy
`#07111F` (no per-glyph alpha in TFT_eSPI). Queue holds 3; a 4th replaces
the oldest.


## On-screen stats bar

The bottom of the TFT shows a stats bar: flame + streak days, sparkle +
points (k-suffix over 10k), heart + happiness. Pet is scaled to R=88 and
centered slightly above middle to make room.

The stats bar is **only shown while the app is connected** — the stats are
app-driven, so while disconnected the device shows the waiting-for-sync
scene instead. The day-based streak check is also paused while connected
(the app is authoritative); it resumes on disconnect for offline mode.

## Waiting-for-sync scene

While the device advertises (no app connected), it shows a waiting-for-sync
scene in place of the stats bar: the pet blends to the `waiting` mood
(0.45 s, like any `mood:` write) and an orbit animation plays around it —
two cyan comets circling the pet with fading dotted tails (3 s revolution,
half a revolution apart) over a slowly breathing orbit ring. A caption sits
at the bottom: "waiting for connection…" (cycling ellipsis) with an "open
the Finagotchi app" subtitle. On connect the scene disappears (the stats
bar returns) and the pet blends back to its previous mood; on disconnect
the scene returns.

## Firmware rendering notes (what the device reproduces)

- Radial bodies from `profiles.ts` (64 samples, same-angle morph lerp)
- Sphere-projected eyes from `face.ts` (`eyePoses` tangent basis, real 3D
  foreshortening, depth fade approximated by blending toward the body color)
- Seeded blink schedule (`createRng(0x5eed)`, incl. double blinks) and
  `loopNoise` liveliness — the device blinks in sync with the app
- Expressions override gaze/split/eyes, blended over 0.45 s
- Glow = body x1.15 at 22% (PetBody), eyes #f5f5f5 (PetEyes)
- Reactions as keyframe tracks; sparkle burst on stage-up (LevelUpAnimation)
- Idle float from PetCanvas (4 s loop)

While an app is connected, the on-device demo auto-evolve and the day-based
streak check are paused so the app has full control of the stage and stats.
On disconnect, gaze is cleared and the demo resumes.
