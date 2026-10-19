# Finagotchi Firmware OTA Spec

Over-the-air firmware updates over Wi-Fi. Status: **spec, not implemented**.
Target: land together with (or right after) the `pcb-v1` PlatformIO
environment, since both require touching the partition table.

## Current state

- The device already speaks HTTPS (`WiFiClientSecure` + `HTTPClient`,
  `src/main.cpp:39-41`) to `https://api.finagotchi.app/device/state`
  (`STATE_SYNC_URL`, Bearer device token, 60 s poll while no app is
  connected). OTA reuses this transport — it is one more authenticated GET
  whose body streams into the ESP32 `Update` class.
- Storage is `Preferences` (NVS) only; token logos and the logo are compiled
  in (`token_logos.h`, `logo.h`). There is **no filesystem** to preserve
  across a partition-table change.
- `platformio.ini:11` uses `board_build.partitions = huge_app.csv` — a
  single ~3 MB app slot with **no OTA partition**. This must change.
- There is no firmware version constant anywhere. OTA needs one.

## 1. Versioning

Add `FW_VERSION "x.y.z"` (semver) to a new `src/version.h`, plus
`FW_BUILD` (git short hash, injected by PlatformIO build flags).

- Shown on the device: menu → About row.
- Reported to the app: appended to the existing BLE state snapshot
  (see `BLE_PROTOCOL.md`) as a new trailing field, tolerated-absent by old
  apps the same way the 8th `dca:plan` field is.
- Sent to the API as a query param on the manifest request
  (`?v=1.2.0&env=pcb-v1`) so the server decides "newer available" and can
  ship per-hardware builds.

## 2. Partition layout

Replace `huge_app.csv` with a custom `partitions_8mb_ota.csv`:

```
nvs,      data, nvs,     0x9000,   0x6000
phy_init, data, phy,     0xf000,   0x1000
ota_0,    app,  ota_0,   0x10000,  0x380000   # 3.5 MB
ota_1,    app,  ota_1,   0x390000, 0x380000   # 3.5 MB
otadata,  data, ota,     0x710000, 0x2000
```

(Current firmware.bin is ~1.6 MB with LVGL; 3.5 MB per slot is comfortable
headroom. NVS offsets stay standard so pet state survives.)

**One-time migration:** devices flashed with `huge_app` cannot OTA
themselves into a dual-slot layout — the first OTA-capable flash goes over
USB. Every flash after that is wireless. Note this in the release notes of
the first OTA-capable version.

## 3. Server side (finagotchi_api_server)

New route file `src/routes/firmware.ts`:

- `GET /firmware/latest?env=<board>&v=<current>` → manifest:

```json
{
  "version": "1.4.0",
  "url": "https://api.finagotchi.app/firmware/files/pcb-v1/1.4.0/firmware.bin",
  "sha256": "…",
  "size": 1683456,
  "notes": "Crown encoder support"
}
```

  Returns `204 No Content` when the device is current. No auth needed for
  the manifest (same policy as public price data); the binary itself is
  integrity-protected by the SHA-256, not by transport secrecy.
- Binary hosting: GitHub Release assets with the API route redirecting
  (simplest — CI already targets GitHub), or any static bucket. The API
  stays the single source of truth for "what is latest".

## 4. Device update flow

Trigger (either):
- On-device: menu row "Check for updates" (crown scroll, push to select,
  cancel to abort — the new input scheme).
- From the app: new BLE write `ota:start` and notify `ota:status:<state>`
  on the state characteristic, string commands consistent with
  `BLE_PROTOCOL.md` style. App shows "update available" by comparing the
  snapshot's version field against the manifest.

Preconditions, checked before anything downloads:
- USB power connected **or** battery ≥ 40% (a brownout mid-flash is what
  the second slot exists for, but don't invite it).
- Wi-Fi joined; no BLE session mid-sync.

Flow:
1. GET manifest (auth optional). If 204 → "Up to date" toast, done.
2. Confirm screen: version + size, "Rotate = no, push = update" style.
3. Suspend the LVGL UI task; draw a single static progress screen
   ("Updating 42% — keep on charger"). TLS handshake + download buffer
   (~16 KB) must not compete with LVGL heap; this is mandatory on the N8
   breadboard build and merely comfortable on the N8R2/PSRAM board.
4. `Update.begin(size)` → stream the binary in 4–8 KB chunks →
   `Update.writeStream()`; feed the watchdog each chunk; verify SHA-256 of
   what was written against the manifest before `Update.end(true)`.
5. On success: reboot into ota_1. On any failure: `Update.abort()`, back
   to the menu with an error row — the running app is untouched.

## 5. Boot safety (two tiers)

- **Tier 1 (free, built into `Update`):** the boot partition only flips to
  the new slot after the full image is written and its internal checksum
  verifies. An interrupted or corrupted download can never boot — the old
  app stays selected. This covers network drops and brownouts.
- **Tier 2 (rollback, needs bootloader support):** a *valid but broken*
  image (boots, then crashes) is only auto-recovered if the bootloader has
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. The stock Arduino core
  bootloader does not ship with it; getting it means building Arduino as an
  ESP-IDF component or a custom bootloader. If we take this on, the pattern
  is: first boot of a new image must call
  `esp_ota_mark_app_valid_cancel_rollback()` only after a self-check passes
  (Wi-Fi joins + LVGL ready within N seconds), else the bootloader reverts
  to the previous slot on the next reset.
- Decision: ship Tier 1 first; Tier 2 is a follow-up once OTA is proven.

## 6. Security

- Manifest and binary over HTTPS with **real CA validation** — the pattern
  already exists: `httpGetJson(..., kApiRootCa)` validates against the
  pinned ISRG Root X1 in `src/api_root_ca.h` (used by the token-carrying
  state sync; the anonymous price feeds keep the unverified context).
  Firmware downloads must use the pinned-CA path, never the bare
  `setInsecure()` one.
- SHA-256 from the manifest checked against the written image (§4 step 4).
- Later option: ECDSA image signatures (`CONFIG_SECURE_SIGNED_APPS`) —
  out of scope for v1.

## 7. CI / release flow

GitHub Actions, on tag `fw-v*`:
1. `pio run -e esp32-s3 -e esp32-s3-sim -e pcb-v1`
2. Rename outputs `finagotchi-<env>-<version>.bin`, compute SHA-256
3. Create the GitHub Release with the binaries attached
4. Update the API's manifest data (commit to the API repo or push to the
   hosting bucket) — server starts returning the new version

Channels (stable/beta) can mirror the Expo channel discipline later; v1 is
a single stable channel.

## 8. Work breakdown & estimate

| # | Item | Where | Effort |
|---|------|-------|--------|
| 1 | `src/version.h` + build flags + About row + BLE snapshot field | firmware | ½ day |
| 2 | `partitions_8mb_ota.csv`, one-time USB reflash of dev unit | firmware | ½ day |
| 3 | `src/routes/firmware.ts` manifest + binary hosting | API server | ½ day |
| 4 | Update flow: manifest check, download, progress screen, guards | firmware | 1 day |
| 5 | `ota:start` / `ota:status` BLE commands + app UI | both | ½ day |
| 6 | GitHub Actions release workflow | firmware repo | ½ day |
| 7 | Tier-2 rollback (optional follow-up) | firmware | 1–2 days |

Roughly 3 days for items 1–6. Order matters: 1→2→3→4→6, with 5 anytime
after 1.

## Open questions

- Manifest poll cadence: piggyback a `fw` field on the existing 60 s state
  poll (server already answers it) vs. a separate low-frequency check?
  Piggybacking is cheaper; the state route is device-authenticated anyway.
- Keep `huge_app.csv` for the `esp32-s3-sim` env (no OTA there anyway) or
  unify all envs on the OTA layout for parity?
- Beta channel: reuse the Expo-style channel concept via
  `?channel=beta` on the manifest, gated by a device flag settable over BLE?
