# Finagotchi Firmware

ESP32-S3 Tamagotchi-style companion for the Finagotchi Solana savings-pet
app: 240×240 ST7789 TFT, BLE mirror of the pet (stage/mood/items/reactions),
day-based streaks, Wi-Fi provisioning over BLE, battery gauge. See
`BLE_PROTOCOL.md` for the wire protocol and `WIRING.md` for the hardware
setup.

## DCA relay endpoint contract

While the app is disconnected over BLE, the device keeps its DCA plan
display fresh by polling a **read-only** relay every 30 minutes:

```
GET http://<RELAY_HOST>/api/device/<DEVICE_ID>/dca
```

`RELAY_HOST` and `DEVICE_ID` are compile-time defines in `src/config.h`
(see `src/config.h.example`). The endpoint must be safe to hit anonymously —
no secrets are baked into the firmware, and the device never calls Titan or
any signing API.

### Response

`200 OK`, `Content-Type: text/plain`, body is a semicolon-separated list of
comma-separated plan rows:

```
TICKER,amount,next_epoch,buys,holdings[,price_usd];TICKER,amount,next_epoch,buys,holdings[,price_usd];...
```

| Field | Type | Meaning |
|---|---|---|
| `TICKER` | string (≤6 chars) | Token symbol; matched case-sensitively against the tickers the app pushed over BLE |
| `amount` | float | SOL per buy |
| `next_epoch` | uint32 unix time | When the next buy is scheduled |
| `buys` | uint32 | Total buys executed so far |
| `holdings` | uint32 | Holdings currently held |
| `price_usd` | float, optional | Token unit price in USD (drives the price/valuation display; omit to keep the cached value) |

Example payload:

```
SPYX,0.25,1780200000,12,3,645.20;GOOGLX,0.1,1780310000,4,90,251.30
```

### Device behavior

- Rows are matched to the (up to 4) cached plans by ticker; unknown tickers
  are ignored, missing tickers keep their cached state.
- If remote `buys` > cached `buys`, the device shows a "+<delta×amount>
  TICKER" gain toast (delta capped at 9) and updates its cache.
- A plan past `next_epoch` whose `buys` didn't change since the last poll is
  marked overdue (amber ring on the carousel line).
- After every successful poll the device re-syncs its wall clock via NTP.
- Any failure — Wi-Fi down, HTTP error, 5 s timeout, malformed CSV — is
  silent: the last cached state stays on screen and the next cycle retries.
  The device never crashes or blanks the screen on relay trouble.
- Independently of the relay, each poll cycle also fetches **real prices**
  over HTTPS: one batched call to the Jupiter Price API v3
  (`https://api.jup.ag/price/v3?ids=<sol_mint>,<plan_mints...>` — note
  `lite-api.jup.ag` is IPv6-only and unusable from the ESP32) gives
  real on-chain DEX prices 24/7 for SOL/USD and every plan whose ticker has
  a known xStock mint (mint table in `src/main.cpp`, sourced from the
  issuer's `/api/v2/public/assets` API). xStock tickers without a known mint
  fall back to the issuer's indicative quote
  (`/public/assets/<symbol>/price-data` → `{"quote": <number|null>}`, null
  while the market is closed — cached value kept). TLS is unverified
  (`setInsecure`) — the data is public and read-only.
- When the app reconnects over BLE, polling pauses and the app is
  authoritative again (it may resend `dca:count:`/`dca:plan:` and any missed
  `dca:hit:` events).
