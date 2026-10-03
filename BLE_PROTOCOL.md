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
<ssid>\n<passphrase>[\n<deviceToken>]
```

UTF-8, newline separator (a newline appears in neither a WPA passphrase nor
a sane SSID). Constraints: ssid 1–32 chars, passphrase 8–63 chars (empty
passphrase = open network). The write is rejected by the stack unless the
link is encrypted, so pair first.

The **optional third field** is the companion-API device token (1–128 chars),
which enables standalone cloud state sync (see below). A legacy 2-field write
works unchanged and leaves any previously stored token untouched; a write
with a present-but-invalid token is rejected.

On receipt the device validates, persists the credentials (and token, if
present) in NVS (they override the compile-time `config.h` defaults from
then on), and immediately reconnects Wi-Fi. A status overlay on the screen
reports success/failure, and `PROV:` lines appear on the serial monitor.

**Auto-push on connect:** the app now writes the phone's current Wi-Fi
credentials (2-field payload) on *every* BLE connect, not just first-time
setup. To keep that cheap, the firmware compares the incoming ssid+pass
against the stored NVS values before touching the radio: if both match, the
write is a no-op — no `WiFi.disconnect()`, no ~10 s rejoin — and the screen
shows an "Already on \<ssid\>" overlay instead. A token field, if present,
is still persisted even when the credentials are unchanged, so token
rotation keeps working; a 2-field write never touches the stored device
token. If the credentials differ, the behavior is unchanged: persist
everything and reconnect.

> The 3-field payload can reach ~226 bytes — the firmware now requests MTU
> 256; the app must still complete the MTU exchange (automatic on iOS,
> `requestMTU(256)` on Android) or the write will truncate.

## Device -> App (read / notify)

Value is a UTF-8 string: `<stage>:<streak>:<mood>:<item>:<points>:<happy>[:<dcaCount>][:<subStage>]`

Example: `coinling:3:1:2:12500:87:0:5`

Sent on connect and whenever any field changes, plus when the day-based
streak rolls over at midnight. The optional 7th field is `dcaCount`
(device-owned). The optional 8th field is `subStage` (1-12), which lets
the hardware show the full 12-stage evolution line while still rendering
one of the four base creature forms.

> The string can exceed 20 bytes — the firmware requests MTU 256; the app
> must still complete the MTU exchange (automatic on iOS, `requestMTU(256)` on
> Android) or notifications will truncate at 20 bytes.

### `sync:req` (manual sync request)

When the user short-presses button 1 (action) on the pet page while the app
is connected, the device notifies the literal string `sync:req` instead of a
state snapshot (it never starts with a stage name, so old apps parse it as
garbage and ignore it). The app should respond by resending a full state
snapshot plus `dca:count:` / `dca:plan:` writes — the same payload it sends
on connect. While the app is disconnected, the same button press triggers
the standalone Wi-Fi sync (relay poll + price fetch) immediately instead of
waiting for the 30-minute cadence.

### `feed:req` (feed request, device → app)

Sent when the user activates **Feed pet** on the device's menu screen while
the app is connected. Same notify mechanism and old-app tolerance as
`sync:req`. The app is authoritative: it applies the feed (skipped for a
dead pet) and answers with `react:glow` immediately plus the resulting
`happy:` (debounced, only when happiness actually changed; points/streak do
not change on feed). The device plays a local jump reaction immediately so
it feels alive even before the answer lands; with no app connected it only
shows a "connect the app" toast.

### `dca:req` (open DCA request, device → app)

Sent when the user activates **Open DCA** on the device's menu screen while
the app is connected. The app should open its DCA wizard/sheet. With no app
connected the device only shows a "connect the app" toast.

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
The firmware requests MTU 256 on connect (automatic on iOS; call
`requestMTU(256)` on Android). If no MTU exchange happened, keep every
write ≤ 20 bytes — in particular send `points:` / `happy:` as their own
writes instead of batching them into the connect snapshot, or they will be
truncated away.

| Command | Effect |
|---|---|
| `stage:egg` / `coinling` / `hodler` / `whale` | Morph to stage (easeOutQuint) |
| `stage:<1-12>` | Numeric stage; maps to base shape (1 egg, 2-7 coinling, 8-9 hodler, 10-12 whale) |
| `substage:<1-12>` | Set the visible 12-stage name badge without changing the base shape |
| `mood:<0-5>` | Blend to emotion (0.45 s ease) |
| `item:<0-5>` | Equip collectible (instant) |
| `bg:<name>` | Set the scene backdrop theme: `default` / `aurora` / `sunset` / `midnight` / `galaxy` / `gold` (mirrors the app's background picker). Optional — old apps never send it, unknown names are ignored; not persisted (the app is authoritative and can re-push on connect) |
| `look:<yaw>,<pitch>` | Steer gaze, degrees (yaw ±30, pitch ±25) |
| `look:off` | Resume idle gaze wander |
| `react:jump` / `spin` / `glow` / `dance` | Play a reaction animation |
| `points:<n>` | Set points (persisted in NVS, shown in the stats bar) |
| `happy:<0-100>` | Set happiness (persisted in NVS, shown in the stats bar) |
| `streak:<n>` | Set displayed streak (persisted in NVS; also stamps the day so the offline day check keeps it) |
| `<stage>:<streak>:<mood>:<item>:<points>:<happy>[:<dcaCount>][:<subStage>]` | Full state snapshot (same shape as the notify string) — sets everything at once; the optional trailing fields update the plan count and 12-stage sub-stage |
| `dca:count:<n>` | Declares that `n` (0–4) `dca:plan:` writes follow; all previous plan slots are wiped from NVS first |
| `dca:plan:<i>:<enabled>:<next_buy_epoch>:<amount>:<TICKER>:<buys>:<holdings>[:<price_usd>]` | Write plan slot `i` (0–3): enabled 0/1, next-buy unix epoch, amount in SOL (float), ticker (clamped to 6 chars), completed buys, holdings held. The optional 8th field is the token's unit price in USD (drives the price/valuation display). Persisted in NVS, shown on the portfolio screen and the next-buy chip |
| `dca:clear` | Wipe all plan slots (RAM + NVS) |
| `dca:hit:<n>:<TICKER>` | A buy just executed: "+n TICKER" reward toast (app purple) + dance reaction + sparkle burst |
| `solusd:<rate>` | SOL/USD rate (float) for the portfolio amount unit toggle (USD ⇄ SOL); persisted in NVS (`finagotchi`/`solUsd`) |
| `epoch:<sec>` | Fallback wall-clock set (unix seconds). Only applied if NTP has never synced; ignored afterwards |

Writes may use write-with-response or write-without-response — the
characteristic exposes both properties. Writes with an unrecognized payload
are logged as `BLE: unknown command` on the serial monitor.

## DCA plan tracking

The device mirrors up to **4 DCA plans** from the app, persisted in NVS
(`finagotchi` namespace: `dcaCount` + `dca0`..`dca3` blobs) so they survive
reboots. When plans exist, a next-buy chip on the pet screen rotates one
plan every 4 s (cross-fade blend):

```
TICKER  $10.00  in 2d 14h
```

If a plan is past its epoch with no new buys it is marked **overdue**:
amber (warning) border + "overdue" text. If the wall clock was never
synced, the countdown shows `--` instead of garbage. Button 2 double-press
advances the chip to the next plan.

Button 2 short-press opens the full-screen **portfolio view**: a header
with the total portfolio value (Σ holdings × price in USD, with a SOL
equivalent under it when the `solusd:` rate is known), then one card per
plan — the actual token logo (official xStocks icons embedded at build
time — see `tools/convert_token_logos.py`; unknown tickers get a
procedural monogram chip), ticker, position value, next-buy countdown and
holdings/buy info. Button 2 walks the card focus (cyan ring); after the
last card it moves on to the **menu screen**, and from there back to the
pet screen (pet → portfolio → menu → pet). Button 1 opens a per-token
**detail view** and closes it again; button 1 double-press toggles amounts
between USD and SOL (needs the `solusd:` rate). Overdue cards are amber.
While offline, the device fetches prices itself over HTTPS (Jupiter Price
API + xStocks `price-data`, SOL/USD included) on the poll cadence. This
view is local UI only — it never leaves the device.

The **menu screen** mirrors the app's bottom action bar: a horizontal row
of round icon buttons — **Accessory** (cycles the collectible locally,
mirrored in the notify snapshot), **Mood** (cycles the emotion locally,
mirrored likewise), **Feed pet** (primary cyan button, `feed:req`) and
**Open DCA** (`dca:req`) — with a caption naming the focused action above
the row. Button 2 walks the focus left-to-right, button 1 runs the focused
action. On any screen, a long press of either button returns straight to
the pet screen; every screen's bottom hint bar shows the current actions
including that escape.

The notify/read snapshot gains an **optional 7th field** — the number of
active plan slots — and an **optional 8th field** — the 12-stage
sub-stage index:

```
<stage>:<streak>:<mood>:<item>:<points>:<happy>[:<dcaCount>][:<subStage>]
```

New firmware tolerates either optional field being absent; old firmware
ignores extra fields. The count is device-owned — the app may echo it back
in a snapshot write, where it is parsed but ignored. The sub-stage index
(1-12) controls the on-screen evolution badge and is independent of the
four base creature shapes.

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

### Cloud state sync (provisioned devices)

Devices provisioned with a **device token** (3rd provisioning field) poll the
companion API for authoritative pet state while no app is connected:

```
GET https://api.finagotchi.app/device/state
Authorization: Bearer <deviceToken>

-> {"stage":4,"sub":4,"streak":7,"mood":"happy","points":120,"happy":80,"upd":1726000000}
```

- Poll cadence **60 s** (first poll ~20 s after boot); paused while the app
  is connected, like the DCA poll. If Wi-Fi dropped, the task silently
  rejoins with the stored credentials and retries next cycle.
- On HTTP 200 the fields are applied through the same code path as a BLE
  snapshot write (`stage` 1–12 maps to the base shapes per the table above,
  `sub` sets the 12-stage badge directly, `mood` is one of the mood names).
  Identical polls are skipped — no NVS writes when nothing changed. `item`
  is not part of the cloud snapshot and stays device-owned.
- On HTTP 401 the token is assumed bad: after **5 consecutive 401s** syncing
  stops until reboot (avoids hammering the API).
- The first successful apply sets the persisted `serverLinked` flag
  (`fina`/`srvlink` in NVS). While linked, the demo auto-evolve and the
  local day-based streak tick stay off — the server owns stage/streak.
  Devices without a token (or that never synced) keep the demo behavior
  exactly as before.

### Gain toasts

`dca:hit:` (connected) and offline buy detection (standalone) spawn a gain
toast: "+n TICKER" centered on screen, floating up ~40 px with ease-out over
1.2 s, holding 0.6 s, then fading out over 0.7 s (LVGL alpha). Buy hits use
the app's reward purple (#9945FF), poll-detected offline buys and status
messages the success green (#5DE2A6).


## On-screen stats row

The bottom of the pet screen shows a stats row: streak days, points
(k-suffix over 10k), and happiness as a small bar (the app's HappinessBar
pattern). Above it, a slim stage track fills cyan toward the 12-stage cap
(PetCanvas stageTrack). Pet is scaled to R=88 and centered slightly above
middle to make room. A hint bar above the stats shows what the two buttons
do on the current screen.

The day-based streak check is paused while connected (the app is
authoritative); it resumes on disconnect for offline mode.

## Waiting-for-sync scene

While the device advertises (no app connected), the pet blends to the
`waiting` mood (0.45 s, like any `mood:` write) and a spinner with a
"waiting for connection" caption plays at the top of the screen (LVGL).
On connect the spinner disappears and the pet blends back to its previous
mood; on disconnect the scene returns.

## Firmware rendering notes (what the device reproduces)

- Scene backdrop: two-zone sky/ground, tinted per the app's
  `BACKGROUND_COLORS` themes (exact rgba-over-#07111F composites), and the
  PetCanvas ground shadow — a dark ellipse under the creature that mirrors
  the idle float (tightens/fades as the pet lifts) and squashes with the
  reaction scale
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
On disconnect, gaze is cleared and the demo resumes. Once the device is
server-linked (successful cloud state sync), the demo auto-evolve and the
local streak tick stay off even while disconnected — the server is then
authoritative for stage/streak.
