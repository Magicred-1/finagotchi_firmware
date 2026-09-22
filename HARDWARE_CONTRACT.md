# Finagotchi Hardware Contract (BLE + Relay)

**Status: FROZEN.** The ESP32-S3 firmware is built to this document. Any change
here is a firmware breaking change — do not edit formats, only append new
commands with new prefixes.

This document is shared byte-for-byte with the firmware repo. All payloads are
UTF-8 strings with no trailing newline unless stated otherwise.

## 1. GATT layout

| Item | UUID | Properties |
| --- | --- | --- |
| Service | `0000f1a0-0000-1000-8000-00805f9b34fb` | — |
| State characteristic | `0000f1a1-0000-1000-8000-00805f9b34fb` | READ + NOTIFY + WRITE |
| Provisioning characteristic | `0000f1a2-0000-1000-8000-00805f9b34fb` | WRITE (encrypted writes only) |

- Advertised device name: `Finagotchi`.
- MTU: **256** (app calls `requestMTU(256)` on Android after connect; iOS
  negotiates automatically). If negotiation fails the app falls back to the
  default 20-byte ATT payload and splits writes (see §3, snapshot fallback).
  Raised from 128 in the 2026-09 cloud-sync revision — see §2.

## 2. Pairing

During pairing the device displays a **6-digit passkey on its own screen
(display-only)**. The user types that code into the app. The app uses it to
gate the Wi-Fi provisioning write; it is never sent back over BLE.

Wi-Fi provisioning (app → device, characteristic `0xf1a2`, encrypted write
only — requires an encrypted/bonded link):

```
<ssid>\n<pass>
```

Exactly one `\n` separator, no trailing newline.

**Revision 2026-09 (cloud sync):** an optional third field carries the device
token that lets the hardware pull pet state directly from the API server:

```
<ssid>\n<pass>\n<deviceToken>
```

Two-field writes remain valid and simply leave Wi-Fi-only provisioning in
place. Because the token makes the payload up to ~226 bytes, the MTU is now
**256** (was 128): the app calls `requestMTU(256)` on Android and firmware
requests 256 as well. Devices running older firmware negotiate 128/185 and
only support the two-field payload.

With a token provisioned, the device polls
`GET https://api.finagotchi.app/device/state` (Bearer token) every 60 s while
no app is connected, and applies `{stage, sub, streak, mood, points, happy}`
from the server. While an app is connected, the app remains authoritative and
the device pauses cloud polling.

## 3. State characteristic commands (app → device, `0xf1a1` writes)

### 3.1 Pet snapshot

```
<stage>:<streak>:<mood>:<item>:<points>:<happy>
```

| Field | Type | Notes |
| --- | --- | --- |
| `stage` | string | `egg` \| `coinling` \| `hodler` \| `whale` |
| `streak` | uint | streak days |
| `mood` | uint | 0–5 |
| `item` | uint | 0–6 (0 none, 1 crown, 2 glasses, 3 bowtie, 4 halo, 5 diamond, 6 tshirt) |
| `points` | uint | integer points balance |
| `happy` | uint | 0–100 |

Sent on every connect. If the full snapshot does not fit the negotiated
payload (i.e. MTU negotiation failed, 20-byte ATT payload), the app MUST NOT
truncate it. Instead it writes three commands, in order:

```
<stage>:<streak>:<mood>:<item>
points:<p>
happy:<h>
```

`points:<p>` and `happy:<h>` are the same key-value commands the app uses for
incremental stat pushes; the firmware already parses them. A truncated stats
value is never acceptable.

The device also **notifies this same string** on `0xf1a1` when its own state
changes; the app reconciles (the app is authoritative while connected).

### 3.2 Clock sync

```
epoch:<sec>
```

Current unix time in **uint32 seconds** (the hardware has no RTC). Sent on
every connect, immediately after the snapshot.

### 3.3 DCA plan push

First the plan count, then one line per plan:

```
dca:count:<n>
dca:plan:<i>:<enabled>:<next_buy_epoch>:<amount>:<TICKER>:<buys>:<holdings>
```

| Field | Type | Notes |
| --- | --- | --- |
| `n` | uint | number of active plans |
| `i` | uint | plan index, 0-based, contiguous |
| `enabled` | uint | 1 = active, 0 = paused |
| `next_buy_epoch` | uint32 | unix seconds of the next scheduled buy |
| `amount` | float | USDC per tick, plain decimal (e.g. `0.25`), never scientific notation |
| `TICKER` | string | ≤ 6 chars, uppercase A–Z/0–9 only (e.g. `SPYX`) |
| `buys` | uint | fills executed so far |
| `holdings` | float | output tokens held, plain decimal |

Rules:

- A plan push is always a **full rewrite**: the device wipes its plan table on
  `dca:count` and reloads from the `dca:plan` lines. Plan edits always end
  with a full `dca:count` + `dca:plan` sequence.
- The app diffs a hash of the rendered plan lines against the last
  acknowledged hash stored in app storage and skips the push when unchanged.
- Validation happens **before every BLE write**: ticker ≤ 6 uppercase chars,
  epochs are uint32 seconds, amounts are plain floats.

### 3.4 Fill toast

```
dca:hit:<n>:<TICKER>
```

`n` is the plan's total `buys` count after the fill (uint), `TICKER` as above.

- Fills that happen while the device is **disconnected** are queued by the app
  and replayed on reconnect — exactly one `dca:hit` per missed fill, spaced
  **800 ms** apart (the device toast queue is 3 deep).
- **`buys` increment is the ONLY toast trigger** (BLE `dca:hit`). Nothing else
  may pop a toast: not `next_buy_epoch` changes, not amount edits, not
  reconnect itself.

## 4. Standalone operation

While the app is away, the hardware operates standalone (the firmware owns its
own data path on that side — there is no app-hosted relay). The app sends
nothing on disconnect and displays "device offline, last synced Xm ago".

## 5. Transport notes

- All BLE writes to `0xf1a1` are serialized by the app: one write completes
  before the next starts.
- On disconnect the app sends nothing further (see §4).
- Android 12+ runtime permissions `BLUETOOTH_SCAN` / `BLUETOOTH_CONNECT` are
  requested with `neverForLocation`, inside the HardwareBinding flow — never
  cold at app start.
