# Finagotchi Firmware OTA Spec

Over-the-air firmware updates over Wi-Fi. Status: **implemented** (items
1–7; Tier 1 + Tier 2 boot safety, §5).

Implementation notes / deviations from the original spec:

- **Reboot-into-OTA-mode instead of §4 step 3's "suspend LVGL".** The N8
  breadboard build idles with ~19 KB free heap (LVGL + screens + BLE), and
  a TLS handshake + download buffer cannot fit next to the running UI. So a
  confirmed update is stashed in NVS (`fina` namespace: `otaPend`/`otaUrl`/
  `otaSha`/`otaSize`/`otaVer` + an `otaTries` attempt counter) and the
  device reboots. Early in `setup()` — before BLE/LVGL/pet — the marker is
  checked (`src/ota.cpp`): OTA mode is Serial + raw TFT_eSPI text + Wi-Fi
  only, downloads in 4 KB chunks (watchdog fed per chunk), SHA-256-verifies
  the written image before `Update.end(true)`, then reboots into the new
  slot. Any failure: `Update.abort()`, error on the raw screen for ~5 s,
  marker cleared, reboot into the untouched firmware. After 3 failed
  attempts the marker is dropped so a poisoned manifest can't boot-loop.
- **Battery-only guard.** The PCB has no VBUS sense pin, so "USB connected"
  can't be detected: the precondition is battery ≥ 40% OR no battery
  present (divider reads nothing = USB power). Checked both at confirm time
  and again at OTA-mode start.
- **Download URL must terminate TLS with a Let's Encrypt cert.** Firmware
  downloads validate against the pinned ISRG Root X1 (`src/api_root_ca.h`),
  so a raw `github.com/.../releases/download/...` asset URL (DigiCert)
  fails validation. The binaries are served from the API itself —
  `https://api.finagotchi.app/firmware/files/<env>/<version>/…` (§3); the
  device does not follow redirects to another host.

## Current state (pre-OTA baseline)

- The device speaks HTTPS (`WiFiClientSecure` + `HTTPClient`) to
  `https://api.finagotchi.app/device/state`; OTA reuses the same pinned-CA
  transport for the manifest fetch and the download.
- Storage is `Preferences` (NVS) only; there is **no filesystem** to
  preserve across the partition-table change.
- `platformio.ini` now uses `partitions_8mb_ota.csv` (dual OTA slots) for
  the hardware envs; `esp32-s3-sim` stays on `huge_app.csv` (Wokwi never
  OTA-updates).
- `src/version.h` carries `FW_VERSION`; `FW_BUILD` (git short hash) is
  injected by `tools/version_flags.py`.

## 1. Versioning — DONE

`FW_VERSION "x.y.z"` (semver) lives in `src/version.h`; `FW_BUILD` (git
short hash) is injected by the PlatformIO pre-script `tools/version_flags.py`
(falls back to `"dev"` when git is unavailable).

- Shown on the device: menu → **Update** row (the right-zone info word
  shows `v<FW_VERSION>`).
- Reported to the app: appended to the BLE state snapshot as the trailing
  9th field `fw:<version>` — tolerated-absent by old apps the same way the
  8th `subStage` field is (see `BLE_PROTOCOL.md`).
- Sent to the API as a query param on the manifest request
  (`?v=1.2.0&env=pcb-v1`) so the server decides "newer available" and can
  ship per-hardware builds (`env` is `pcb-v1` when built with
  `FINAGOTCHI_PCB_V1`, else `esp32-s3`).

## 2. Partition layout — DONE

`partitions_8mb_ota.csv` (repo root) replaces `huge_app.csv` on the
`esp32-s3` and `pcb-v1` envs:

```
nvs,      data, nvs,     0x9000,   0x5000
otadata,  data, ota,     0xe000,   0x2000
ota_0,    app,  ota_0,   0x10000,  0x380000   # 3.5 MB
ota_1,    app,  ota_1,   0x390000, 0x380000   # 3.5 MB
coredump, data, coredump,0x710000, 0x10000
```

`otadata` must be at `0xe000`: every USB upload writes `boot_app0.bin`
there. The first version of this table had NVS covering `0xe000` and
otadata at `0x710000`, so each USB flash corrupted the last NVS pages and
never reset the boot slot. Devices flashed with that table need one more
USB flash of this build (NVS is re-initialised if its pages are damaged).

(Current firmware.bin is ~2.0 MB with LVGL+BLE; 3.5 MB per slot is
comfortable headroom. NVS offsets stay standard so pet state survives.
`esp32-s3-sim` deliberately keeps `huge_app.csv` — the Wokwi build is never
flashed and never OTA-updates, so its layout stays untouched.)

**One-time migration:** devices flashed with `huge_app` cannot OTA
themselves into a dual-slot layout — the first OTA-capable flash goes over
USB (`pio run -e <env> -t upload`). Every flash after that is wireless.
This caveat ships in the release notes of the first OTA-capable version
(the release workflow's release body says it).

## 3. Server side (finagotchi_api_server) — DONE (manifest; hosting manual)

`src/routes/firmware.ts` implements:

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

  Returns `204 No Content` when the device is current (semver-compare; also
  when `v` is NEWER than the data file). `400` on a missing/malformed `env`
  or `v`, `404` on an unknown env. No auth (same policy as public price
  data); the binary is integrity-protected by the SHA-256, not by transport
  secrecy. Rate-limited per IP (`RATE_LIMIT_FIRMWARE_LATEST_IP_PER_MIN`,
  default 30/min).
- Manifest data lives in `data/firmware.json` (one entry per env:
  `esp32-s3`, `pcb-v1`), loaded once at router creation — a release updates
  the file and redeploys. The CI release workflow emits a ready-to-paste
  `manifest-snippet.json`.
- Binary hosting: `GET /firmware/files/<env>/<version>/finagotchi-<env>-<version>.bin`
  on the same API (Let's Encrypt cert — satisfies the §6 pin). Files live
  at `data/firmware/<env>/<version>/` on the API host (gitignored, uploaded
  at release time). The route is public like the manifest, answers with the
  exact bytes + `Content-Length` the device checks, rejects any file name
  that isn't precisely the release binary (path-traversal guard), and marks
  responses immutable. Tests: `test/firmware.test.ts` covers the
  200/204/400/404 manifest cases and the files route (bytes, headers,
  missing file, wrong name, traversal).

## 4. Device update flow — DONE (with the reboot-into-OTA-mode deviation)

Trigger (either):
- On-device: menu → **Update** row (info word = current
  `FW_VERSION`; crown scroll, push to select, cancel to abort).
- From the app: BLE write `ota:start`, with `ota:status:<state>` notifies
  on the state characteristic (`idle | checking | current |
  available:<ver> | scheduled:<ver> | error:<battery|wifi|http|manifest>` —
  see `BLE_PROTOCOL.md`). App-side UI is out of scope here (mobile repo).

Preconditions, checked before anything downloads:
- Battery ≥ 40% **or no battery present** (USB power). The PCB has no VBUS
  sense pin, so "USB connected" can't be detected — battery-only guard.
  Re-checked at OTA-mode start before the download.
- Wi-Fi joined (the check joins from the stored NVS credentials if not),
  and the wall clock synced (pinned-CA validation needs it — same guard as
  the cloud state sync).

Flow:
1. GET the manifest (`FIRMWARE_MANIFEST_URL`, default
   `https://api.finagotchi.app/firmware/latest?env=<board>&v=<FW_VERSION>`)
   over the pinned-CA `httpGetJson` path. 204 → "up to date" toast, done.
2. Confirm screen (`src/ui/screen_ota.cpp`, lazy build + free like the
   menu/lock screens): `v<current> -> v<new>` + size, "push = update /
   hold = abort" on PCB v1. It rides the existing screen/action plumbing
   (`uiScreenOtaConfirm`/`uiScreenOtaCancel` in input.cpp), so the
   breadboard (long = update, right short = abort) and the PCB crown
   (short push = update, long push = abort) both drive it with no new input code.
   The manifest's sha256 must be 64 hex chars and `size` ≤ the slot size —
   anything else is rejected as `error:manifest` and never scheduled.
3. **DEVIATION (replaces "suspend the LVGL UI task"):** confirm stashes the
   manifest in NVS and reboots into OTA mode (`src/ota.cpp`): no BLE, no
   LVGL, no pet — Serial + raw TFT_eSPI progress screen + Wi-Fi only, so
   the TLS handshake and 4 KB download buffer have the whole heap.
4. OTA mode: `Update.begin(size)` → stream in 4 KB chunks (watchdog fed per
   chunk, 30 s stall timeout) → SHA-256 of the written image verified
   against the manifest before `Update.end(true)`.
5. On success: marker cleared, reboot into the new slot. On any failure:
   `Update.abort()`, error on the raw screen ~5 s, reboot. Transient
   failures (Wi-Fi join, clock, stall, network/5xx) keep the marker so the
   next boot retries; permanent ones (bad data, 4xx, hash or flash errors)
   clear it. Either way the marker is dropped after 3 attempts. Then reboot
   into the untouched firmware. After 3 failed attempts the marker is
   dropped (no boot loop on a poisoned manifest).

## 5. Boot safety (two tiers)

- **Tier 1 (free, built into `Update`) — SHIPPED:** the boot partition only
  flips to the new slot after the full image is written and its internal
  checksum verifies (and we additionally SHA-256-verify against the
  manifest first). An interrupted or corrupted download can never boot —
  the old app stays selected. This covers network drops and brownouts.
- **Tier 2 (rollback) — SHIPPED:** the core's prebuilt bootloader has
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. `main.cpp` overrides the core's
  weak `verifyRollbackLater()` so a new image stays `PENDING_VERIFY`, and
  only calls `esp_ota_mark_app_valid_cancel_rollback()` after 30 s of a
  healthy loop (UI up, heap sane). A crash, watchdog reset or brownout
  before that makes the bootloader revert to the previous slot. OTA mode
  marks the running image valid before writing the other slot.

## 6. Security — DONE

- Manifest and binary over HTTPS with **real CA validation** — both use the
  pinned ISRG Root X1 in `src/api_root_ca.h` (the manifest via
  `httpGetJson(..., kApiRootCa)`; the download via
  `WiFiClientSecure::setCACert(kApiRootCa)` in `src/ota.cpp`). Never
  `setInsecure()`. Consequence: the binary URL must be served from a
  Let's Encrypt-issued host (§3).
- SHA-256 from the manifest checked against the written image before
  `Update.end(true)` (§4 step 4).
- Later option: ECDSA image signatures (`CONFIG_SECURE_SIGNED_APPS`) —
  out of scope for v1.

## 7. CI / release flow — DONE

`.github/workflows/release.yml`, on tag `fw-v*`:
1. `pio run -e esp32-s3 -e esp32-s3-sim -e pcb-v1`
2. Renames outputs `finagotchi-<env>-<version>.bin` (version read from
   `src/version.h`), computes SHA-256 (`SHA256SUMS.txt`)
3. Creates the GitHub Release with the binaries attached
4. Attaches `manifest-snippet.json` (`tools/make_manifest.py`) — its urls
   already point at the API's `/firmware/files/` route. Publishing = copy
   each `.bin` to the API host's `data/firmware/<env>/<version>/`, paste
   the snippet into `data/firmware.json`, redeploy. The API stays the
   single source of truth for "what is latest".

Channels (stable/beta) can mirror the Expo channel discipline later; v1 is
a single stable channel.

## 8. Work breakdown & estimate

| # | Item | Where | Status |
|---|------|-------|--------|
| 1 | `src/version.h` + `tools/version_flags.py` + Update menu row + BLE snapshot `fw:` field | firmware | ✅ done |
| 2 | `partitions_8mb_ota.csv` on `esp32-s3` + `pcb-v1` (one-time USB reflash per device) | firmware | ✅ done |
| 3 | `src/routes/firmware.ts` manifest + `/firmware/files/` binaries route + `data/firmware.json` (binaries uploaded to `data/firmware/<env>/<version>/` at release) | API server | ✅ done |
| 4 | Update flow: manifest check, reboot-into-OTA-mode download, progress screen, guards | firmware | ✅ done |
| 5 | `ota:start` / `ota:status:<state>` BLE commands (firmware side; app UI is a mobile-repo follow-up) | firmware | ✅ done |
| 6 | GitHub Actions release workflow (`fw-v*`) | firmware repo | ✅ done |
| 7 | Tier-2 rollback (self-test, then mark valid) | firmware | ✅ done |

## Open questions

- Manifest poll cadence: piggyback a `fw` field on the existing 60 s state
  poll (server already answers it) vs. a separate low-frequency check?
  Piggybacking is cheaper; the state route is device-authenticated anyway.
  (v1 ships with a manual check only: the menu row and `ota:start`.)
- ~~Keep `huge_app.csv` for the `esp32-s3-sim` env (no OTA there anyway) or
  unify all envs on the OTA layout for parity?~~ Decided: the sim env keeps
  `huge_app.csv` — it is never flashed and never OTA-updates.
- Beta channel: reuse the Expo-style channel concept via
  `?channel=beta` on the manifest, gated by a device flag settable over BLE?
- ~~Binary hosting~~ — answered: the API's own `/firmware/files/<env>/<version>/`
  route serves the bytes (LE cert, satisfies the pinned CA); binaries are
  uploaded to the API host's `data/firmware/` tree at release time (§3, §7).
