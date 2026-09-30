# Finagotchi Firmware

ESP32-S3 Tamagotchi-style companion for the Finagotchi Solana savings-pet
app: 240×240 ST7789 TFT, BLE mirror of the pet (stage/mood/items/reactions),
day-based streaks, Wi-Fi provisioning over BLE, standalone cloud state sync
via a device token, battery gauge. See `BLE_PROTOCOL.md` for the wire
protocol, `HARDWARE_CONTRACT.md` for the app↔device↔API contract, and
`WIRING.md` for the hardware setup.

## Standalone cloud state sync (device token)

The device can keep showing authoritative pet state while the app is away,
without the app relaying anything:

1. **Pair** — in the app, the wallet calls `POST /device/pair` on the
   companion API (`https://api.finagotchi.app`), which mints a `fgd_…`
   device token. The API stores only a hash of it.
2. **Provision** — the app writes the token to the device over BLE as the
   optional third field of the provisioning payload
   (`<ssid>\n<passphrase>\n<deviceToken>`, encrypted writes only — see
   `BLE_PROTOCOL.md`). The device persists it in NVS (`fina`/`dtoken`).
3. **Sync** — while no app is connected, a sync task polls
   `GET <STATE_SYNC_URL>` (default
   `https://api.finagotchi.app/device/state`, overridable in
   `src/config.h`) every 60 s with `Authorization: Bearer <deviceToken>`
   and applies the compact JSON snapshot
   (`{"stage","sub","streak","mood","points","happy","upd"}`) through the
   same code path as a BLE state write. Unchanged polls cost no NVS writes;
   after 5 consecutive 401s the token is considered revoked and syncing
   stops until reboot.

Once a snapshot has been applied, the persisted `serverLinked` flag keeps
the demo auto-evolve and the local day-based streak tick off — the server
owns stage/streak. Devices without a token behave exactly as before.

### Prices

Independently of the state sync, each poll cycle also fetches **real
prices** over HTTPS: one batched call to the Jupiter Price API v3
(`https://api.jup.ag/price/v3?ids=<sol_mint>,<plan_mints...>` — note
`lite-api.jup.ag` is IPv6-only and unusable from the ESP32) gives real
on-chain DEX prices 24/7 for SOL/USD and every plan whose ticker has a
known xStock mint (mint table in `src/main.cpp`, sourced from the issuer's
`/api/v2/public/assets` API). xStock tickers without a known mint fall back
to the issuer's indicative quote
(`/public/assets/<symbol>/price-data` → `{"quote": <number|null>}`, null
while the market is closed — cached value kept). TLS is unverified
(`setInsecure`) — the data is public and read-only.

### Legacy DCA relay defines

`src/config.h` still carries `RELAY_HOST`/`DEVICE_ID` and the firmware
still contains the read-only DCA plan poll
(`GET http://<RELAY_HOST>/api/device/<DEVICE_ID>/dca`, CSV rows) that the
BTN1 standalone "sync now" action also triggers. No public server
implements that endpoint — the poll fails silently and the cached plan
state stays on screen. The token flow above is the supported standalone
sync path.

- When the app reconnects over BLE, all polling pauses and the app is
  authoritative again (it may resend `dca:count:`/`dca:plan:` and any
  missed `dca:hit:` events).
