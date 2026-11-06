/*
  finagotchi_firmware
  ESP32-S3 + 1.3" ST7789 TFT display.
  Splash screen with the Finagotchi logo, then the animated blob avatar
  (egg -> coinling -> hodler -> whale, looping as a demo).

  BLE: advertises as "Finagotchi", exposes one characteristic (READ + NOTIFY
  + WRITE). The app reads/subscribes to
  "<stage>:<streak>:<mood>:<item>:<points>:<happy>[:<dcaCount>]" updates and
  writes commands to drive the pet (see BLE_PROTOCOL.md):
    stage:egg|coinling|hodler|whale
    look:<yaw>,<pitch>   look:off
    mood:<0-5>  item:<0-5>  react:jump|spin|glow|dance
    points:<n>  happy:<0-100>  streak:<n>
    dca:count:<n>  dca:plan:<i>:<en>:<epoch>:<amt>:<TICKER>:<buys>:<holdings>
    dca:clear  dca:hit:<n>:<TICKER>  solusd:<rate>
    ota:start   (firmware update check -> on-device confirm; see OTA.md)
  While the app is connected, the demo auto-evolve, the day-based streak
  check and the relay DCA poll are paused (the app is authoritative).

  Wi-Fi + NTP: syncs local time so the streak is day-based (consecutive days
  the device has been alive). Streak, points and happiness persist in NVS.
  Credentials come from config.h, or from the app over BLE: pair using the
  passkey shown on screen, then write "ssid\npass[\ndeviceToken]" to the
  provisioning characteristic (encrypted writes only) — stored in NVS from
  then on. With a device token, a sync task polls the companion API for
  authoritative pet state every 60 s while no app is connected; once a state
  has been applied the server owns stage/streak and the demo auto-evolve and
  local streak tick stay off.

  The bottom stats bar (streak / points / happiness) is always on screen —
  while no app is connected it plays under the waiting-for-connection scene.
*/

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <esp_wifi.h>   // esp_wifi_set_country (channels 12-13 domain)
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sntp.h>   // SNTP sync callback (late NTP -> timeSynced)
#include <esp_ota_ops.h>   // first-boot self-test after an OTA (rollback)
#if SHOW_RESET_REASON || LOW_POWER_RADIO_TEST
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_heap_caps.h>
#include <esp_phy_init.h>
#endif
#include <Preferences.h>
#include <array>
#include "config.h"
#include "logo.h"
#include "api_root_ca.h"
#include "price_root_ca.h"
#include "version.h"
#include "ota.h"
#include "pet.h"
#include "feedback.h"
#include "nearby.h"
#include "ui/ui.h"

// Defaults so older config.h copies (pre-DCA) still build.
#ifndef RELAY_HOST
#define RELAY_HOST "relay.example.com"
#endif
#ifndef DEVICE_ID
#define DEVICE_ID "finagotchi-01"
#endif
// Companion API endpoint the state sync task polls with the device token.
// Override in config.h to point at a self-hosted/staging API. TLS for this
// endpoint is validated against kApiRootCa (api_root_ca.h) — replace that
// PEM too if the override host is not issued by Let's Encrypt.
#ifndef STATE_SYNC_URL
#define STATE_SYNC_URL "https://api.finagotchi.app/device/state"
#endif

// OTA firmware manifest endpoint (OTA.md §3): GET ?env=<board>&v=<FW_VERSION>
// -> 204 when current, else {version,url,sha256,size,notes}. Same pinned-CA
// TLS path as the state sync, no auth (public, like the price data).
#ifndef FIRMWARE_MANIFEST_URL
#define FIRMWARE_MANIFEST_URL "https://api.finagotchi.app/firmware/latest"
#endif

// Legacy standalone DCA relay poll. The current backend has NO
// /api/device/<id>/dca route — the app pushes the full plan table over BLE
// and /device/state covers standalone state — so polling the relay just
// burns a 404 (and a TCP connection) every 30 min. Off by default; set
// RELAY_POLL_ENABLED 1 in config.h only if you run a relay that implements
// the CSV contract documented in README.md.
#ifndef RELAY_POLL_ENABLED
#define RELAY_POLL_ENABLED 0
#endif

// Perf stats in loop(), off by default (build with -D UI_PERF_LOG=1).
#ifndef UI_PERF_LOG
#define UI_PERF_LOG 0
#endif

namespace {

TFT_eSPI tft;
FinagotchiPet pet;

#ifdef NO_RADIO_DEMO
constexpr bool kNoRadioDemo = true;
#else
constexpr bool kNoRadioDemo = false;
#endif

// ---------------------------------------------------------------------------
// BLE
// ---------------------------------------------------------------------------

constexpr const char* SERVICE_UUID        = "0000f1a0-0000-1000-8000-00805f9b34fb";
constexpr const char* CHARACTERISTIC_UUID = "0000f1a1-0000-1000-8000-00805f9b34fb";
constexpr const char* PROV_CHARACTERISTIC_UUID = "0000f1a2-0000-1000-8000-00805f9b34fb";

BLEServer*         pServer = nullptr;
BLECharacteristic* pCharacteristic = nullptr;
volatile bool      appConnected = false;

const char* STAGE_NAMES[kPetStateCount] = { "egg", "coinling", "hodler", "whale" };
uint8_t  subStage = 1;
uint32_t streak = 0;
uint8_t  mood = static_cast<uint8_t>(PetMoodId::MOOD_CALM);
uint8_t  item = static_cast<uint8_t>(PetItem::ITEM_NONE);
uint32_t points = 0;
uint8_t  happiness = 50;
uint8_t  bgTheme = 0;   // PetCanvas scene theme (menu backdrop row / bg:)

// Parental lock (optional 4-digit code, NVS "fina"). When set, the device
// boots locked: the pet animates but the money screens ask for the code.
// Boot recovery clears it along with BLE bonds (crown-only hold on PCB v1).
bool     pinSet = false;
uint16_t pinCode = 0;
bool     timeSynced = false;
// Once the companion server has pushed state at least once it owns
// stage/streak: the demo auto-evolve and local streak tick stay off
// (persisted as "srvlink" in NVS).
bool     serverLinked = false;
Preferences prefs;

// Wi-Fi credentials: config.h defaults, overridden by app-provisioned NVS
// values (see the provisioning characteristic below).
char wifiSsid[33] = WIFI_SSID;
char wifiPass[65] = WIFI_PASS;

// Companion-API device token (optional 3rd provisioning field, NVS "dtoken").
// Empty = no cloud state sync.
char deviceToken[160] = "";

// Mood names matching the server JSON (order matches PetMoodId).
const char* MOOD_NAMES[kMoodCount] = {
  "calm", "happy", "excited", "waiting", "sleepy", "sad"
};

// Collectible names for the menu accessory row (order matches PetItem).
const char* ITEM_NAMES[kItemCount] = {
  "none", "crown", "glasses", "bowtie", "halo", "diamond", "tshirt"
};

// ---------------------------------------------------------------------------
// DCA plans (read-only mirror of the app's multi-DCA tracker)
//
// The app is authoritative while connected (dca:count / dca:plan / dca:clear
// / dca:hit writes). While disconnected, a FreeRTOS poll task refreshes the
// same slots from the read-only relay (RELAY_HOST/DEVICE_ID in config.h).
// Plans persist in NVS under the "finagotchi" namespace: key "dcaCount"
// (UChar slot count) and "dca0".."dca3" (raw DcaPlan blobs).
// ---------------------------------------------------------------------------

DcaPlan  plans[kDcaMaxPlans] = {};
bool     planOverdue[kDcaMaxPlans] = {};   // past nextBuyEpoch, no new buys
// price_usd came from the app (8th dca:plan field, > 0): fresher than the
// on-device feed, so fetchPrices() skips self-fetching that ticker. Runtime
// only — after a reboot the device self-fetches until the app pushes again.
bool     appPrice[kDcaMaxPlans] = {};
uint8_t  dcaCount = 0;
volatile bool plansDirty = false;          // poll task -> loop task handoff
volatile bool dcaNudgePending = false;     // (dis)connect -> re-eval mood nudge
float    solUsdRate = 0.0f;                // SOL/USD (BLE solusd: or price feed)
volatile bool priceDirty = false;          // price feed -> loop task handoff
volatile bool syncRequested = false;       // BTN1 -> poll task: sync now
void     fetchPrices();                    // defined in the poll section below
bool     pollDcaRelay();                   // defined in the poll section below
void     actionCheckUpdate();              // defined in the OTA section below
void     otaCheckFromTask();               // OTA section: runs on the poll task
volatile bool otaCheckRequested = false;   // menu/BLE -> poll task handoff

void loadPlans() {
  prefs.begin("finagotchi", true);
  dcaCount = prefs.getUChar("dcaCount", 0);
  if (dcaCount > kDcaMaxPlans) dcaCount = kDcaMaxPlans;
  for (size_t i = 0; i < dcaCount; i++) {
    char key[8];
    snprintf(key, sizeof(key), "dca%u", static_cast<unsigned>(i));
    if (prefs.getBytes(key, &plans[i], sizeof(DcaPlan)) != sizeof(DcaPlan))
      memset(&plans[i], 0, sizeof(DcaPlan));   // short/absent blob -> empty slot
    plans[i].ticker[sizeof(plans[i].ticker) - 1] = 0;   // belt & braces
  }
  prefs.end();
  for (size_t i = 0; i < dcaCount; i++)
    ui::setDcaPlan(static_cast<uint8_t>(i), plans[i], planOverdue[i]);

  // SOL/USD rate for the amount unit toggle on the DCA screen (solusd:).
  prefs.begin("finagotchi", true);
  solUsdRate = prefs.getFloat("solUsd", 0.0f);
  prefs.end();
  ui::setSolUsd(solUsdRate);
}

void savePlans() {
  prefs.begin("finagotchi", false);
  prefs.putUChar("dcaCount", dcaCount);
  for (size_t i = 0; i < dcaCount; i++) {
    char key[8];
    snprintf(key, sizeof(key), "dca%u", static_cast<unsigned>(i));
    prefs.putBytes(key, &plans[i], sizeof(DcaPlan));
  }
  prefs.end();
}

// dca:count / dca:clear: remove all plan blobs, then reset the count.
void wipePlans() {
  prefs.begin("finagotchi", false);
  for (size_t i = 0; i < kDcaMaxPlans; i++) {
    char key[8];
    snprintf(key, sizeof(key), "dca%u", static_cast<unsigned>(i));
    prefs.remove(key);
  }
  prefs.putUChar("dcaCount", 0);
  prefs.end();
  memset(plans, 0, sizeof(plans));
  memset(planOverdue, 0, sizeof(planOverdue));
  memset(appPrice, 0, sizeof(appPrice));
  dcaCount = 0;
  ui::clearDcaPlans();
}


// ---------------------------------------------------------------------------
// Demo seed (sim build only): placeholder xStocks plans so the DCA cards
// and next-buy chip have content without an app/relay. Enabled via
// -D DCA_DEMO_SEED=1 (env:esp32-s3-sim) — never in the production build.
// ---------------------------------------------------------------------------

#ifdef DCA_DEMO_SEED
void seedDemoPlans() {
  if (dcaCount > 0) return;   // real NVS data wins
  uint32_t now = timeSynced ? static_cast<uint32_t>(time(nullptr)) : 0;
  uint32_t base = now ? now : 1780000000u;
  auto mk = [](uint32_t epoch, float amt, const char* tick,
               uint32_t buys, float held, float price) {
    DcaPlan p{};
    p.enabled = true;
    p.nextBuyEpoch = epoch;
    p.amountSol = amt;
    strlcpy(p.ticker, tick, sizeof(p.ticker));
    p.buys = buys;
    p.holdingsHeld = held;
    p.priceUsd = price;
    return p;
  };
  plans[0] = mk(base + 2 * 86400 + 14 * 3600, 0.25f, "SPYX",   12,   3, 645.20f);
  plans[1] = mk(base + 5 * 3600,              0.10f, "GOOGLX",  4,  90, 251.30f);
  plans[2] = mk(base - 3600,                  0.05f, "HOODX",   7,  42, 108.45f);  // overdue
  plans[3] = mk(0,                            0.02f, "TSLAX",   5,  10, 255.00f);  // paused
  plans[3].enabled = false;   // paused plans arrive as en=0, epoch=0
  dcaCount = 4;
  for (size_t i = 0; i < dcaCount; i++) {
    planOverdue[i] = (i == 2);
    ui::setDcaPlan(static_cast<uint8_t>(i), plans[i], planOverdue[i]);
  }
  dcaNudgePending = true;   // show the overdue mood nudge too
  ui::setSolUsd(212.40f);   // demo SOL/USD rate for the amount toggle
  Serial.println("DEMO: seeded placeholder plans (SPYX / GOOGLX / HOODX / TSLAX paused)");
}
#endif

bool setupWiFiTime();          // defined in the Wi-Fi section below
void updateStreakFromTime();
const char* wifiFailCode();    // Wi-Fi section: wifi:fail code mapping

// Push "<stage>:<streak>:<mood>:<item>:<points>:<happy>[:<dcaCount>[:<sub>
// [:fw:<ver>]]]" (read + notify). The optional trailing fields are
// tolerated-absent: old firmware parses only the first six, old apps stop
// at what they know. The 9th field is the firmware version ("fw:1.0.0") so
// the app can offer an update when the API's manifest is newer (OTA.md §1).
// Can exceed 20 bytes — the app should negotiate MTU >= 64.
void blePushState(PetState s) {
  if (!pCharacteristic) return;
  char buf[64];
  snprintf(buf, sizeof(buf), "%s:%lu:%u:%u:%lu:%u:%u:%u:fw:%s",
           STAGE_NAMES[static_cast<size_t>(s)], static_cast<unsigned long>(streak), mood, item,
           static_cast<unsigned long>(points), happiness, dcaCount, subStage, FW_VERSION);
  pCharacteristic->setValue(buf);
  pCharacteristic->notify();
  ui::setStats(streak, points, happiness);
  ui::setMenuAccessory(ITEM_NAMES[item]);   // keep the menu rows in sync
  ui::setMenuMood(MOOD_NAMES[mood]);
  ui::setMenuBackdrop(FinagotchiPet::sceneThemeName(bgTheme));
  Serial.printf("BLE -> %s\n", buf);
}

// Join-result report to the app (a SEPARATE notification on the state
// characteristic — the app monitors it): "wifi:ok:<ssid>" on a real join,
// "wifi:fail:<code>" on failure. Restores the snapshot as the read value
// afterwards (same pattern as sync:req). Safe with no subscribers.
// Codes: off = no credentials/nothing attempted, ssid = no AP found,
// auth = wrong password/handshake family, ip = connected but no IP.
void bleNotifyWifi(bool ok, const char* code) {
  if (!pCharacteristic) return;
  char buf[48];
  if (ok) snprintf(buf, sizeof(buf), "wifi:ok:%s", wifiSsid);
  else snprintf(buf, sizeof(buf), "wifi:fail:%s", code ? code : "off");
  pCharacteristic->setValue(buf);
  pCharacteristic->notify();
  Serial.printf("BLE -> %s\n", buf);
  blePushState(pet.state());   // restore the snapshot as the read value
}

// OTA status report to the app: "ota:status:<state>" as a separate
// notification on the state characteristic (same pattern as wifi:ok/fail —
// the app monitors it, old apps ignore it). States: idle | checking |
// current | available:<ver> | scheduled:<ver> | error:<reason> with reason
// in battery|wifi|http|manifest. The download itself runs in OTA mode after
// a reboot (BLE is down there), so no progress states exist.
void bleNotifyOta(const char* state) {
  if (!pCharacteristic) return;
  char buf[48];
  snprintf(buf, sizeof(buf), "ota:status:%s", state);
  pCharacteristic->setValue(buf);
  pCharacteristic->notify();
  Serial.printf("BLE -> %s\n", buf);
  blePushState(pet.state());   // restore the snapshot as the read value
}

// App-store stages 1-12 collapse to the 4 base shapes. Shared by the BLE
// stage:<n> handler and the cloud state sync.
PetState petStateForStage(int n) {
  static const std::array<PetState, 12> numMap = {
    PetState::PET_EGG,                                  // 1
    PetState::PET_COINLING, PetState::PET_COINLING,     // 2-3
    PetState::PET_COINLING, PetState::PET_COINLING,     // 4-5
    PetState::PET_COINLING, PetState::PET_COINLING,     // 6-7
    PetState::PET_HODLER,   PetState::PET_HODLER,       // 8-9
    PetState::PET_WHALE,    PetState::PET_WHALE,
    PetState::PET_WHALE                                 // 10-12
  };
  if (n < 1) n = 1;
  if (n > 12) n = 12;
  return numMap[static_cast<size_t>(n - 1)];
}

// Shared snapshot apply for the BLE full-state write and the cloud state
// sync: sets stage/sub-stage/stats, persists the stats in NVS, pushes the
// notify. `sub` <= 0 keeps the current sub-stage.
void applySnapshot(PetState state, int sub, uint32_t s, uint8_t m, uint32_t p, uint8_t h) {
  float nowSec = millis() / 1000.0f;
  pet.setState(state, nowSec);
  if (sub >= 1) {
    subStage = static_cast<uint8_t>(sub > 12 ? 12 : sub);
    ui::setSubStage(subStage);
  }
  streak = s;
  points = p;
  happiness = h > 100 ? 100 : h;
  mood = m < kMoodCount ? m : static_cast<uint8_t>(PetMoodId::MOOD_CALM);
  pet.setMood(static_cast<PetMoodId>(mood), nowSec);
  prefs.begin("fina", false);
  prefs.putUInt("streak", streak);
  prefs.putUInt("points", points);
  prefs.putUChar("happy", happiness);
  prefs.end();
  blePushState(pet.state());
}

// Handle one app command, e.g. "stage:coinling" or "look:-15,8".
void handleCommand(const char* cmd) {
  float nowSec = millis() / 1000.0f;

  if (strncmp(cmd, "stage:", 6) == 0) {
    const char* name = cmd + 6;
    for (size_t i = 0; i < kPetStateCount; i++) {
      if (strcmp(name, STAGE_NAMES[i]) == 0) {
        pet.setState(static_cast<PetState>(i), nowSec);
        blePushState(pet.state());
        return;
      }
    }
    // App store uses numeric stages 1-12; collapse them to the 4 base shapes.
    char* end = nullptr;
    long n = strtol(name, &end, 10);
    if (end != name && *end == 0 && n >= 1 && n <= 12) {
      subStage = static_cast<uint8_t>(n);
      ui::setSubStage(subStage);
      pet.setState(petStateForStage(static_cast<int>(n)), nowSec);
      blePushState(pet.state());
      return;
    }
    Serial.printf("BLE: unknown stage '%s'\n", name);
  }
  else if (strncmp(cmd, "substage:", 9) == 0) {
    int v = atoi(cmd + 9);
    subStage = static_cast<uint8_t>(v < 1 ? 1 : (v > 12 ? 12 : v));
    ui::setSubStage(subStage);
    blePushState(pet.state());
  }
  else if (strncmp(cmd, "react:", 6) == 0) {
    const char* name = cmd + 6;
    PetReaction r = PetReaction::REACT_NONE;
    if (strcmp(name, "jump") == 0) r = PetReaction::REACT_JUMP;
    else if (strcmp(name, "spin") == 0) r = PetReaction::REACT_SPIN;
    else if (strcmp(name, "glow") == 0) r = PetReaction::REACT_GLOW;
    else if (strcmp(name, "dance") == 0) r = PetReaction::REACT_DANCE;
    pet.react(r, nowSec);
  }
  else if (strncmp(cmd, "look:", 5) == 0) {
    if (strcmp(cmd + 5, "off") == 0) {
      pet.clearLook(nowSec);
      return;
    }
    float yaw, pitch;
    // NaN/inf would poison the gaze blend (clamping NaN yields NaN) and
    // the face renders garbage until reboot.
    if (sscanf(cmd + 5, "%f,%f", &yaw, &pitch) == 2 &&
        isfinite(yaw) && isfinite(pitch)) {
      pet.setLook(yaw, pitch, nowSec);
    }
  }
  else if (strncmp(cmd, "mood:", 5) == 0) {
    // Bounds-check BEFORE storing: mood indexes MOOD_NAMES in
    // blePushState, so an out-of-range id was an out-of-bounds read.
    long m = strtol(cmd + 5, nullptr, 10);
    if (m < 0 || m >= static_cast<long>(kMoodCount)) {
      Serial.printf("BLE: mood %ld out of range\n", m);
      return;
    }
    mood = static_cast<uint8_t>(m);
    pet.setMood(static_cast<PetMoodId>(mood), nowSec);
    blePushState(pet.state());
  }
  else if (strncmp(cmd, "bg:", 3) == 0) {
    // Scene backdrop theme (PetCanvas BACKGROUND_COLORS names). Optional:
    // old apps never send it, unknown names are ignored.
    int t = FinagotchiPet::sceneThemeForName(cmd + 3);
    if (t >= 0) {
      bgTheme = static_cast<uint8_t>(t);
      pet.setSceneTheme(bgTheme);
      ui::setMenuBackdrop(FinagotchiPet::sceneThemeName(bgTheme));
      Serial.printf("BLE: scene theme '%s'\n", FinagotchiPet::sceneThemeName(bgTheme));
    } else {
      Serial.printf("BLE: unknown bg '%s'\n", cmd + 3);
    }
  }
  else if (strncmp(cmd, "item:", 5) == 0) {
    long it = strtol(cmd + 5, nullptr, 10);
    // Unknown ids degrade to none (atoi + uint8_t truncation let 256 alias 0).
    item = static_cast<uint8_t>(it >= 0 && it < static_cast<long>(kItemCount) ? it : 0);
    pet.setItem(static_cast<PetItem>(item));
    blePushState(pet.state());
  }
  else if (strncmp(cmd, "points:", 7) == 0) {
    points = static_cast<uint32_t>(strtoul(cmd + 7, nullptr, 10));
    prefs.begin("fina", false);
    prefs.putUInt("points", points);
    prefs.end();
    blePushState(pet.state());
  }
  else if (strncmp(cmd, "happy:", 6) == 0) {
    int v = atoi(cmd + 6);
    happiness = static_cast<uint8_t>(v < 0 ? 0 : (v > 100 ? 100 : v));
    prefs.begin("fina", false);
    prefs.putUChar("happy", happiness);
    prefs.end();
    blePushState(pet.state());
  }
  else if (strncmp(cmd, "streak:", 7) == 0) {
    streak = static_cast<uint32_t>(strtoul(cmd + 7, nullptr, 10));
    // Persist so the minute-tick day check doesn't revert the app's
    // override, and stamp the day so a stale lastDay doesn't make the tick
    // reset/bump the streak the app just pushed once it resumes.
    prefs.begin("fina", false);
    prefs.putUInt("streak", streak);
    if (timeSynced) {
      struct tm ti;
      if (getLocalTime(&ti, 0))
        prefs.putUInt("lastDay", static_cast<uint32_t>(ti.tm_year + 1900) * 400 + static_cast<uint32_t>(ti.tm_yday));
    }
    prefs.end();
    blePushState(pet.state());
  }
  else if (strncmp(cmd, "dca:count:", 10) == 0) {
    // Declares how many dca:plan: writes follow; old slots are wiped first.
    int n = atoi(cmd + 10);
    wipePlans();
    dcaCount = static_cast<uint8_t>(n < 0 ? 0 : (n > static_cast<int>(kDcaMaxPlans)
                                     ? static_cast<int>(kDcaMaxPlans) : n));
    prefs.begin("finagotchi", false);
    prefs.putUChar("dcaCount", dcaCount);
    prefs.end();
    Serial.printf("BLE: dca count=%u\n", dcaCount);
  }
  else if (strncmp(cmd, "dca:plan:", 9) == 0) {
    // dca:plan:<i>:<enabled>:<next_buy_epoch>:<amount>:<TICKER>:<buys>:<holdings>[:<price_usd>]
    // The 8th field (token unit price, USD) is optional; older apps omit it.
    // holdings is a FLOAT (fractional tokens, e.g. "1.5") — parsing it as an
    // integer stops sscanf at the dot and silently drops the price field.
    unsigned i, en;
    unsigned long epoch, buys;
    float amt, hold, price = 0.0f;
    char tick[7];
    int got = sscanf(cmd + 9, "%u:%u:%lu:%f:%6[^:]:%lu:%f:%f",
                     &i, &en, &epoch, &amt, tick, &buys, &hold, &price);
    // Reject values the UI can't render sanely (NaN/inf/negative amounts,
    // tickers with control or non-ASCII bytes the font has no glyphs for).
    if (got >= 7) {
      bool tickOk = tick[0] != 0;
      for (const char* t = tick; *t; t++)
        if (!isalnum(static_cast<uint8_t>(*t))) { tickOk = false; break; }
      if (!tickOk || !isfinite(amt) || amt < 0.0f || !isfinite(hold) || hold < 0.0f ||
          (got == 8 && (!isfinite(price) || price < 0.0f))) {
        Serial.println("BLE: dca:plan rejected (bad ticker/number)");
        return;
      }
    }
    if (got >= 7 && i >= kDcaMaxPlans) {
      Serial.printf("BLE: dca:plan slot %u over capacity (max %u)\n", i,
                    static_cast<unsigned>(kDcaMaxPlans));
    } else if (got >= 7) {
      DcaPlan& p = plans[i];
      p.enabled = en != 0;
      p.nextBuyEpoch = static_cast<uint32_t>(epoch);
      p.amountSol = amt;
      strlcpy(p.ticker, tick, sizeof(p.ticker));
      p.buys = static_cast<uint32_t>(buys);
      p.holdingsHeld = hold;
      p.priceUsd = got == 8 ? price : 0.0f;
      planOverdue[i] = false;   // fresh app data: app is authoritative
      // The 8th field is always sent by current apps (0 when unknown): a
      // positive price marks the slot app-priced so fetchPrices() skips
      // self-fetching that ticker; 0 clears the slot's price (no stale
      // values on reused slots).
      appPrice[i] = (got == 8 && price > 0.0f);
      if (i >= dcaCount) dcaCount = static_cast<uint8_t>(i + 1);
      prefs.begin("finagotchi", false);
      char key[8];
      snprintf(key, sizeof(key), "dca%u", i);
      prefs.putBytes(key, &p, sizeof(DcaPlan));
      prefs.putUChar("dcaCount", dcaCount);
      prefs.end();
      ui::setDcaPlan(static_cast<uint8_t>(i), p, false);
      Serial.printf("BLE: dca plan %u %s %.4g SOL next=%lu\n", i, p.ticker,
                    static_cast<double>(p.amountSol), epoch);
    } else {
      Serial.printf("BLE: malformed dca:plan (%d fields)\n", got);
    }
  }
  else if (strcmp(cmd, "dca:clear") == 0) {
    wipePlans();
    Serial.println("BLE: dca cleared");
  }
  else if (strncmp(cmd, "dca:hit:", 8) == 0) {
    // dca:hit:<n>:<TICKER> — a buy just executed: toast + dance + sparkles.
    unsigned n;
    char tick[7];
    if (sscanf(cmd + 8, "%u:%6[A-Za-z0-9]", &n, tick) == 2) {
      char t[24];
      snprintf(t, sizeof(t), "+%u %s", n > 999 ? 999 : n, tick);
      ui::enqueueRewardToast(t);   // a buy landed: amber celebration toast
      pet.react(PetReaction::REACT_DANCE, nowSec);
      pet.sparkleBurst(nowSec);
      Serial.printf("BLE: dca hit %s\n", t);
    }
  }
  else if (strncmp(cmd, "solusd:", 7) == 0) {
    // SOL/USD rate for the DCA-screen amount unit toggle (persisted).
    float rate = strtof(cmd + 7, nullptr);
    if (isfinite(rate) && rate > 0.0f && rate < 1e6f) {
      solUsdRate = rate;
      ui::setSolUsd(rate);
      prefs.begin("finagotchi", false);
      prefs.putFloat("solUsd", rate);
      prefs.end();
      Serial.printf("BLE: SOL/USD=%.2f\n", static_cast<double>(rate));
    }
  }
  else if (strcmp(cmd, "ota:start") == 0) {
    // Same flow as the menu "Update" row: battery/Wi-Fi guard, manifest
    // check, then the on-device confirm screen (push = update).
    actionCheckUpdate();
  }
  else if (strncmp(cmd, "epoch:", 6) == 0) {
    // Fallback clock set from the app (devices whose Wi-Fi has no NTP
    // access). Ignored once NTP has synced — NTP stays authoritative.
    if (!timeSynced) {
      time_t t = static_cast<time_t>(strtoul(cmd + 6, nullptr, 10));
      if (t > 1700000000 && t < 4102444800) {   // sanity: 2023..2100
        struct timeval tv = { t, 0 };
        settimeofday(&tv, nullptr);
        timeSynced = true;
        Serial.printf("BLE: clock set from app (%lu)\n", static_cast<unsigned long>(t));
      }
    }
  }
  else {
    // "<stage>:<streak>:<mood>:<item>[:<points>:<happy>[:<dcaCount>[:<sub>]]]".
    // Lets the app push everything in one write instead of field-by-field
    // commands. The 7th field (plan count, device-owned) is parsed but
    // ignored, so new and old app builds can share one format. When the
    // snapshot does not fit the negotiated ATT payload the app falls back
    // to a 4-field head write followed by separate points:/happy:/substage:
    // commands (HARDWARE_CONTRACT §3.1), so 4 fields must also parse —
    // points/happy keep their current values in that case.
    char sname[12];
    unsigned long s, p;
    unsigned m, it, h, dc, ss;
    int parsed = sscanf(cmd, "%11[^:]:%lu:%u:%u:%lu:%u:%u:%u", sname, &s, &m, &it, &p, &h, &dc, &ss);
    if (parsed >= 4) {
      // Stage name -> one of the 4 base shapes (unknown name: keep the
      // current shape). If a sub-stage is present (8th field), store it too;
      // otherwise keep the current sub-stage. Item stays a BLE-only field —
      // the cloud snapshot has no collectibles.
      PetState ps = pet.state();
      for (size_t i = 0; i < kPetStateCount; i++) {
        if (strcmp(sname, STAGE_NAMES[i]) == 0) {
          ps = static_cast<PetState>(i);
          break;
        }
      }
      item = static_cast<uint8_t>(it < kItemCount ? it : static_cast<unsigned>(PetItem::ITEM_NONE));
      pet.setItem(static_cast<PetItem>(item));
      applySnapshot(ps, parsed >= 8 ? static_cast<int>(ss < 1 ? 1 : ss) : 0,
                    static_cast<uint32_t>(s),
                    static_cast<uint8_t>(m < kMoodCount ? m : static_cast<unsigned>(PetMoodId::MOOD_CALM)),
                    parsed >= 5 ? static_cast<uint32_t>(p) : points,
                    parsed >= 6 ? static_cast<uint8_t>(h > 100 ? 100 : h) : happiness);
      return;
    }
    Serial.printf("BLE: unknown command '%s'\n", cmd);
  }
}

// Received writes are queued here and processed on the loop task: the
// Bluedroid BTC task that runs onWrite has a small stack, and doing
// printf/strtok/sscanf/NVS/notify in the callback overflowed it (stack
// canary panic, BTC_TASK). A small FIFO (not a single slot): the app
// streams dca:count + one dca:plan per plan back-to-back, and a single
// slot silently dropped every write that landed while the loop was busy
// rendering — plans went missing. Slots hold a full ATT payload (MTU 256
// - 3), so ';'-batched writes are never cut mid-command.
constexpr size_t CMD_SLOT_LEN = 254;
constexpr uint8_t CMD_SLOTS = 8;
char cmdQueue[CMD_SLOTS][CMD_SLOT_LEN];
uint16_t cmdQueueLen[CMD_SLOTS];
volatile uint8_t cmdHead = 0;   // written by the BTC task
volatile uint8_t cmdTail = 0;   // written by the loop task
volatile uint32_t cmdDropped = 0;
portMUX_TYPE cmdMux = portMUX_INITIALIZER_UNLOCKED;

bool cmdPending() { return cmdHead != cmdTail; }

class CmdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    std::string v = c->getValue();
    if (v.empty()) return;
    portENTER_CRITICAL(&cmdMux);
    if (static_cast<uint8_t>(cmdHead - cmdTail) < CMD_SLOTS) {
      uint8_t slot = cmdHead % CMD_SLOTS;
      size_t n = v.size() < CMD_SLOT_LEN - 1 ? v.size() : CMD_SLOT_LEN - 1;
      memcpy(cmdQueue[slot], v.data(), n);
      cmdQueue[slot][n] = 0;
      cmdQueueLen[slot] = static_cast<uint16_t>(n);
      cmdHead++;
    } else {
      cmdDropped++;   // logged from the loop task
    }
    portEXIT_CRITICAL(&cmdMux);
  }
};

// Runs on the loop task: log + dispatch one received write.
void processCommand() {
  char buf[CMD_SLOT_LEN];
  size_t n;
  portENTER_CRITICAL(&cmdMux);
  uint8_t slot = cmdTail % CMD_SLOTS;
  n = cmdQueueLen[slot];
  memcpy(buf, cmdQueue[slot], n + 1);
  cmdTail++;
  uint32_t dropped = cmdDropped;
  cmdDropped = 0;
  portEXIT_CRITICAL(&cmdMux);
  if (dropped) Serial.printf("BLE: %lu write(s) dropped (queue full)\n",
                             static_cast<unsigned long>(dropped));
  // Embedded NULs would hide the rest of the payload from the parser.
  n = strnlen(buf, n);

  // Log length + payload. Hex-dump non-ASCII writes so binary or
  // truncated payloads are still visible on the serial monitor.
  bool ascii = true;
  for (size_t i = 0; i < n; i++) {
    if (buf[i] < 32 || buf[i] > 126) { ascii = false; break; }
  }
  if (ascii) {
    Serial.printf("BLE <- (%u) %s\n", static_cast<unsigned>(n), buf);
  } else {
    Serial.printf("BLE <- (%u) hex:", static_cast<unsigned>(n));
    for (size_t i = 0; i < n; i++) Serial.printf(" %02X", static_cast<uint8_t>(buf[i]));
    Serial.println();
  }

  // Allow several commands per write, separated by ';'
  char* tok = strtok(buf, ";");
  while (tok) {
    handleCommand(tok);
    tok = strtok(nullptr, ";");
  }
}

// ---------------------------------------------------------------------------
// Wi-Fi provisioning (app -> device, first-time setup)
//
// Pairing uses BLE Secure Connections with bonding; the device has a screen,
// so it displays the 6-digit passkey (ESP_IO_CAP_OUT) and the app must enter
// it. BOTH characteristics require encrypted access (state: encrypted
// read+write, provisioning: encrypted write), so the phone initiates pairing
// on first contact — the passkey shows without needing a provisioning write.
// ---------------------------------------------------------------------------

volatile bool passkeyPending = false;
volatile bool authPushPending = false;   // BTC task -> loop: push snapshot after pairing
uint32_t pairingPasskey = 0;

class SecCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override {
    // We are DisplayOnly (IO_CAP_OUT): this would mean the PEER wants us to
    // type a key — shouldn't happen; log it loudly if it does.
    Serial.println("BLE security: PASSKEY_REQ (unexpected for DisplayOnly)");
    return 0;
  }
  void onPassKeyNotify(uint32_t pass_key) override {
    // ESP_GAP_BLE_PASSKEY_NOTIF_EVT: the Bluedroid path that actually
    // carries the 6-digit code for SC DisplayOnly — show it on screen.
    pairingPasskey = pass_key;
    passkeyPending = true;
    Serial.printf("BLE pairing passkey: %06lu\n", static_cast<unsigned long>(pass_key));
  }
  bool onSecurityRequest() override {
    Serial.println("BLE security: SEC_REQ from peer (accepted)");
    return true;
  }
  bool onConfirmPIN(uint32_t pin) override {
    // Numeric Comparison never applies to a DisplayOnly responder; a request
    // here means a downgraded/odd pairing — refuse instead of auto-accepting
    // (auto-yes made the comparison worthless against a MITM).
    Serial.printf("BLE security: numeric comparison %06lu rejected\n",
                  static_cast<unsigned long>(pin));
    return false;
  }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
    passkeyPending = false;
    // Log the negotiated auth mode so a capture proves MITM passkey pairing
    // (ESP_LE_AUTH_REQ_SC_MITM_BOND = 0x0D) vs the old Just Works (0x04/0x08).
    Serial.printf("BLE pairing %s (auth_mode=0x%02X, key_type=0x%02X, fail_reason=0x%02X)\n",
                  cmpl.success ? "OK" : "FAILED",
                  static_cast<unsigned>(cmpl.auth_mode),
                  static_cast<unsigned>(cmpl.key_type),
                  static_cast<unsigned>(cmpl.fail_reason));
    if (cmpl.success) {
      // Push a fresh snapshot as soon as the encrypted link is up so the
      // app gets data immediately. A FAILED here with a previously-paired
      // phone = stale bond (phone side: forget the device; device side:
      // hold both buttons at boot to erase bonds — see WIRING.md).
      // blePushState touches LVGL (stats/menu labels): defer it to the
      // loop task — this callback runs on the BTC task.
      authPushPending = true;
    }
  }
};

// Erase every BLE bond Bluedroid persists in NVS. Call right after
// BLEDevice::init() when the boot chord (both buttons held) fired.
void eraseBleBonds() {
  int num = esp_ble_get_bond_device_num();
  Serial.printf("BLE: %d bonded device(s) in NVS\n", num);
  if (num <= 0) return;
  // Heap list sized to the real count: a fixed 10-entry array left bonds
  // 11+ behind (the stack keeps up to CONFIG_BT_SMP_MAX_BONDS = 15).
  esp_ble_bond_dev_t* list = static_cast<esp_ble_bond_dev_t*>(
      malloc(sizeof(esp_ble_bond_dev_t) * static_cast<size_t>(num)));
  if (!list) return;
  int count = num;
  if (esp_ble_get_bond_device_list(&count, list) == ESP_OK) {
    for (int i = 0; i < count; i++) esp_ble_remove_bond_device(list[i].bd_addr);
    Serial.printf("BLE: erased %d bond(s)\n", count);
  }
  free(list);
}

// Writes are stashed and processed on the loop task (see cmdQueue above).
// Sized for the full 3-field payload: 32 + 63 + 128 + 2 separators + NUL.
char provBuf[232];
volatile size_t provLen = 0;
volatile bool provPending = false;
portMUX_TYPE provMux = portMUX_INITIALIZER_UNLOCKED;

class ProvCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    std::string v = c->getValue();
    if (v.empty()) return;
    portENTER_CRITICAL(&provMux);
    if (!provPending) {
      size_t n = v.size() < sizeof(provBuf) - 1 ? v.size() : sizeof(provBuf) - 1;
      memcpy(provBuf, v.data(), n);
      provBuf[n] = 0;
      provLen = n;
      provPending = true;
    }
    portEXIT_CRITICAL(&provMux);
  }
};

void logCredentialShape(const char* field, const char* value, size_t len) {
  unsigned nonPrint = 0;
  for (size_t i = 0; i < len; i++) {
    uint8_t c = static_cast<uint8_t>(value[i]);
    if (c < 32 || c > 126) nonPrint++;
  }
  Serial0.printf("PROV: %s leading_space=%d trailing_space=%d non_print=%u\n",
                 field, len > 0 && value[0] == ' ',
                 len > 0 && value[len - 1] == ' ', nonPrint);
}

// Screen overlays (status messages + the pairing passkey panel) are LVGL
// widgets on lv_layer_top() — see ui/ui.cpp. The passkey panel is driven
// from loop() off passkeyPending/pairingPasskey.

// Runs on the loop task: validate + persist "ssid\npass[\ndeviceToken]",
// then reconnect. The device token (companion API, cloud state sync) is
// optional: a 2-field write is the legacy payload and leaves any stored
// token untouched. If ssid+pass match the stored NVS credentials the write
// is a no-op (no disconnect/rejoin) except that a provided token is still
// persisted.
void processProvision() {
  char buf[232];
  portENTER_CRITICAL(&provMux);
  size_t n = provLen;
  memcpy(buf, provBuf, n + 1);
  provPending = false;
  portEXIT_CRITICAL(&provMux);

  // Newline is the separator: it appears in neither a WPA passphrase
  // (printable ASCII only) nor a sane SSID.
  char* nl = strchr(buf, '\n');
  if (!nl) {
    Serial.println("PROV: rejected (expected \"ssid\\npass\")");
    ui::setConnectStatus("setup failed - retry");
    ui::showOverlay("WiFi setup failed", 2500);
    return;
  }
  *nl = 0;
  const char* ssid = buf;
  char* pass = nl + 1;
  // Optional third field: split it off before validating the passphrase.
  const char* token = nullptr;
  char* nl2 = strchr(pass, '\n');
  if (nl2) {
    *nl2 = 0;
    token = nl2 + 1;
  }
  size_t sl = strlen(ssid), pl = strlen(pass);
  Serial0.printf("PROV: payload=%u ssid_len=%u pass_len=%u token=%d\n",
                 static_cast<unsigned>(n), static_cast<unsigned>(sl),
                 static_cast<unsigned>(pl), token ? 1 : 0);
  logCredentialShape("ssid", ssid, sl);
  logCredentialShape("pass", pass, pl);
  if (sl < 1 || sl > 32 || pl > 63 || (pl > 0 && pl < 8)) {
    Serial.printf("PROV: rejected (ssid %u chars, pass %u chars)\n",
                  static_cast<unsigned>(sl), static_cast<unsigned>(pl));
    ui::setConnectStatus("setup failed - retry");
    ui::showOverlay("WiFi setup failed", 2500);
    return;
  }
  size_t tl = token ? strlen(token) : 0;
  if (token && (tl < 1 || tl > 128)) {
    Serial.printf("PROV: rejected (token %u chars)\n", static_cast<unsigned>(tl));
    ui::setConnectStatus("setup failed - retry");
    ui::showOverlay("WiFi setup failed", 2500);
    return;
  }

  // The app auto-pushes its current Wi-Fi credentials on every connect, so
  // the same creds arriving again is the common case: skip the ~10 s
  // disconnect/rejoin and just ack. A token field is still persisted so
  // token rotation keeps working on a 2-field-no-change write.
  prefs.begin("fina", true);
  String storedSsid = prefs.getString("wssid", "");
  String storedPass = prefs.getString("wpass", "");
  prefs.end();
  if (storedSsid == ssid && storedPass == pass) {
    if (token) {
      prefs.begin("fina", false);
      prefs.putString("dtoken", token);
      prefs.end();
      strlcpy(deviceToken, token, sizeof(deviceToken));
    }
    Serial.printf("PROV: credentials unchanged ('%s')%s\n",
                  ssid, token ? ", device token updated" : "");
    if (WiFi.status() == WL_CONNECTED) {
      // Same creds and already online: report success, skip the rejoin.
      Serial.printf("PROV: already on '%s'\n", ssid);
      char msg[48];
      snprintf(msg, sizeof(msg), "Already on %s", ssid);
      ui::setMenuWifi("online");
      ui::setConnectStatus("online");
      ui::showOverlay(msg, 2500);
      ui::setWifiOnline(true);
      bleNotifyWifi(true, nullptr);
    } else {
      // Same creds but offline: attempt one rejoin and report the outcome.
      Serial.println("PROV: unchanged but offline, rejoining");
      ui::setMenuWifi("joining");
      ui::setConnectStatus("joining Wi-Fi...");
      ui::showOverlay("WiFi rejoining...", 2500);
      ui::showWifiSetup(true);
      timeSynced = setupWiFiTime();   // blocks up to ~10 s, user-triggered
      ui::showWifiSetup(false);
      if (timeSynced) updateStreakFromTime();
      bool ok = WiFi.status() == WL_CONNECTED;
      ui::setMenuWifi(ok ? "online" : "retry");
      ui::setConnectStatus(ok ? "online" : "Wi-Fi failed - retry");
      ui::showOverlay(ok ? (timeSynced ? "Online!" : "Online - clock pending")
                         : "WiFi failed", 2500);
      ui::setWifiOnline(ok);
      bleNotifyWifi(ok, ok ? nullptr : wifiFailCode());
    }
    return;
  }

  prefs.begin("fina", false);
  prefs.putString("wssid", ssid);
  prefs.putString("wpass", pass);
  if (token) prefs.putString("dtoken", token);
  prefs.end();
  strlcpy(wifiSsid, ssid, sizeof(wifiSsid));
  strlcpy(wifiPass, pass, sizeof(wifiPass));
  if (token) strlcpy(deviceToken, token, sizeof(deviceToken));
  ui::setMenuWifi("joining");
  ui::setConnectStatus("joining Wi-Fi...");
  Serial.printf("PROV: credentials for '%s' saved%s, reconnecting...\n", ssid,
                token ? " (+ device token)" : "");
  ui::showOverlay("WiFi saved, joining...", 4000);

  // Driver stays initialized: a plain disconnect (driver up), then
  // setupWiFiTime -> wifiRejoin brings STA back with the new credentials.
  WiFi.disconnect(false);
  ui::showWifiSetup(true);
  timeSynced = setupWiFiTime();   // blocks up to ~10 s, once, user-triggered
  ui::showWifiSetup(false);
  updateStreakFromTime();
  bool ok = WiFi.status() == WL_CONNECTED;
  ui::setMenuWifi(ok ? "online" : "retry");
  ui::setConnectStatus(ok ? "online" : "Wi-Fi failed - retry");
  ui::showOverlay(ok ? (timeSynced ? "Online!" : "Online - clock pending")
                     : "WiFi failed", 2500);
  ui::setWifiOnline(ok);
  // Report the real join result to the app (it monitors the state
  // characteristic) — wifi:ok:<ssid> or wifi:fail:<code>.
  bleNotifyWifi(ok, ok ? nullptr : wifiFailCode());
}

// ---------------------------------------------------------------------------
// Serial Wi-Fi provisioning (no app needed) accepts native USB or UART0.
// Protocol (two lines, so spaces in SSIDs/passwords are fine):
//   wifi:MySSID<enter>
//   MyPass<enter>
// Feeds the SAME buffer/flag as BLE provisioning.
// ---------------------------------------------------------------------------
char serBuf[96];
size_t serLen = 0;
char pendingSsid[40];       // set by the "wifi:<ssid>" line
bool havePendingSsid = false;

void pollSerial() {
  Stream* port = Serial0.available() ? static_cast<Stream*>(&Serial0)
                                     : static_cast<Stream*>(&Serial);
  while (port->available()) {
    char c = static_cast<char>(port->read());
    if (c == '\r') continue;
    if (c != '\n') {
      if (serLen < sizeof(serBuf) - 1) serBuf[serLen++] = c;
      continue;
    }
    serBuf[serLen] = 0;
    serLen = 0;
    if (serBuf[0] == 0) continue;
    if (strncmp(serBuf, "wifi:", 5) == 0) {
      strlcpy(pendingSsid, serBuf + 5, sizeof(pendingSsid));
      havePendingSsid = true;
      port->printf("SER: ssid '%s' — now type the password and press enter\n",
                   pendingSsid);
    } else if (havePendingSsid) {
      // same payload shape as the BLE characteristic: "ssid\npass"
      portENTER_CRITICAL(&provMux);
      if (!provPending) {
        int n = snprintf(provBuf, sizeof(provBuf), "%s\n%s", pendingSsid, serBuf);
        if (n > 0 && n < static_cast<int>(sizeof(provBuf))) {
          provLen = n;
          provPending = true;
        }
      }
      portEXIT_CRITICAL(&provMux);
      havePendingSsid = false;
      port->println("SER: provisioning...");
    } else if (strcmp(serBuf, "help") == 0) {
      port->println("SER: wifi:MySSID <enter>, then the password <enter>");
    } else {
      port->printf("SER: unknown command '%s' (try 'help')\n", serBuf);
    }
  }
}

// These callbacks run on the Bluedroid BTC task. LVGL is NOT thread-safe:
// touching widgets here raced the loop task's lv_timer_handler / screen
// deletes and corrupted LVGL's event list (LoadProhibited panic in
// lv_event_mark_deleted when a phone connected while the user navigated).
// So they only flip flags; loop() applies the UI side (linkUiPending).
volatile bool linkUiPending = false;

class SrvCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    appConnected = true;
    linkUiPending = true;
    Serial.println("App connected (demo paused).");
  }
  void onConnect(BLEServer* s, esp_ble_gatts_cb_param_t* param) override {
    // Idle disconnects: with no device-side preference, the phone's initial
    // conn params can pair a slow interval with a tight supervision timeout,
    // and missed anchors (the device is busy pushing frames) drop the link.
    // Ask for 30-50 ms intervals, slave latency 0, 5 s supervision timeout.
    // (units: interval in 1.25 ms, timeout in 10 ms)
    s->updateConnParams(param->connect.remote_bda, 24, 40, 0, 500);
  }
  void onDisconnect(BLEServer* s) override {
    appConnected = false;
    linkUiPending = true;     // loop: spinner, status, clearLook
    dcaNudgePending = true;   // overdue nudge may apply again offline
    s->getAdvertising()->start();    // keep advertising for the next connection
    Serial.println("App disconnected (demo resumed).");
  }
};

void showBootStatus(const char* msg);

void setupBLE(bool wipeBonds) {
#if RESET_RF_CALIBRATION
  esp_err_t calErr = esp_phy_erase_cal_data_in_nvs();
  Serial.printf("RF calibration erased: %s (%d); full recalibration follows\n",
                esp_err_to_name(calErr), calErr);
#endif
#if SHOW_RESET_REASON
  analogReadResolution(12);
  analogSetPinAttenuation(5, ADC_11db);
  uint32_t adcMv = 0;
  for (int i = 0; i < 8; i++) {
    adcMv += analogReadMilliVolts(5);
    delay(5);
  }
  Serial.printf("Supply before BLE: VBAT=%.2f V (below 2.50 V means USB-only)\n",
                static_cast<double>(adcMv / 8 * 2.0f / 1000.0f));
  Serial.printf("BLE preinit: status=%d heap=%u internal=%u largest=%u\n",
                static_cast<int>(esp_bt_controller_get_status()), ESP.getFreeHeap(),
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
    esp_bt_controller_config_t btConfig = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#if LOW_POWER_RADIO_TEST
    btConfig.txpwr_dft = ESP_PWR_LVL_N12;
#endif
    esp_err_t err = esp_bt_controller_init(&btConfig);
    Serial.printf("BLE controller init: %s (%d), status=%d\n", esp_err_to_name(err), err,
                  static_cast<int>(esp_bt_controller_get_status()));
    if (err != ESP_OK) {
      tft.fillScreen(TFT_RED);
      showBootStatus("BT INIT FAILED");
      while (true) delay(1000);
    }
  }
#endif
  BLEDevice::init("Finagotchi");
#if LOW_POWER_RADIO_TEST
  esp_err_t powerErr = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_N12);
  Serial.printf("LOW POWER RADIO: CPU=%u MHz, BLE TX=-12 dBm (%s)\n",
                getCpuFrequencyMhz(), esp_err_to_name(powerErr));
#endif
#if SHOW_RESET_REASON
  if (!btStarted() || esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
    Serial.printf("BLE enable failed: controller=%d bluedroid=%d\n",
                  static_cast<int>(esp_bt_controller_get_status()),
                  static_cast<int>(esp_bluedroid_get_status()));
    tft.fillScreen(TFT_RED);
    showBootStatus("BT ENABLE FAILED");
    while (true) delay(1000);
  }
#endif
  tft.fillScreen(TFT_BLUE);
  showBootStatus("BLE controller");
  if (wipeBonds) eraseBleBonds();   // recovery chord fired at boot
  BLEDevice::setMTU(256);   // 3-field provisioning write + state string exceed 128

  // Secure Connections + bonding; the device displays the passkey.
  BLEDevice::setSecurityCallbacks(new SecCallbacks());
  BLESecurity* pSecurity = new BLESecurity();
  // Secure Connections + bonding + MITM: without MITM the phone negotiates
  // Just Works/Numeric Comparison and NO passkey is ever generated — with
  // MITM our DisplayOnly side (ESP_IO_CAP_OUT) gets ESP_GAP_BLE_PASSKEY_NOTIF
  // (the code to show) and the phone its system passkey-entry dialog.
  pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  pSecurity->setCapability(ESP_IO_CAP_OUT);
  pSecurity->setKeySize(16);
  pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  pSecurity->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  // Requesting MITM is not enough: a peer that advertises NoInputNoOutput
  // still negotiates Just Works and gets an encrypted (but unauthenticated)
  // link — enough to satisfy *_ENCRYPTED permissions and push Wi-Fi
  // credentials or commands without ever seeing the on-screen passkey.
  // Refuse any pairing weaker than the requested SC+MITM+bond mode.
  uint8_t onlySpecified = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_ENABLE;
  esp_ble_gap_set_security_param(ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH,
                                 &onlySpecified, sizeof(onlySpecified));
  tft.fillScreen(TFT_GREEN);
  showBootStatus("BLE security");

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new SrvCallbacks());
  tft.fillScreen(TFT_YELLOW);
  showBootStatus("BLE server");

  BLEService* pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_READ |
      BLECharacteristic::PROPERTY_NOTIFY |
      BLECharacteristic::PROPERTY_WRITE |
      BLECharacteristic::PROPERTY_WRITE_NR   // accept writeWithoutResponse too,
                                             // or those packets are dropped
  );
  pCharacteristic->setCallbacks(new CmdCallbacks());
  // Authenticated (MITM / passkey) encryption on reads + writes of the STATE
  // characteristic: pairing (the passkey) is then required on first contact
  // — the phone initiates it on the first state read/subscribe. The plain
  // ENCRYPTED bits accepted a Just Works link; ENC_MITM does not. The base
  // READ/WRITE bits are kept per the proven provisioning-char pattern (the
  // stack still enforces the strongest requirement present). The 2-byte
  // CCCD descriptor below keeps default perms so enabling notifications
  // after bonding still works.
  pCharacteristic->setAccessPermissions(ESP_GATT_PERM_READ |
                                        ESP_GATT_PERM_READ_ENC_MITM |
                                        ESP_GATT_PERM_WRITE |
                                        ESP_GATT_PERM_WRITE_ENC_MITM);
  // CCCD (0x2902) as a plain 2-byte descriptor: BLE2902() allocates the
  // default ESP_GATT_MAX_ATTR_LEN (600 B) for a value that is only ever 2
  // bytes, and that oversized malloc is what starved on a full heap.
  BLEDescriptor* cccd = new BLEDescriptor(BLEUUID((uint16_t)0x2902), 2);
  uint8_t cccdInit[2] = {0, 0};
  cccd->setValue(cccdInit, 2);   // notifications start disabled
  pCharacteristic->addDescriptor(cccd);
  pCharacteristic->setValue("egg:0:0:0:0:50");

  // Wi-Fi provisioning: authenticated (passkey) encrypted writes only — the
  // stack rejects writes on an unpaired or Just Works link, which is what
  // forces the passkey pairing first.
  BLECharacteristic* pProvChar = pService->createCharacteristic(
      PROV_CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_WRITE |
      BLECharacteristic::PROPERTY_WRITE_NR
  );
  pProvChar->setAccessPermissions(ESP_GATT_PERM_WRITE |
                                  ESP_GATT_PERM_WRITE_ENC_MITM);
  pProvChar->setCallbacks(new ProvCallbacks());
  tft.fillScreen(TFT_WHITE);
  showBootStatus("GATT start");
  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->start();

  Serial.println("BLE ready. Pair with Finagotchi app.");
}

// ---------------------------------------------------------------------------
// Hardware buttons (GPIO to GND, uses internal pull-ups)
//
// Debounce/press detection and the LVGL keypad indev live in ui/input.cpp
// (final mapping documented there). main.cpp only owns the semantic actions
// that touch BLE/pet state, registered with ui::begin() in setup().
// ---------------------------------------------------------------------------

// Reaction cycle for the menu's Play row.
const std::array<PetReaction, 4> REACT_CYCLE = {
  PetReaction::REACT_JUMP, PetReaction::REACT_SPIN,
  PetReaction::REACT_GLOW, PetReaction::REACT_DANCE
};
uint8_t reactCycleIdx = 0;

// Mood cycle for BTN1 long-press
const std::array<PetMoodId, 5> MOOD_CYCLE = {
  PetMoodId::MOOD_CALM, PetMoodId::MOOD_HAPPY, PetMoodId::MOOD_EXCITED,
  PetMoodId::MOOD_SLEEPY, PetMoodId::MOOD_SAD
};
uint8_t moodCycleIdx = 0;

// BTN1 short on the pet screen: sync now. With the app connected it is
// authoritative, so ask it to resend state + plans ("sync:req"); standalone,
// run the Wi-Fi sync (relay + prices) on the poll task.
void actionSyncNow(float nowSec) {
  (void)nowSec;
  if (appConnected) {
    pCharacteristic->setValue("sync:req");
    pCharacteristic->notify();
    blePushState(pet.state());   // restore the snapshot as the read value
    ui::enqueueToast("syncing...");
    Serial.println("BTN1: sync requested from app");
  } else {
    syncRequested = true;
    ui::enqueueToast("syncing...");
    Serial.println("BTN1: Wi-Fi sync requested");
  }
}

void actionCycleReaction(float nowSec) {
  PetReaction r = REACT_CYCLE[reactCycleIdx];
  reactCycleIdx = (reactCycleIdx + 1) % REACT_CYCLE.size();
  pet.react(r, nowSec);
  Serial.printf("BTN1 double: reaction %u\n", static_cast<unsigned>(r));
}

// Menu "Feed pet": the app is authoritative — ask it for a feed (it will
// push the resulting happy:/points: back) and play a local reaction right
// away so the device feels alive even before the answer lands.
void actionFeedPet(float nowSec) {
  pet.react(PetReaction::REACT_JUMP, nowSec);
  if (appConnected) {
    pCharacteristic->setValue("feed:req");
    pCharacteristic->notify();
    blePushState(pet.state());   // restore the snapshot as the read value
    ui::enqueueToast("feeding...");
    Serial.println("Menu: feed requested from app");
  } else {
    ui::enqueueToast("connect the app");
    Serial.println("Menu: feed requested (no app connected)");
  }
}

// Menu accessory row: cycle the collectible locally and mirror it in the
// notify snapshot (works offline — local only then).
void actionCycleItem() {
  item = static_cast<uint8_t>((item + 1) % kItemCount);
  pet.setItem(static_cast<PetItem>(item));
  blePushState(pet.state());
  Serial.printf("Menu: item=%u (%s)\n", item, ITEM_NAMES[item]);
}

// Menu mood row (absorbs the old BTN1-long mood cycle).
void actionCycleMood(float nowSec) {
  PetMoodId m = MOOD_CYCLE[moodCycleIdx];
  moodCycleIdx = (moodCycleIdx + 1) % MOOD_CYCLE.size();
  mood = static_cast<uint8_t>(m);
  pet.setMood(m, nowSec);
  blePushState(pet.state());
  Serial.printf("Menu: mood=%u (%s)\n", mood, MOOD_NAMES[mood]);
}

// Menu backdrop row: cycle the PetCanvas scene theme locally. Cosmetic
// only — the app's bg: write overrides it on connect. (bgTheme itself is
// declared with the other BLE-mirrored state above.)
void actionCycleBackdrop() {
  bgTheme = (bgTheme + 1) % FinagotchiPet::kSceneThemeCount;
  pet.setSceneTheme(bgTheme);
  ui::setMenuBackdrop(FinagotchiPet::sceneThemeName(bgTheme));
  Serial.printf("Menu: backdrop=%s\n", FinagotchiPet::sceneThemeName(bgTheme));
}

// --- parental lock (menu "Parental" row + lock screen callbacks) -------------

// Brute-force throttle: a 4-digit code is only 10,000 guesses. The first
// PIN_FREE_TRIES wrong codes are free; every further miss locks the pad for
// 30 s, doubling per miss up to 15 min. The failure count lives in NVS
// ("pinfail"), so rebooting does not reset it — the lockout re-arms at boot.
constexpr uint8_t  PIN_FREE_TRIES = 5;
constexpr uint32_t PIN_LOCKOUT_BASE_MS = 30000;
constexpr uint32_t PIN_LOCKOUT_MAX_MS = 15UL * 60UL * 1000UL;
uint8_t  pinFails = 0;
uint32_t pinLockoutUntil = 0;   // millis(); 0 = none

void pinArmLockout() {
  if (pinFails < PIN_FREE_TRIES) { pinLockoutUntil = 0; return; }
  uint8_t over = pinFails - PIN_FREE_TRIES;
  uint32_t ms = over >= 5 ? PIN_LOCKOUT_MAX_MS : PIN_LOCKOUT_BASE_MS << over;
  if (ms > PIN_LOCKOUT_MAX_MS) ms = PIN_LOCKOUT_MAX_MS;
  pinLockoutUntil = millis() + ms;
  if (pinLockoutUntil == 0) pinLockoutUntil = 1;
}

uint32_t pinLockoutRemainingMs() {
  if (pinLockoutUntil == 0) return 0;
  int32_t left = static_cast<int32_t>(pinLockoutUntil - millis());
  if (left <= 0) { pinLockoutUntil = 0; return 0; }
  return static_cast<uint32_t>(left);
}

// The row opens the code pad in the right mode: no code stored -> set one
// (enter twice); code stored -> enter it to remove.
void actionParentalLock() {
  ui::showLock(pinSet ? 3 : 1);
}

bool actionPinCheck(uint16_t pin) {
  if (!pinSet) return false;
  if (pinLockoutRemainingMs() > 0) return false;   // UI shows the wait
  if (pin == pinCode) {
    if (pinFails) {
      pinFails = 0;
      prefs.begin("fina", false);
      prefs.putUChar("pinfail", 0);
      prefs.end();
    }
    return true;
  }
  // Wrong code: count it (persisted, so a power cycle doesn't reset the
  // budget) and, past the free tries, back off exponentially.
  if (pinFails < 255) pinFails++;
  prefs.begin("fina", false);
  prefs.putUChar("pinfail", pinFails);
  prefs.end();
  pinArmLockout();
  Serial.printf("Parental: wrong code (%u failed)\n", pinFails);
  return false;
}

uint32_t actionPinLockoutMs() {
  return pinLockoutRemainingMs();
}

void actionPinStore(uint16_t pin) {
  pinCode = pin;
  pinSet = true;
  pinFails = 0;
  pinLockoutUntil = 0;
  prefs.begin("fina", false);
  prefs.putUShort("pin", pinCode);
  prefs.putBool("pinset", true);
  prefs.putUChar("pinfail", 0);
  prefs.end();
  ui::setMenuLock("on");
  Serial.println("Parental: code set");
}

void actionPinClear() {
  pinSet = false;
  pinCode = 0;
  pinFails = 0;
  pinLockoutUntil = 0;
  prefs.begin("fina", false);
  prefs.putBool("pinset", false);
  prefs.putUChar("pinfail", 0);
  prefs.end();
  ui::setMenuLock("off");
  Serial.println("Parental: code removed");
}

// Detail confirm: pause/resume a plan. Optimistic local toggle (the app
// rewrites the whole table back, reconciling), then dca:pause:<slot>.
void actionTogglePause(uint8_t slot) {
  if (slot >= dcaCount) return;
  if (!appConnected) {
    ui::enqueueToast("connect the app");
    Serial.println("DCA pause: no app connected");
    return;
  }
  plans[slot].enabled = !plans[slot].enabled;
  planOverdue[slot] = false;
  savePlans();
  ui::setDcaPlan(slot, plans[slot], planOverdue[slot]);   // optimistic visuals
  char req[16];
  snprintf(req, sizeof(req), "dca:pause:%u", slot);
  pCharacteristic->setValue(req);
  pCharacteristic->notify();
  blePushState(pet.state());   // restore the snapshot as the read value
  Serial.printf("BLE -> %s (optimistic %s)\n", req,
                plans[slot].enabled ? "resumed" : "paused");
}

// Create-screen confirm: dca:new:<TICKER>:<amountSol>:<freqSec>. The app
// opens its DCA wizard prefilled; no ack. Returns true when sent.
bool actionCreatePlan(const char* ticker, float amountSol, uint32_t freqSec) {
  if (!appConnected) {
    ui::enqueueToast("connect the app");
    Serial.println("DCA new: no app connected");
    return false;
  }
  char req[40];
  snprintf(req, sizeof(req), "dca:new:%s:%.4g:%lu", ticker,
           static_cast<double>(amountSol), static_cast<unsigned long>(freqSec));
  pCharacteristic->setValue(req);
  pCharacteristic->notify();
  blePushState(pet.state());
  ui::enqueueToast("check the app");
  Serial.printf("BLE -> %s\n", req);
  return true;
}

// ---------------------------------------------------------------------------
// Battery gauge (LiPo via 1:1 divider on ADC1)
// ---------------------------------------------------------------------------

// LiPo+ through a 1:1 divider (e.g. 2x 100k) to GPIO5. ADC1, because ADC2 is
// unusable while Wi-Fi is on. (GPIO4 is taken by the left button.)
constexpr uint8_t BATTERY_ADC_PIN = 5;
constexpr float BATTERY_DIVIDER = 2.0f;
constexpr int BATTERY_FULL_MV = 4200;      // LiPo, rested
constexpr int BATTERY_EMPTY_MV = 3000;
constexpr int BATTERY_PRESENT_MV = 2500;   // below this: no battery, USB power
constexpr uint32_t BATTERY_READ_MS = 5000;

void setupBattery() {
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);   // full ~3.1 V range
}

void updateBattery() {
  static uint32_t lastRead = 0;
  static float ema = -1.0f;
  uint32_t nowMs = millis();
  if (nowMs - lastRead < BATTERY_READ_MS) return;
  lastRead = nowMs;

  int mv = static_cast<int>(analogReadMilliVolts(BATTERY_ADC_PIN) * BATTERY_DIVIDER);
  if (mv < BATTERY_PRESENT_MV) {   // divider reads nothing: USB-powered
    ui::clearBattery();
    ema = -1.0f;
    return;
  }

  // Exponential smoothing — a LiPo under load wobbles tens of mV.
  ema = ema < 0.0f ? static_cast<float>(mv) : ema * 0.7f + mv * 0.3f;
  int pct = static_cast<int>((ema - BATTERY_EMPTY_MV) * 100.0f /
                             (BATTERY_FULL_MV - BATTERY_EMPTY_MV));
  pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
  ui::setBattery(static_cast<uint8_t>(pct));
}

// Fresh, unsmoothed read for the OTA battery guard (updateBattery's EMA
// trails by design, and the guard wants the value now). Returns 100 when no
// battery is present = USB power — the PCB has no VBUS sense pin, so the
// OTA guard is battery-only.
int batteryPercentNow() {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogReadMilliVolts(BATTERY_ADC_PIN);
    delay(5);
  }
  int mv = static_cast<int>(sum / 8 * BATTERY_DIVIDER);
  if (mv < BATTERY_PRESENT_MV) return 100;
  int pct = (mv - BATTERY_EMPTY_MV) * 100 / (BATTERY_FULL_MV - BATTERY_EMPTY_MV);
  return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

// ---------------------------------------------------------------------------
// Wi-Fi + NTP
// ---------------------------------------------------------------------------

// App-provisioned credentials (NVS) win over the config.h defaults. Also
// loads the optional companion-API device token (cloud state sync).
void loadWiFiCreds() {
  prefs.begin("fina", true);
  String s = prefs.getString("wssid", "");
  String p = prefs.getString("wpass", "");
  String t = prefs.getString("dtoken", "");
  prefs.end();
  if (s.length() > 0 && s.length() <= 32) {
    strlcpy(wifiSsid, s.c_str(), sizeof(wifiSsid));
    strlcpy(wifiPass, p.c_str(), sizeof(wifiPass));
    Serial.printf("WiFi: using provisioned credentials (ssid '%s')\n", wifiSsid);
  }
  if (t.length() > 0 && t.length() <= 128) {
    strlcpy(deviceToken, t.c_str(), sizeof(deviceToken));
    Serial.println("WiFi: device token loaded (cloud sync enabled)");
  }
}

// --- join diagnostics ---------------------------------------------------------
// Last STA disconnect reason (wifi_event_sta_disconnected_t.reason), recorded
// by a WiFi event hook installed on the first rejoin attempt. 0 = none yet.
volatile int lastDisconnectReason = 0;
bool wifiEventsHooked = false;
volatile bool wifiOnlinePending = false;   // task/event -> loop: icon update
volatile bool wifiOnlineState = false;

// Common 802.11 reason codes from esp_wifi_types.h.
const char* wifiReasonStr(int r) {
  switch (r) {
    case 1:   return "UNSPECIFIED";
    case 2:   return "AUTH_EXPIRE (wrong password?)";
    case 3:   return "AUTH_LEAVE";
    case 4:   return "ASSOC_EXPIRE";
    case 5:   return "ASSOC_TOOMANY";
    case 8:   return "ASSOC_LEAVE";
    case 15:  return "4WAY_HANDSHAKE_TIMEOUT (wrong password?)";
    case 39:  return "TIMEOUT";
    case 201: return "NO_AP_FOUND (SSID not found)";
    case 202: return "AUTH_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    default:  return "other";
  }
}

volatile bool wifiGotIpPending = false;    // event -> loop: start NTP, icon, notify
volatile bool ntpSyncedPending = false;    // SNTP callback -> loop: clock is real

void hookWifiEvents() {
  if (wifiEventsHooked) return;
  wifiEventsHooked = true;
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    lastDisconnectReason = info.wifi_sta_disconnected.reason;
    Serial.printf("WiFi: STA disconnected, reason=%d (%s)\n",
                  lastDisconnectReason, wifiReasonStr(lastDisconnectReason));
    Serial0.printf("WiFi: disconnected reason=%d (%s)\n",
                   lastDisconnectReason, wifiReasonStr(lastDisconnectReason));
    // Runs on the Wi-Fi event task: just flag it, loop() updates the icon.
    wifiOnlineState = false;
    wifiOnlinePending = true;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  // The boot join is non-blocking, so nothing else notices when it lands:
  // without this the icon stayed "offline" until the first poll (60 s) and
  // NTP never started, leaving the streak/countdowns on "--".
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
    wifiOnlineState = true;
    wifiOnlinePending = true;
    wifiGotIpPending = true;
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  // SNTP completion (lwIP task): the only reliable "clock is real" signal
  // for syncs that land after the short blocking waits gave up.
  sntp_set_time_sync_notification_cb([](struct timeval*) {
    ntpSyncedPending = true;
  });
}

// wifi:fail code for the current state (called after a failed attempt).
const char* wifiFailCode() {
  if (!wifiSsid[0]) return "off";
  if (WiFi.status() == WL_CONNECTED &&
      WiFi.localIP() == INADDR_NONE) return "ip";   // up but no lease
  if (lastDisconnectReason == 201) return "ssid";
  return "auth";   // AUTH_EXPIRE / handshake timeouts / unknown: auth family
}

// Wi-Fi rejoin, used by the boot path, the poll tasks and BLE provisioning.
// The driver is NEVER de-initialized (no WIFI_OFF): the boot-time deinit
// wedged it ("timeout when WiFi un-init" -> esp_wifi_init 257 on the next
// STA enable), and it only freed ~14 KB anyway. So a rejoin is just
// begin(); if the driver is genuinely down (WL_NO_SHIELD), one full
// power-cycle is the last resort. No credentials -> never touch the radio.
bool wifiBootOffline = false;   // set when the boot attempt fails/skips
bool wifiWasUp = false;         // link edge detector (rejoin notify)
volatile bool wifiNotifyPending = false;   // task -> loop: send wifi:ok
SemaphoreHandle_t wifiJoinMutex = xSemaphoreCreateMutex();

void wifiRejoinUnlocked() {
  if (kNoRadioDemo || !wifiSsid[0]) return;
  hookWifiEvents();
  lastDisconnectReason = 0;
  WiFi.mode(WIFI_STA);
#if LOW_POWER_RADIO_TEST
  WiFi.setTxPower(WIFI_POWER_MINUS_1dBm);
#endif
  // Channels 12-13: the ESP32's default regulatory domain only scans 1-11, so
  // a mesh that auto-hops its 2.4 GHz band up there becomes invisible and
  // every join reports reason=201 (NO_AP_FOUND). Pin a 1-13 domain.
  static bool countrySet = false;
  if (!countrySet) {
    countrySet = true;
    wifi_country_t country = {"01", 1, 13, WIFI_COUNTRY_POLICY_MANUAL};
    esp_wifi_set_country(&country);
  }
  wl_status_t st = WiFi.begin(wifiSsid, wifiPass);
  // NB: esp_wifi_set_ps(WIFI_PS_NONE) here would ABORT the chip — the
  // coexistence config mandates modem sleep while Wi-Fi + BLE are both on.
  Serial0.printf("WiFi: begin status=%d\n", static_cast<int>(st));
  if (st == WL_NO_SHIELD) {   // driver not initialized / wedged
    Serial.println("WiFi: STA enable failed, full radio re-init");
    WiFi.mode(WIFI_OFF);
    delay(500);
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid, wifiPass);
  }
}

void wifiRejoin() {
  xSemaphoreTake(wifiJoinMutex, portMAX_DELAY);
  wifiRejoinUnlocked();
  xSemaphoreGive(wifiJoinMutex);
}

// Link edge detector, called from the poll tasks' connected branches: on a
// down->up transition, log + queue the wifi:ok notify (drained by loop(),
// which owns all BLE work).
void noteWifiLink() {
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !wifiWasUp) {
    wifiWasUp = true;
    wifiBootOffline = false;
    wifiNotifyPending = true;
    wifiOnlineState = true;    // icon update is drained by loop()
    wifiOnlinePending = true;
    Serial.printf("WiFi rejoined, IP=%s\n", WiFi.localIP().toString().c_str());
  } else if (!up && wifiWasUp) {
    wifiWasUp = false;
    wifiOnlineState = false;
    wifiOnlinePending = true;
  }
}

bool setupWiFiTime() {
  if (kNoRadioDemo) {
    wifiBootOffline = true;
    return false;
  }
  if (!wifiSsid[0]) {
    Serial.println("WiFi: no credentials, running offline.");
    wifiBootOffline = true;
    return false;
  }

  // A background poll must not call WiFi.begin() halfway through a user's
  // provisioning join and restart the WPA handshake.
  xSemaphoreTake(wifiJoinMutex, portMAX_DELAY);
  wifiRejoinUnlocked();
  Serial.print("WiFi connecting");

  // Chunked waits pump LVGL so the join spinner (showWifiSetup) animates
  // through what is otherwise a fully blocked loop task.
  auto joinWait = [](uint32_t sliceMs) {
    const uint32_t t0 = millis();
    while (millis() - t0 < sliceMs) { ui::pump(); delay(20); }
  };

  // Non-blocking-ish: give up after 10 s so the pet still runs offline.
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    joinWait(250);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("\nWiFi: join failed, status=%d reason=%d (%s)\n",
                  WiFi.status(), lastDisconnectReason,
                  wifiReasonStr(lastDisconnectReason));
    Serial0.printf("WiFi: join failed status=%d reason=%d (%s)\n",
                   WiFi.status(), lastDisconnectReason,
                   wifiReasonStr(lastDisconnectReason));
    // disconnect(false): drop the association but keep the driver UP —
    // disconnect(true)/WIFI_OFF wedge the deinit on this stack and every
    // later esp_wifi_init fails (NO_MEM), killing standalone sync.
    WiFi.disconnect(false);
    int16_t apCount = WiFi.scanNetworks();
    bool targetSeen = false;
    int strongestRssi = -1000;
    int32_t strongestChannel = 0;
    uint8_t strongestBssid[6] = {};
    for (int16_t i = 0; i < apCount; i++) {
      if (strcmp(WiFi.SSID(i).c_str(), wifiSsid) != 0) continue;
      targetSeen = true;
      int rssi = WiFi.RSSI(i);
      Serial0.printf("WiFi: target seen channel=%d rssi=%d encryption=%d\n",
                     WiFi.channel(i), rssi,
                     static_cast<int>(WiFi.encryptionType(i)));
      if (rssi > strongestRssi) {
        strongestRssi = rssi;
        strongestChannel = WiFi.channel(i);
        memcpy(strongestBssid, WiFi.BSSID(i), sizeof(strongestBssid));
      }
    }
    Serial0.printf("WiFi: scan aps=%d target_seen=%d heap=%u\n",
                   apCount, targetSeen ? 1 : 0, ESP.getFreeHeap());
    WiFi.scanDelete();

    // Mesh/multi-AP networks can leave the radio roaming through weak or
    // busy nodes until the WPA handshake expires. Retry the strongest AP.
    if (targetSeen) {
      lastDisconnectReason = 0;
      WiFi.mode(WIFI_STA);
      wl_status_t directed = WiFi.begin(wifiSsid, wifiPass, strongestChannel,
                                        strongestBssid);
      Serial0.printf("WiFi: directed begin channel=%d status=%d\n",
                     static_cast<int>(strongestChannel), static_cast<int>(directed));
      start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) joinWait(250);
    }
    // Last resort: some mesh APs ignore a first association burst while
    // band-steering. Drop the link, pause, and try once more without the
    // BSSID lock so any node of the mesh can answer.
    if (WiFi.status() != WL_CONNECTED) {
      Serial0.println("WiFi: third round (no BSSID lock)");
      WiFi.disconnect(false);
      delay(500);
      lastDisconnectReason = 0;
      WiFi.begin(wifiSsid, wifiPass);
      start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) joinWait(250);
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial0.printf("WiFi: final join failure status=%d reason=%d (%s)\n",
                     WiFi.status(), lastDisconnectReason,
                     wifiReasonStr(lastDisconnectReason));
      WiFi.disconnect(false);
      wifiBootOffline = true;
      xSemaphoreGive(wifiJoinMutex);
      return false;
    }
  }

  Serial.printf("\nWiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());
  wifiWasUp = true;   // edge detector starts "up" after a boot-time join

  configTzTime(TZ_POSIX, "pool.ntp.org", "time.nist.gov");

  // Wait briefly for the first NTP sync.
  struct tm ti;
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&ti, 200) && ti.tm_year > 120) {
      timeSynced = true;
      break;
    }
  }
  Serial.println(timeSynced ? "Time synced." : "NTP sync timed out.");
  xSemaphoreGive(wifiJoinMutex);
  return timeSynced;
}

// ---------------------------------------------------------------------------
// Day-based streak (persisted in NVS)
// ---------------------------------------------------------------------------

void loadStats() {
  prefs.begin("fina", true);
  streak = prefs.getUInt("streak", 0);
  points = prefs.getUInt("points", 0);
  happiness = prefs.getUChar("happy", 50);
  serverLinked = prefs.getUChar("srvlink", 0) != 0;
  pinSet = prefs.getBool("pinset", false);
  pinCode = prefs.getUShort("pin", 0);
  pinFails = prefs.getUChar("pinfail", 0);
  prefs.end();
  if (pinSet) pinArmLockout();   // a reboot must not reset the guess budget
}

// Recalculate the streak from the local date. Call after NTP sync and
// periodically; persists only when the day rolls over.
void updateStreakFromTime() {
  if (!timeSynced) return;

  struct tm ti;
  if (!getLocalTime(&ti, 0)) return;

  // Monotonic-ish day index for day-boundary comparison.
  uint32_t today = static_cast<uint32_t>(ti.tm_year + 1900) * 400 + static_cast<uint32_t>(ti.tm_yday);

  prefs.begin("fina", false);
  uint32_t lastDay = prefs.getUInt("lastDay", 0);
  uint32_t s = prefs.getUInt("streak", 0);

  if (lastDay == 0)           s = 1;    // first ever day
  else if (today == lastDay)  {}        // same day, no change
  else if (today == lastDay + 1) s++;   // consecutive day
  else                        s = 1;    // streak broken

  if (today != lastDay) prefs.putUInt("lastDay", today);
  if (s != prefs.getUInt("streak", 0)) prefs.putUInt("streak", s);
  prefs.end();

  if (s != streak) {
    streak = s;
    Serial.printf("Streak -> %lu\n", static_cast<unsigned long>(streak));
    blePushState(pet.state());
  }
}

// ---------------------------------------------------------------------------
// Standalone DCA poll (BLE disconnected)
//
// While no app is connected, a low-priority task refreshes the plan slots
// from the read-only relay every 30 min:
//   GET http://<RELAY_HOST>/api/device/<DEVICE_ID>/dca
//   -> CSV "TICKER,amount,next_epoch,buys,holdings;..." (see README.md)
// CSV is parsed in place (strtok_r on a stack buffer — no String churn).
// Failures (Wi-Fi down, timeout, malformed CSV) are silent: the last cached
// state stays on screen and the next cycle retries. The relay is read-only;
// the device never calls Titan or any signing API.
//
// NOTE: the relay poll is DISABLED by default (RELAY_POLL_ENABLED 0) — the
// current backend does not implement this route. The price feed below runs
// regardless and is the live part of the 30-min cycle.
// ---------------------------------------------------------------------------

constexpr uint32_t DCA_POLL_MS = 30UL * 60UL * 1000UL;   // 30 min
#ifndef DCA_FIRST_POLL_MS
#define DCA_FIRST_POLL_MS 60000      // first poll 1 min after boot (sim: less)
#endif

// Toasts stashed by the poll task, drained to ui::enqueueToast on the loop
// task (same pattern as cmdBuf: all pet access happens on the loop task).
char toastStash[3][24];
volatile uint8_t toastHead = 0;
volatile uint8_t toastTail = 0;
portMUX_TYPE toastMux = portMUX_INITIALIZER_UNLOCKED;

void stashToast(const char* text) {
  portENTER_CRITICAL(&toastMux);
  if (static_cast<uint8_t>(toastHead - toastTail) < 3) {
    strlcpy(toastStash[toastHead % 3], text, sizeof(toastStash[0]));
    toastHead++;
  }
  portEXIT_CRITICAL(&toastMux);
}

bool pollDcaRelay() {
  if (WiFi.status() != WL_CONNECTED) return false;

  char url[128];
  snprintf(url, sizeof(url), "http://%s/api/device/%s/dca", RELAY_HOST, DEVICE_ID);

  HTTPClient http;
  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("DCA poll: HTTP %d, keeping cache\n", code);
    http.end();
    return false;
  }
  char csv[512];
  int n = http.getStreamPtr()->readBytes(csv, sizeof(csv) - 1);
  csv[n > 0 ? n : 0] = 0;
  http.end();

  bool any = false;
  time_t nowT = time(nullptr);
  char* outer;
  for (char* ent = strtok_r(csv, ";", &outer); ent; ent = strtok_r(nullptr, ";", &outer)) {
    char* inner;
    char* tick   = strtok_r(ent, ",", &inner);
    char* amtS   = strtok_r(nullptr, ",", &inner);
    char* epS    = strtok_r(nullptr, ",", &inner);
    char* buyS   = strtok_r(nullptr, ",", &inner);
    char* holdS  = strtok_r(nullptr, ",", &inner);
    char* priceS = strtok_r(nullptr, ",", &inner);   // optional 6th column
    if (!tick || !amtS || !epS || !buyS || !holdS) continue;   // malformed entry

    for (size_t i = 0; i < dcaCount; i++) {
      if (!plans[i].enabled || strcmp(plans[i].ticker, tick) != 0) continue;
      uint32_t buys = static_cast<uint32_t>(strtoul(buyS, nullptr, 10));
      uint32_t oldBuys = plans[i].buys;
      plans[i].nextBuyEpoch  = static_cast<uint32_t>(strtoul(epS, nullptr, 10));
      plans[i].holdingsHeld  = strtof(holdS, nullptr);   // fractional tokens
      plans[i].buys = buys;
      if (priceS) plans[i].priceUsd = strtof(priceS, nullptr);

      if (buys > oldBuys) {
        // A buy landed while offline: "+<delta*amount> TICKER" (delta cap 9).
        uint32_t delta = buys - oldBuys;
        if (delta > 9) delta = 9;
        char t[24];
        snprintf(t, sizeof(t), "+%.3g %s",
                 static_cast<double>(delta * plans[i].amountSol), plans[i].ticker);
        stashToast(t);
        planOverdue[i] = false;
      } else if (timeSynced && nowT > 0 && plans[i].nextBuyEpoch > 0 &&
                 static_cast<uint32_t>(nowT) >= plans[i].nextBuyEpoch) {
        planOverdue[i] = true;   // past due and no new buys since last poll
      }
      any = true;
    }
  }
  if (any) plansDirty = true;   // loop task: save NVS + push to pet

  // Re-sync the wall clock after every successful poll.
  configTzTime(TZ_POSIX, "pool.ntp.org", "time.nist.gov");
  struct tm ti;
  if (getLocalTime(&ti, 100) && ti.tm_year > 120) timeSynced = true;
  Serial.printf("DCA poll: %d bytes, %s\n", n, any ? "plans updated" : "no matching plans");
  return true;
}

// Polls only while the app is disconnected; when connected the app is
// authoritative and polls stay paused. BTN1 sets syncRequested for an
// immediate manual sync (same relay + price path) — the 500 ms wake keeps
// that responsive while the 30 min cadence is untouched. The render loop on
// the loop task is never blocked. wifiRejoin() is a no-op without
// credentials, so an unprovisioned device never touches the radio.
void dcaPollTask(void*) {
  bool first = true;
  uint32_t lastPoll = millis();
  for (;;) {
    // OTA manifest check (menu row / BLE ota:start): runs here, off the
    // loop task, so the UI keeps animating and accepting input meanwhile.
    if (otaCheckRequested) otaCheckFromTask();
    if (syncRequested) {
      syncRequested = false;
      if (!appConnected) {
        if (WiFi.status() == WL_CONNECTED) {
          noteWifiLink();
          bool ok = true;
          if (RELAY_POLL_ENABLED) ok = pollDcaRelay();
          fetchPrices();
          stashToast(ok ? "synced" : "sync failed");
        } else if (!wifiSsid[0]) {
          stashToast("no wifi credentials");
        } else {
          // Rejoin with the current credentials; user can press again.
          wifiRejoin();
          stashToast("wifi joining...");
        }
        lastPoll = millis();   // don't let the auto cadence fire right after
        first = false;
      }
    } else if (!appConnected) {
      uint32_t wait = first ? DCA_FIRST_POLL_MS : DCA_POLL_MS;
      if (millis() - lastPoll >= wait) {
        lastPoll = millis();
        first = false;
        if (WiFi.status() == WL_CONNECTED) {
          noteWifiLink();
          if (RELAY_POLL_ENABLED) {
            pollDcaRelay();   // also re-syncs the wall clock on success
          } else {
            // The relay poll used to re-sync NTP every cycle; keep the
            // cadence now that the relay is disabled.
            configTzTime(TZ_POSIX, "pool.ntp.org", "time.nist.gov");
          }
          fetchPrices();
        } else {
          // Silent retry: rejoin with the current credentials, poll next cycle.
          wifiRejoin();
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ---------------------------------------------------------------------------
// On-device price feed (standalone mode): real prices straight from public
// HTTP APIs — no relay needed. Primary source is the Jupiter Price API v3
// (real on-chain DEX prices, 24/7 — the issuer's indicative quote is null
// while the stock market is closed): one batched call for SOL + every plan.
// Unknown xStock tickers fall back to the issuer API
// (/public/assets/<symbol>/price-data -> {"quote": <number|null>}).
// HTTPS validated against each provider's pinned roots (price_root_ca.h):
// the data is public, but it drives the portfolio values on screen, so an
// unverified context let any on-path attacker fake them. Failures keep the
// last cached price.
// solUsdRate / priceDirty are declared with the plan storage above.
// ---------------------------------------------------------------------------

constexpr const char* SOL_MINT = "So11111111111111111111111111111111111111112";

// Solana mints for the xStocks the device ships logos for (from
// https://api.xstocks.fi/api/v2/public/assets/<symbol> -> deployments).
struct XStockMint { const char* ticker; const char* mint; };
const XStockMint XSTOCK_MINTS[] = {
  { "SPYX",   "XsoCS1TfEyfFhfvj8EtZ528L3CaKBDBRqRapnBbDF2W" },
  { "GOOGLX", "XsCPL9dNWBMvFtTmwcCA5v3xWPSMEBCszbQdiLLq6aN" },
  { "AAPLX",  "XsbEhLAtcf6HdfpFZ5xEMdqW8nfAvcsP5bdudRLJzJp" },
  { "TSLAX",  "XsDoVfqeBukxuZHWhdvWHBhgEHjGNst4MLodqsJHzoB" },
  { "NVDAX",  "Xsc9qvGR1efVDFGLrVsmkzv3qi45LTBjeUKSPmx9qEh" },
  { "METAX",  "Xsa62P5mvPszXL1krVUnU5ar38bBSVcWAB6fmPCo5Zu" },
  { "MSFTX",  "XspzcW1PRtgf6Wj92HCiZdjzKCyFekVD8P5Ueh3dRMX" },
  { "AMZNX",  "Xs3eBt7uRfJX8QUs4suhyU8p2M6DoUDrJyWBa8LLZsg" },
  { "MSTRX",  "XsP7xzNPvEHS1m6qfanPUGjNmdnmsLKEoNAnHjdxxyZ" },
  { "CRCLX",  "XsueG8BtpquVJX9LVLLEGuViXUungE6WmK5YZ3p3bd1" },
  { "NFLXX",  "XsEH7wWfJJu2ZT3UCFeVfALnVA6CP5ur7Ee11KmzVpL" },
  { "COINX",  "Xs7ZdzSHLU9ftNJsii5fCeJhoRWSC32SQGzGQtePxNu" },
  { "HOODX",  "XsvNBAYkrDRNhA7wPHQfX3ZUXZyZLdnCQDfHZ56bzpg" },
};

const char* mintFor(const char* ticker) {
  for (const auto& m : XSTOCK_MINTS)
    if (strcmp(ticker, m.ticker) == 0) return m.mint;
  return nullptr;
}

// GET a small JSON body into buf. Returns bytes read, 0 on any failure.
// One retry: the first TLS/DNS attempt occasionally fails on fresh Wi-Fi.
// `auth` (optional) is sent as the Authorization header value; `codeOut`
// (optional) receives the last HTTP status. A 401 is never retried.
// `rootCa` pins the trust anchor and is REQUIRED: every request is
// validated (never setInsecure). A null CA is a programming error and the
// request is refused rather than silently downgraded.
size_t httpGetJson(const char* url, char* buf, size_t len,
                   const char* auth, int* codeOut, const char* rootCa) {
  if (codeOut) *codeOut = 0;
  buf[0] = 0;
  if (!rootCa || strncmp(url, "https://", 8) != 0) return 0;
  for (int attempt = 0; attempt < 2; attempt++) {
    WiFiClientSecure client;           // task-local: fresh TLS session
    client.setCACert(rootCa);
    client.setHandshakeTimeout(10);    // seconds; a stalled AP must not hang the task
    HTTPClient http;
    http.setTimeout(5000);
    http.setReuse(false);
    // Never follow redirects: a 3xx could bounce a Bearer token or the
    // firmware URL to another host.
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(client, url)) return 0;
    if (auth) http.addHeader("Authorization", auth);
    int code = http.GET();
    if (codeOut) *codeOut = code;
    size_t n = 0;
    if (code == HTTP_CODE_OK)
      n = http.getStreamPtr()->readBytes(reinterpret_cast<uint8_t*>(buf), len - 1);
    http.end();
    if (n > 0) {
      buf[n] = 0;
      return n;
    }
    if (code == HTTP_CODE_UNAUTHORIZED) break;   // bad token: retrying won't help
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  buf[0] = 0;
  return 0;
}

// Extract the number after a flat JSON key ("usdPrice": / "quote":) at or
// after `from`; 0 when the key is absent or explicitly null.
float jsonNumber(const char* from, const char* key) {
  const char* p = strstr(from, key);
  if (!p) return 0.0f;
  p += strlen(key);
  while (*p == ' ') p++;
  if (strncmp(p, "null", 4) == 0) return 0.0f;
  return strtof(p, nullptr);
}

// Extract the quoted string after a flat JSON key ("mood":) into out.
// Returns false when the key is absent or not a string.
bool jsonString(const char* from, const char* key, char* out, size_t len) {
  const char* p = strstr(from, key);
  if (!p) return false;
  p += strlen(key);
  while (*p == ' ') p++;
  if (*p != '"') return false;
  const char* e = strchr(++p, '"');
  if (!e) return false;
  size_t n = static_cast<size_t>(e - p);
  if (n >= len) n = len - 1;
  memcpy(out, p, n);
  out[n] = 0;
  return true;
}

void fetchPrices() {
  if (WiFi.status() != WL_CONNECTED) return;

  // One batched Jupiter call: SOL + every enabled plan with a known mint.
  // (api.jup.ag, not lite-api.jup.ag — lite is IPv6-only, ESP32 is IPv4.)
  // Slots the app priced itself (8th dca:plan field) are skipped — the
  // app's quote is fresher; self-fetch is the fallback for the rest.
  char url[384];
  int n = snprintf(url, sizeof(url), "https://api.jup.ag/price/v3?ids=%s", SOL_MINT);
  size_t idx[kDcaMaxPlans], ni = 0;   // plan slots included in the call
  for (size_t i = 0; i < dcaCount; i++) {
    if (!plans[i].enabled || appPrice[i]) continue;
    const char* mint = mintFor(plans[i].ticker);
    if (!mint) continue;
    n += snprintf(url + n, sizeof(url) - static_cast<size_t>(n), ",%s", mint);
    idx[ni++] = i;
  }

  static char body[3072];   // static: keep the poll task stack small
  if (httpGetJson(url, body, sizeof(body), nullptr, nullptr, kJupiterRootCa) > 0) {
    const char* sol = strstr(body, SOL_MINT);
    float rate = sol ? jsonNumber(sol, "\"usdPrice\":") : 0.0f;
    if (rate > 0.0f) {
      solUsdRate = rate;
      priceDirty = true;
      Serial.printf("Price feed: SOL/USD=%.2f\n", static_cast<double>(rate));
    }
    for (size_t k = 0; k < ni; k++) {
      const char* e = strstr(body, mintFor(plans[idx[k]].ticker));
      float p = e ? jsonNumber(e, "\"usdPrice\":") : 0.0f;
      if (p > 0.0f) {
        plans[idx[k]].priceUsd = p;
        plansDirty = true;
        Serial.printf("Price feed: %s=$%.2f\n", plans[idx[k]].ticker,
                      static_cast<double>(p));
      }
    }
  }

  // Fallback for enabled xStock tickers without a known mint: the issuer's
  // indicative quote (null while the market is closed -> keep cached).
  for (size_t i = 0; i < dcaCount; i++) {
    if (!plans[i].enabled || appPrice[i] || mintFor(plans[i].ticker)) continue;
    size_t tl = strlen(plans[i].ticker);
    if (tl < 2 || plans[i].ticker[tl - 1] != 'X') continue;   // not an xStock
    char sym[8];
    strlcpy(sym, plans[i].ticker, sizeof(sym));
    sym[tl - 1] = 'x';                      // SPYX -> SPYx (canonical case)
    char qurl[96];
    snprintf(qurl, sizeof(qurl),
             "https://api.xstocks.fi/api/v2/public/assets/%s/price-data", sym);
    char qbody[192];
    if (httpGetJson(qurl, qbody, sizeof(qbody), nullptr, nullptr, kXStocksRootCa) == 0)
      continue;
    float quote = jsonNumber(qbody, "\"quote\":");
    if (quote > 0.0f) {
      plans[i].priceUsd = quote;
      plansDirty = true;
      Serial.printf("Price feed: %s=$%.2f (issuer)\n", plans[i].ticker,
                    static_cast<double>(quote));
    }
  }
}

// Overdue plans nudge the mood (additive, disconnected only — never override
// an app-pushed mood while connected): >=1 overdue -> waiting, >=2 -> sad.
bool dcaMoodNudged = false;
PetMoodId moodBeforeNudge = PetMoodId::MOOD_CALM;

void applyDcaMoodNudge(float nowSec) {
  if (appConnected) {          // the app owns the mood; drop any stale nudge
    dcaMoodNudged = false;
    return;
  }
  size_t overdue = 0;
  for (size_t i = 0; i < dcaCount; i++)
    if (plans[i].enabled && planOverdue[i]) overdue++;

  if (overdue > 0) {
    PetMoodId target = overdue >= 2 ? PetMoodId::MOOD_SAD : PetMoodId::MOOD_WAITING;
    if (!dcaMoodNudged) { moodBeforeNudge = pet.mood(); dcaMoodNudged = true; }
    pet.setMood(target, nowSec);
  } else if (dcaMoodNudged) {
    dcaMoodNudged = false;
    pet.setMood(moodBeforeNudge, nowSec);   // resolved: revert
  }
}

// ---------------------------------------------------------------------------
// Cloud state sync (standalone mode)
//
// Devices provisioned with a device token (optional 3rd provisioning field)
// poll the companion API for authoritative pet state while no app is
// connected:
//   GET <STATE_SYNC_URL>   (default https://api.finagotchi.app/device/state)
//   Authorization: Bearer <deviceToken>
//   -> {"stage":4,"sub":4,"streak":7,"mood":"happy","points":120,"happy":80,
//       "upd":1726000000}
// Parsed values are stashed under stateMux and applied on the loop task via
// applySnapshot (same code path as the BLE snapshot write). After 5
// consecutive 401s the token is assumed dead and syncing stops until reboot.
// ---------------------------------------------------------------------------

constexpr uint32_t STATE_SYNC_MS = 60000;          // poll cadence
constexpr uint32_t STATE_SYNC_FIRST_MS = 20000;    // first poll after boot
constexpr uint8_t  STATE_SYNC_MAX_401 = 5;         // then give up until reboot

struct CloudState {
  int      stage;
  int      sub;
  uint32_t streak;
  int      mood;      // -1 = absent/unknown label: keep the current mood
  uint32_t points;
  uint8_t  happy;
  uint32_t upd;       // server row update time (unix s); 0 = no state row
};
CloudState     cloudStash = {};
volatile bool  stateDirty = false;       // sync task -> loop task handoff
portMUX_TYPE   stateMux = portMUX_INITIALIZER_UNLOCKED;
bool           lastSyncOk = false;
// Staleness guard (sync-task local, RAM-only): the server stamps every pet
// state row with `upd`; responses at or below the last applied revision are
// ignored so a delayed/older poll can never regress the pet. Reset on reboot
// is fine — re-applying the latest server state is idempotent.
uint32_t       lastAppliedUpd = 0;

// One poll. Returns the HTTP status code (0 = transport failure).
int syncStateFromServer() {
  // Validated TLS needs a plausible wall clock (cert validity window). On
  // networks where NTP has not landed yet, skip this cycle — the next poll
  // retries, and the app's epoch: fallback also unsticks the clock.
  if (time(nullptr) < 1700000000) {   // before Nov 2023 = unset clock
    Serial.println("State sync: clock not synced yet, skipping");
    return 0;
  }
  char auth[192];
  snprintf(auth, sizeof(auth), "Bearer %s", deviceToken);
  static char body[512];   // static: keep the sync task stack small
  int code = 0;
  // The Bearer token rides on this request, so unlike the anonymous price
  // feeds the TLS context is validated against the pinned API root CA.
  size_t n = httpGetJson(STATE_SYNC_URL, body, sizeof(body), auth, &code,
                         kApiRootCa);
  if (n == 0) {
    lastSyncOk = false;
    Serial.printf("State sync: failed (HTTP %d)\n", code);
    return code;
  }

  CloudState cs;
  cs.stage   = static_cast<int>(jsonNumber(body, "\"stage\":"));
  cs.sub     = static_cast<int>(jsonNumber(body, "\"sub\":"));
  cs.streak  = static_cast<uint32_t>(jsonNumber(body, "\"streak\":"));
  cs.points  = static_cast<uint32_t>(jsonNumber(body, "\"points\":"));
  cs.happy   = static_cast<uint8_t>(jsonNumber(body, "\"happy\":"));
  cs.upd     = static_cast<uint32_t>(jsonNumber(body, "\"upd\":"));
  // A server that omits "upd" entirely (legacy/self-hosted) gets the old
  // apply-every-poll behavior; the guards below only engage when the field
  // is present (the production API always sends it).
  const bool hasUpd = strstr(body, "\"upd\":") != nullptr;
  char moodName[12] = "";
  jsonString(body, "\"mood\":", moodName, sizeof(moodName));
  // The server stores the APP's mood labels, which diverge from the
  // firmware's: "proud" means happy, "sleeping" means sleepy. An empty or
  // unknown label means "no opinion" — keep the device's current mood
  // instead of clobbering it.
  if (strcmp(moodName, "proud") == 0) strcpy(moodName, "happy");
  else if (strcmp(moodName, "sleeping") == 0) strcpy(moodName, "sleepy");
  cs.mood = -1;
  for (size_t i = 0; i < kMoodCount; i++) {
    if (strcmp(moodName, MOOD_NAMES[i]) == 0) {
      cs.mood = static_cast<int>(i);
      break;
    }
  }

  if (cs.stage < 1 || cs.stage > 12) {
    lastSyncOk = false;
    Serial.printf("State sync: bad stage %d, ignoring\n", cs.stage);
    return code;
  }
  if (cs.happy > 100) cs.happy = 100;
  if (hasUpd && cs.upd == 0) {
    // The server has no pet-state row yet: the response carries defaults
    // (stage 1, happy 80) that must NOT be applied — they would reset the
    // pet to a fresh egg. Counts as success so the 401 counter resets.
    lastSyncOk = true;
    Serial.println("State sync: no server state yet, ignoring");
    return code;
  }
  if (hasUpd && cs.upd <= lastAppliedUpd) {
    // Already applied this revision, or a delayed older response.
    lastSyncOk = true;
    return code;
  }

  portENTER_CRITICAL(&stateMux);
  cloudStash = cs;
  portEXIT_CRITICAL(&stateMux);
  stateDirty = true;
  if (hasUpd) lastAppliedUpd = cs.upd;
  lastSyncOk = true;
  Serial.printf("State sync: stage=%d sub=%d streak=%lu upd=%lu\n", cs.stage, cs.sub,
                static_cast<unsigned long>(cs.streak), static_cast<unsigned long>(cs.upd));
  return code;
}

// Polls only while the app is disconnected (same discipline as dcaPollTask);
// rejoins Wi-Fi silently when the link dropped. 10 s idle tick.
void stateSyncTask(void*) {
  uint32_t lastSync = millis();
  uint8_t authFails = 0;
  bool disabled = false;
  bool first = true;
  for (;;) {
    if (!disabled && deviceToken[0] != '\0' && !appConnected) {
      uint32_t wait = first ? STATE_SYNC_FIRST_MS : STATE_SYNC_MS;
      if (millis() - lastSync >= wait) {
        lastSync = millis();
        first = false;
        if (WiFi.status() == WL_CONNECTED) {
          noteWifiLink();
          int code = syncStateFromServer();
          if (code == HTTP_CODE_UNAUTHORIZED) {
            if (++authFails >= STATE_SYNC_MAX_401) {
              disabled = true;   // bad token: stop hammering until reboot
              Serial.println("State sync: 5 consecutive 401s, sync disabled until reboot");
            }
          } else if (code == HTTP_CODE_OK) {
            authFails = 0;
          }
        } else {
          // Silent retry: rejoin with the current credentials, poll next cycle.
          wifiRejoin();
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}

// ---------------------------------------------------------------------------
// OTA firmware update check (menu "Update" row / BLE ota:start)
//
// Flow (OTA.md §4): guard (battery, Wi-Fi) -> GET the manifest over the
// pinned-CA TLS path -> 204 = "up to date" toast; newer = confirm screen.
// A confirmed update is stashed in NVS and the device REBOOTS into OTA mode
// (ota.cpp), which downloads with BLE/LVGL/pet down — the N8 build idles
// with ~19 KB free heap, no room for a TLS handshake next to the live UI.
// The check runs on the loop task (blocking, user-triggered) — same
// discipline as the BLE provisioning reconnect.
// ---------------------------------------------------------------------------

// env param for the manifest request: the API serves per-hardware builds.
#ifdef FINAGOTCHI_PCB_V1
constexpr const char* OTA_ENV = "pcb-v1";
#else
constexpr const char* OTA_ENV = "esp32-s3";
#endif

constexpr uint32_t OTA_SLOT_SIZE = 0x380000;   // ota_0/ota_1, partitions_8mb_ota.csv
constexpr int OTA_MIN_BATTERY_PCT = 40;

// The manifest that passed validation, stashed until confirm/cancel.
struct OtaManifest {
  char version[16];
  char url[224];
  char sha256[65];
  uint32_t size;
};
OtaManifest otaManifest;
volatile bool otaCheckBusy = false;        // a check is in flight (debounce)

// Check outcome, produced on the poll task and applied on the loop task
// (UI + BLE notify are loop-only). otaResultPending is set LAST, after the
// manifest/status fields are written.
enum class OtaResult : uint8_t { Current, Available, Battery, Wifi, Http, Manifest };
volatile OtaResult otaResult = OtaResult::Current;
volatile bool otaResultPending = false;
volatile bool otaCheckingNotify = false;   // poll task -> loop: ota:status:checking

void actionCheckUpdate() {
  if (otaCheckBusy || otaCheckRequested) {
    ui::enqueueToast("already checking...");
    return;
  }
  otaCheckRequested = true;
  ui::showOverlay("Checking for update...", 20000);
  Serial.println("OTA: check requested");
}

// Runs on the poll task (TLS-sized 8 KB stack): the old loop-task version
// froze buttons, BLE commands and every animation for up to ~20 s while it
// joined Wi-Fi and fetched the manifest. No UI/BLE calls here.
OtaResult runOtaCheck() {
  // Battery guard: a brownout mid-flash is what the second slot exists for,
  // but don't invite it. (batteryPercentNow returns 100 on USB power.)
  int batt = batteryPercentNow();
  if (batt < OTA_MIN_BATTERY_PCT) {
    Serial.printf("OTA: refused, battery %d%% (need %d%%)\n", batt, OTA_MIN_BATTERY_PCT);
    return OtaResult::Battery;
  }
  otaCheckingNotify = true;

  if (WiFi.status() != WL_CONNECTED && wifiSsid[0]) {
    wifiRejoin();   // non-blocking begin; wait here (this task only)
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000)
      vTaskDelay(pdMS_TO_TICKS(250));
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("OTA: no wifi, aborting check");
    return OtaResult::Wifi;
  }
  // Pinned-CA validation needs a plausible clock (cert validity window).
  if (time(nullptr) < 1700000000) {
    configTzTime(TZ_POSIX, "pool.ntp.org", "time.nist.gov");
    uint32_t t0 = millis();
    while (time(nullptr) < 1700000000 && millis() - t0 < 8000)
      vTaskDelay(pdMS_TO_TICKS(200));
    if (time(nullptr) < 1700000000) {
      Serial.println("OTA: clock not synced, TLS would fail");
      return OtaResult::Wifi;
    }
  }

  char url[256];
  snprintf(url, sizeof(url), "%s?env=%s&v=%s", FIRMWARE_MANIFEST_URL, OTA_ENV, FW_VERSION);
  static char body[768];   // static: keep the task stack small
  int code = 0;
  // Anonymous GET, but over the pinned ISRG Root X1 path (never insecure).
  size_t n = httpGetJson(url, body, sizeof(body), nullptr, &code, kApiRootCa);
  if (code == HTTP_CODE_NO_CONTENT) {   // 204: device is current
    Serial.printf("OTA: firmware %s is current\n", FW_VERSION);
    return OtaResult::Current;
  }
  if (n == 0) {
    Serial.printf("OTA: manifest fetch failed (HTTP %d)\n", code);
    return OtaResult::Http;
  }

  char ver[16], furl[sizeof(otaManifest.url)], sha[65];
  double sizeD = static_cast<double>(jsonNumber(body, "\"size\":"));
  bool ok = jsonString(body, "\"version\":", ver, sizeof(ver)) &&
            jsonString(body, "\"url\":", furl, sizeof(furl)) &&
            jsonString(body, "\"sha256\":", sha, sizeof(sha)) &&
            sizeD > 0.0 && sizeD <= static_cast<double>(OTA_SLOT_SIZE);
  // The image must come over HTTPS (OTA mode pins the CA; a plain-http url
  // would be refused there anyway — reject it up front).
  if (ok && strncmp(furl, "https://", 8) != 0) ok = false;
  // Version is shown on screen and sent over BLE: digits/dots only.
  if (ok) {
    if (!ver[0]) ok = false;
    for (const char* v = ver; ok && *v; v++)
      if (!isdigit(static_cast<uint8_t>(*v)) && *v != '.') ok = false;
  }
  if (ok) {
    // sha256 must be exactly 64 hex chars — anything else is a
    // placeholder/corrupt manifest; never schedule an unverifiable image.
    if (strlen(sha) != 64) ok = false;
    else for (size_t i = 0; i < 64; i++)
      if (!isxdigit(static_cast<uint8_t>(sha[i]))) { ok = false; break; }
  }
  if (!ok) {
    Serial.println("OTA: manifest rejected (bad version/url/sha256/size)");
    return OtaResult::Manifest;
  }
  if (strcmp(ver, FW_VERSION) == 0) return OtaResult::Current;   // 200 for the same build

  strlcpy(otaManifest.version, ver, sizeof(otaManifest.version));
  strlcpy(otaManifest.url, furl, sizeof(otaManifest.url));
  strlcpy(otaManifest.sha256, sha, sizeof(otaManifest.sha256));
  otaManifest.size = static_cast<uint32_t>(sizeD);
  Serial.printf("OTA: update available: %s (%lu bytes)\n",
                ver, static_cast<unsigned long>(otaManifest.size));
  return OtaResult::Available;
}

// Poll-task wrapper: run the check, hand the result to the loop.
void otaCheckFromTask() {
  otaCheckRequested = false;
  otaCheckBusy = true;
  OtaResult r = runOtaCheck();
  otaResult = r;
  otaResultPending = true;
  otaCheckBusy = false;
}

// Loop task: show the outcome + notify the app.
void applyOtaResult() {
  otaResultPending = false;
  ui::hideOverlay();   // retire the "checking" overlay
  switch (otaResult) {
    case OtaResult::Current:
      ui::enqueueToast("up to date");
      bleNotifyOta("current");
      break;
    case OtaResult::Battery:
      ui::showOverlay("Battery too low\ncharge to 40% first", 3000);
      bleNotifyOta("error:battery");
      break;
    case OtaResult::Wifi:
      ui::showOverlay("No Wi-Fi\nset it up in the app", 3000);
      bleNotifyOta("error:wifi");
      break;
    case OtaResult::Http:
      ui::showOverlay("Update check failed\ntry again later", 3000);
      bleNotifyOta("error:http");
      break;
    case OtaResult::Manifest:
      ui::showOverlay("Update unavailable\ntry again later", 3000);
      bleNotifyOta("error:manifest");
      break;
    case OtaResult::Available: {
      char st[32];
      snprintf(st, sizeof(st), "available:%s", otaManifest.version);
      bleNotifyOta(st);
      ui::showOtaConfirm(otaManifest.version, otaManifest.size);
      break;
    }
  }
}

// Confirm screen, push: notify the app, stash the manifest in NVS and
// reboot into OTA mode (ota.cpp runs the download from there).
void actionOtaConfirmed() {
  char st[32];
  snprintf(st, sizeof(st), "scheduled:%s", otaManifest.version);
  bleNotifyOta(st);
  ui::showOverlay("Restarting to update...", 5000);
  for (int i = 0; i < 10; i++) { ui::pump(); delay(20); }   // let it render
  ota::schedule(otaManifest.url, otaManifest.sha256, otaManifest.size,
                otaManifest.version);   // never returns
}

void actionOtaCancelled() {
  bleNotifyOta("idle");
  Serial.println("OTA: update aborted by user");
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

// Demo timing
constexpr uint32_t FRAME_MS  = 33;      // ~30 fps target
constexpr uint32_t EVOLVE_MS = 8000;    // lifecycle stage duration
constexpr uint32_t STREAK_CHECK_MS = 60000;  // re-check the day every minute

// AFK nap (ui::afkActive): scene slows to a breathing cadence and the
// backlight drops — the panel backlight is most of the battery budget.
constexpr uint32_t AFK_FRAME_MS = 400;

#ifndef AFK_BL_DUTY
#define AFK_BL_DUTY 28          // ~11% of full brightness (8-bit duty)
#endif

uint32_t lastFrame = 0;
uint32_t lastEvolve = 0;
uint32_t lastStreakCheck = 0;

// Backlight on a PWM channel so AFK can dim instead of only on/off.
// Channel 6: the buzzer owns ledc channel 0 (feedback.cpp).
constexpr uint8_t BL_LEDC_CH = 6;

void backlightSet(uint8_t duty) {
#ifdef TFT_BL
  static bool pwmInit = false;
  if (!pwmInit) {
    ledcSetup(BL_LEDC_CH, 5000, 8);
    ledcAttachPin(TFT_BL, BL_LEDC_CH);
    pwmInit = true;
  }
  ledcWrite(BL_LEDC_CH, TFT_BACKLIGHT_ON ? duty : 255 - duty);
#endif
}

void showSplash(uint32_t durationMs) {
  const uint16_t navy = tft.color565(0x0B, 0x28, 0x58);
  const uint16_t text = tft.color565(0xF6, 0xFA, 0xFF);
  const uint16_t accent = tft.color565(0x8D, 0xC9, 0xF6);
  tft.fillScreen(navy);

  // The logo array holds native RGB565; swapBytes puts the bytes in SPI
  // wire order for the push (the panel wants the high byte first).
  int16_t x = (tft.width() - FINAGOTCHI_LOGO_WIDTH) / 2;
  int16_t y = (tft.height() - FINAGOTCHI_LOGO_HEIGHT) / 2 - 24;
  tft.setSwapBytes(true);
  tft.pushImage(x, y, FINAGOTCHI_LOGO_WIDTH, FINAGOTCHI_LOGO_HEIGHT, finagotchi_logo);
  tft.setSwapBytes(false);

  tft.setTextDatum(TC_DATUM);
  tft.setTextFont(4);
  tft.setTextSize(1);
  tft.setTextColor(text, navy);
  const char* wordmark = "finagotchi.";
  const int16_t wordY = tft.height() - 66;
  const int16_t centerX = tft.width() / 2;
  tft.drawString(wordmark, centerX, wordY);
  const int16_t wordWidth = tft.textWidth(wordmark);
  const int16_t dotWidth = tft.textWidth(".");
  tft.setTextColor(accent, navy);
  tft.drawString(".", centerX + (wordWidth - dotWidth) / 2, wordY);

  delay(durationMs);
}

void showBootStatus(const char* msg) {
  const uint16_t navy = tft.color565(0x0B, 0x28, 0x58);
  tft.setTextDatum(TC_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(tft.color565(0xE0, 0xEB, 0xF8), navy);
  tft.setTextSize(1);
  tft.drawString(msg, tft.width() / 2, tft.height() - 16);
}

// --- OTA rollback (Tier 2, OTA.md §5) ----------------------------------------
// The core's bootloader is built with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE,
// but initArduino() marks a freshly flashed OTA image valid before setup()
// even runs — so an image that boot-loops or wedges was never rolled back.
// Claiming verification here keeps the new image PENDING_VERIFY until the
// self-test below passes; any reset before that (crash, watchdog, brownout)
// makes the bootloader fall back to the previous slot.
constexpr uint32_t OTA_SELFTEST_MS = 30000;   // healthy uptime that counts as "works"
bool otaVerifyPending = false;

void otaSelfTestBegin() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  otaVerifyPending = running &&
      esp_ota_get_state_partition(running, &st) == ESP_OK &&
      st == ESP_OTA_IMG_PENDING_VERIFY;
  if (otaVerifyPending)
    Serial.printf("OTA: new image on %s pending self-test (%lu s)\n", running->label,
                  static_cast<unsigned long>(OTA_SELFTEST_MS / 1000));
}

// Called every loop pass: the UI is up, the loop is spinning and the heap
// is sane for OTA_SELFTEST_MS -> keep this image.
void otaSelfTestPoll() {
  if (!otaVerifyPending || millis() < OTA_SELFTEST_MS) return;
  if (ESP.getFreeHeap() < 8 * 1024) return;   // starving: let the WDT decide
  otaVerifyPending = false;
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  Serial.printf("OTA: self-test passed, image marked valid (%s)\n", esp_err_to_name(err));
}

} // namespace

// Weak hook in the core (esp32-hal-misc.c, C linkage): true = the sketch
// validates the image itself (see otaSelfTestPoll).
extern "C" bool verifyRollbackLater() { return true; }

void setup() {
#if LOW_POWER_RADIO_TEST
  setCpuFrequencyMhz(80);
#endif
  Serial.begin(115200);
  Serial0.begin(115200);
  delay(200);
  Serial.println("\nfinagotchi_firmware starting...");
  Serial0.println("UART0 Wi-Fi diagnostics ready");

#if DISPLAY_TEST_ONLY
  Serial.println("DISPLAY TEST: initializing GC9A01, SPI 27 MHz");
#endif
  tft.init();

#ifdef TFT_BL
  backlightSet(255);   // full brightness (PWM, so AFK can dim it later)
#endif

#ifndef TFT_ROTATION
#define TFT_ROTATION 0
#endif
  tft.setRotation(TFT_ROTATION);

#if SHOW_RESET_REASON
  esp_reset_reason_t resetReason = esp_reset_reason();
  char resetText[24];
  snprintf(resetText, sizeof(resetText), "RESET REASON %d", resetReason);
  tft.setTextDatum(TC_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(resetText, tft.width() / 2, tft.height() / 2);
  Serial.printf("BOOT: previous reset reason=%d\n", resetReason);
  delay(1500);
#endif

#if DISPLAY_TEST_ONLY
  Serial.println("DISPLAY TEST: init complete; RED");
  tft.fillScreen(TFT_RED);
  delay(1000);
  Serial.println("DISPLAY TEST: GREEN");
  tft.fillScreen(TFT_GREEN);
  delay(1000);
  Serial.println("DISPLAY TEST: BLUE");
  tft.fillScreen(TFT_BLUE);
  delay(1000);
  Serial.println("DISPLAY TEST: WHITE");
  tft.fillScreen(TFT_WHITE);
  Serial.println("DISPLAY TEST: complete; holding white (comment out DISPLAY_TEST_ONLY for the app)");
  Serial.flush();
  while (true) delay(1000);
#endif

  // OTA mode: a confirmed update stashed in NVS (ota::schedule) takes over
  // the boot BEFORE splash/WiFi/BLE/LVGL/pet — OTA mode is Serial + raw
  // TFT_eSPI + Wi-Fi only, so the TLS download gets the whole heap. Never
  // returns (every path ends in ESP.restart()).
  if (!kNoRadioDemo && ota::pending()) ota::run(tft);
  otaSelfTestBegin();

  showSplash(1500);

#if SHOW_RESET_REASON
  tft.fillScreen(TFT_GREEN);
  delay(500);
#endif

  loadStats();               // restore streak/points before BLE advertises them
  loadWiFiCreds();           // app-provisioned credentials override config.h

#if SHOW_RESET_REASON
  tft.fillScreen(TFT_YELLOW);
  showBootStatus("recovery check");
#endif
  // Recovery: both breadboard buttons, or a 4 s crown hold on PCB v1.
  // Erases stale BLE bonds and the parental code without requiring SW1.
#if DISABLE_RECOVERY_CHECK
  bool wipeBonds = false;
#else
  bool wipeBonds = ui::recoveryRequested();
#endif
  if (wipeBonds) {
    pinSet = false;
    pinCode = 0;
    pinFails = 0;
    pinLockoutUntil = 0;
    prefs.begin("fina", false);
    prefs.putBool("pinset", false);
    prefs.putUChar("pinfail", 0);
    prefs.end();
    showBootStatus("bonds + code erased");
    delay(1200);
  }

#if SHOW_RESET_REASON
  tft.fillScreen(TFT_MAGENTA);
  showBootStatus("BLE start");
#endif
  // BLE FIRST: Bluedroid + the GATT table need a large, contiguous heap
  // slice. Starting BLE before Wi-Fi/the 115 KB pet sprite/LVGL guarantees
  // it gets one — the later allocations all have graceful fallbacks, a
  // half-initialized BLE stack does not (f842b5f boot loop).
  if (kNoRadioDemo) {
    Serial.println("NO RADIO DEMO: BLE and Wi-Fi stay disabled");
#if SHOW_RESET_REASON
    tft.fillScreen(TFT_CYAN);
    showBootStatus("NO RADIO DEMO");
#endif
  } else {
    setupBLE(wipeBonds);
    Serial.printf("heap after BLE: %u free\n", ESP.getFreeHeap());
#if SHOW_RESET_REASON
    tft.fillScreen(TFT_CYAN);
    showBootStatus("BLE ready");
#endif
  }

  // Wi-Fi joins asynchronously below. A bad saved password must never hold
  // startup on a blocking status screen before the pet and controls exist.
  updateStreakFromTime();    // may bump streak if a new day started

  pet.begin(&tft, 88.0f);    // smaller pet, room for the stats bar
  pet.setState(PetState::PET_EGG, millis() / 1000.0f);
#if SHOW_RESET_REASON
  tft.fillScreen(TFT_WHITE);
  showBootStatus("UI begin");
#endif

  // LVGL takes over the display: pet canvas + chrome widgets. Button
  // semantics that touch BLE/pet state stay here as callbacks.
  ui::Actions actions = { actionSyncNow, actionCycleReaction, actionFeedPet,
                          actionCycleItem, actionCycleMood,
                          actionCycleBackdrop, actionTogglePause, actionCreatePlan,
                          actionParentalLock, actionPinCheck, actionPinStore,
                          actionPinClear, actionCheckUpdate, actionOtaConfirmed,
                          actionOtaCancelled, actionPinLockoutMs };
  ui::begin(&tft, &pet, actions);
  ui::setSubStage(subStage);
  ui::setLocked(pinSet);                     // parental gate (NVS "fina"/"pinset")
  ui::setMenuLock(pinSet ? "on" : "off");
  bool wifiUp = !kNoRadioDemo && WiFi.status() == WL_CONNECTED;
  ui::setMenuWifi(kNoRadioDemo ? "demo" : (wifiUp ? "online" : (wifiSsid[0] ? "saved" : "not set")));
  ui::setConnectStatus(kNoRadioDemo ? "offline demo" : (wifiUp ? "online" : "waiting for app"));
  ui::setWifiOnline(wifiUp);
  if (!kNoRadioDemo) wifiRejoin();

  // Create-plan screen: offer the xStocks the device ships mints/logos for.
  static const char* createTickers[sizeof(XSTOCK_MINTS) / sizeof(XSTOCK_MINTS[0])];
  for (size_t i = 0; i < sizeof(XSTOCK_MINTS) / sizeof(XSTOCK_MINTS[0]); i++)
    createTickers[i] = XSTOCK_MINTS[i].ticker;
  ui::setCreateTickers(createTickers, sizeof(XSTOCK_MINTS) / sizeof(XSTOCK_MINTS[0]));

  loadPlans();               // restore DCA plan slots (dcaN blobs) -> UI
#ifdef DCA_DEMO_SEED
  seedDemoPlans();           // sim build: placeholder xStocks plans
#endif

  setupBattery();
  feedback::begin();          // PCB v1: buzzer GPIO17 + haptic GPIO18 (no-op off-PCB)
  nearbyBegin();
  Serial.printf("heap after UI: %u free, %u min-free\n",
                ESP.getFreeHeap(), ESP.getMinFreeHeap());
  blePushState(pet.state());
  ui::setSyncWait(!kNoRadioDemo);

  if (!kNoRadioDemo) {
    // Standalone DCA mirror: polls the relay + price feeds while the app is
    // disconnected. 8 KB stacks: TLS handshakes (WiFiClientSecure) peak ~6 KB.
    // A failed create MUST log — the tasks dying silently (heap too low) is
    // how the 200px-sprite + resident-driver budget broke standalone sync.
    if (xTaskCreate(dcaPollTask, "dca", 8192, nullptr, 1, nullptr) != pdPASS)
      Serial.println("TASK FAIL: dca (heap too low)");

    // Cloud state sync: same shape as the DCA poll, active only when a device
    // token was provisioned.
    if (xTaskCreate(stateSyncTask, "sync", 8192, nullptr, 1, nullptr) != pdPASS)
      Serial.println("TASK FAIL: sync (heap too low)");
  }

  Serial.printf("heap after tasks: %u free\n", ESP.getFreeHeap());

  lastEvolve = millis();
  feedback::chime();          // boot/wake two-tone (PCB v1 only)
  if (WiFi.status() != WL_CONNECTED)
    Serial.println("SER: offline — provision Wi-Fi over serial: "
                   "'wifi:MySSID' <enter>, then the password <enter>");
  Serial.println("Pet running.");
}

void loop() {
  uint32_t nowMs = millis();
  float nowSec = nowMs / 1000.0f;

  nearbyUpdate(appConnected, ui::screenSleeping(), pet.state(), pet.mood(), pet.item());
  uint32_t waveFrom;
  if (nearbyTakeWave(waveFrom)) {
    char waveText[24];
    snprintf(waveText, sizeof(waveText), "%03X waved", static_cast<unsigned>(waveFrom & 0xFFF));
    ui::enqueueToast(waveText);
    pet.react(PetReaction::REACT_DANCE, nowSec);
  }

  // AFK nap edges drive the backlight; the nap also slows the pet scene and
  // adds a small loop idle below (backlight + SPI traffic dominate draw).
  static bool afkWas = false;
  const bool afkNow = ui::afkActive();
  const bool screenOff = ui::screenSleeping();
  if (afkNow != afkWas) {
    afkWas = afkNow;
    backlightSet(afkNow ? AFK_BL_DUTY : 255);
  }

  // Lightweight frame stats, off by default (build with -D UI_PERF_LOG=1):
  // every 10 s, average pet-render time per frame and LVGL time per pass.
#if UI_PERF_LOG
  static uint32_t perfPetUs = 0, perfUiUs = 0, perfPasses = 0,
                  perfFrames = 0, perfT0 = 0;
  uint32_t p0 = micros();
#endif

  // Pet scene renders at the FRAME_MS cadence; LVGL (buttons, timers,
  // screen flush) runs every pass. Napping: slow breathing only.
  if (!screenOff && nowMs - lastFrame >= (afkNow ? AFK_FRAME_MS : FRAME_MS)) {
    lastFrame = nowMs;
    ui::renderPetFrame(nowSec);
#if UI_PERF_LOG
    perfFrames++;
#endif
  }
#if UI_PERF_LOG
  uint32_t p1 = micros();
#endif
  ui::update();

#if UI_PERF_LOG
  perfPetUs += p1 - p0;
  perfUiUs += micros() - p1;
  perfPasses++;
  if (perfT0 == 0) perfT0 = nowMs;
  if (nowMs - perfT0 >= 10000 && perfFrames > 0) {
    Serial.printf("PERF: pet %.1f ms/frame (%lu fps), lvgl %.2f ms/pass (%lu passes/s)\n",
                  static_cast<double>(perfPetUs) / 1000.0 / perfFrames,
                  static_cast<unsigned long>(perfFrames * 1000UL / (nowMs - perfT0)),
                  static_cast<double>(perfUiUs) / 1000.0 / perfPasses,
                  static_cast<unsigned long>(perfPasses * 1000UL / (nowMs - perfT0)));
    perfPetUs = perfUiUs = perfPasses = perfFrames = 0;
    perfT0 = nowMs;
  }
#endif

  otaSelfTestPoll();          // OTA rollback: confirm a new image once healthy
  updateBattery();
  feedback::update();         // walk the beep/buzz pattern (non-blocking)
  pollSerial();               // serial Wi-Fi provisioning (app-less setup)
  if (cmdPending()) processCommand();
  if (provPending) processProvision();
  // OTA check (menu row / BLE ota:start): deferred ~50 ms so the "checking"
  // overlay renders before the blocking manifest fetch.
  if (otaResultPending) applyOtaResult();
  if (otaCheckingNotify) { otaCheckingNotify = false; bleNotifyOta("checking"); }

  // Task-side Wi-Fi rejoin -> wifi:ok notify (queued by noteWifiLink; all
  // BLE work happens on the loop task).
  if (wifiNotifyPending) {
    wifiNotifyPending = false;
    bleNotifyWifi(true, nullptr);
  }

  // BLE (dis)connect edges from the BTC task: UI + pet work happens here.
  if (authPushPending) {
    authPushPending = false;
    blePushState(pet.state());
  }
  if (linkUiPending) {
    linkUiPending = false;
    bool up = appConnected;
    ui::setAppConnected(up);
    pet.setAmbientActivities(!up, nowSec);
    ui::setSyncWait(!up);
    ui::setConnectStatus(up ? "connected - share Wi-Fi" : "waiting for app");
    if (!up) pet.clearLook(nowSec);
  }

  // Boot/background join landed (GOT_IP event): start NTP right away (the
  // blocking setupWiFiTime path is no longer the only one that does) and
  // tell the app once.
  if (wifiGotIpPending) {
    wifiGotIpPending = false;
    configTzTime(TZ_POSIX, "pool.ntp.org", "time.nist.gov");
    if (!wifiWasUp) {
      wifiWasUp = true;
      wifiBootOffline = false;
      bleNotifyWifi(true, nullptr);
    }
    Serial.printf("WiFi: got IP %s, NTP started\n", WiFi.localIP().toString().c_str());
  }
  if (ntpSyncedPending) {
    ntpSyncedPending = false;
    if (time(nullptr) > 1700000000) {
      bool first = !timeSynced;
      timeSynced = true;
      if (first) {
        Serial.println("Time synced (SNTP).");
        updateStreakFromTime();
      }
    }
  }

  // Wi-Fi icon updates (queued by noteWifiLink / the WiFi event hook).
  if (wifiOnlinePending) {
    wifiOnlinePending = false;
    ui::setWifiOnline(wifiOnlineState);
    ui::setMenuWifi(wifiOnlineState ? "online" : (wifiSsid[0] ? "saved" : "not set"));
    ui::setConnectStatus(wifiOnlineState ? "online"
                         : (appConnected ? "connected - share Wi-Fi" : "offline"));
  }

  // Pairing passkey panel rides over everything while pairing is pending.
  // Re-shows on retry: pending->false (failed attempt) ->pending->true is
  // handled by the edge check, and a fresh code arriving while a panel is
  // still up (passkey changed) re-pushes the text.
  static bool passkeyShown = false;
  static uint32_t passkeyShownCode = 0;
  if (passkeyPending && (!passkeyShown || pairingPasskey != passkeyShownCode)) {
    ui::setConnectStatus("pairing - enter code");
    ui::showPasskey(pairingPasskey);
    passkeyShownCode = pairingPasskey;
    passkeyShown = true;
  } else if (!passkeyPending && passkeyShown) {
    ui::hidePasskey();
    ui::setConnectStatus(appConnected ? "connected - share Wi-Fi" : "waiting for app");
    passkeyShown = false;
  }

  // Wall clock for the DCA countdown text (0 = never synced -> "--").
  ui::setEpoch(timeSynced ? static_cast<uint32_t>(time(nullptr)) : 0);

  // Poll task handoffs: stashed gain toasts, then updated plan slots.
  while (toastTail != toastHead) {
    portENTER_CRITICAL(&toastMux);
    char t[sizeof(toastStash[0])];
    memcpy(t, toastStash[toastTail % 3], sizeof(t));
    toastTail++;
    portEXIT_CRITICAL(&toastMux);
    ui::enqueueToast(t);
  }
  if (plansDirty) {
    plansDirty = false;
    savePlans();
    for (size_t i = 0; i < dcaCount; i++)
      ui::setDcaPlan(static_cast<uint8_t>(i), plans[i], planOverdue[i]);
    dcaNudgePending = true;
  }
  if (priceDirty) {
    priceDirty = false;
    ui::setSolUsd(solUsdRate);
    prefs.begin("finagotchi", false);
    prefs.putFloat("solUsd", solUsdRate);
    prefs.end();
  }
  if (dcaNudgePending) {
    dcaNudgePending = false;
    applyDcaMoodNudge(nowSec);
  }
  if (stateDirty) {
    portENTER_CRITICAL(&stateMux);
    CloudState cs = cloudStash;
    stateDirty = false;
    portEXIT_CRITICAL(&stateMux);
    // Apply only when something actually changed — identical polls must not
    // burn NVS write cycles.
    PetState ps = petStateForStage(cs.stage);
    bool changed = ps != pet.state() ||
                   (cs.sub >= 1 && cs.sub <= 12 &&
                    static_cast<uint8_t>(cs.sub) != subStage) ||
                   cs.streak != streak || cs.points != points ||
                   cs.happy != happiness ||
                   (cs.mood >= 0 && static_cast<uint8_t>(cs.mood) != mood);
    if (changed) {
      applySnapshot(ps, cs.sub, cs.streak,
                    cs.mood >= 0 ? static_cast<uint8_t>(cs.mood) : mood,
                    cs.points, cs.happy);
      if (!serverLinked) {
        serverLinked = true;
        prefs.begin("fina", false);
        prefs.putUChar("srvlink", 1);
        prefs.end();
        Serial.println("State sync: server linked (demo auto-evolve off)");
      }
    }
  }

  // Demo auto-evolve runs only while no app is connected and no server is
  // linked (once linked, the server owns the stage).
  if (!appConnected && !serverLinked && nowMs - lastEvolve >= EVOLVE_MS) {
    lastEvolve = nowMs;
    if (!pet.evolve(nowSec)) {
      pet.setState(PetState::PET_EGG, nowSec);   // loop demo
    }
    blePushState(pet.state());
  }

  // Day-boundary streak check. Paused while the app is connected: the app
  // is authoritative for the stats bar and pushes streak:/points:/happy:.
  // Also paused once server-linked: the server owns the streak.
  if (!appConnected && !serverLinked && nowMs - lastStreakCheck >= STREAK_CHECK_MS) {
    lastStreakCheck = nowMs;
    updateStreakFromTime();
  }

  if (afkNow) delay(4);   // idle the loop while napping (BLE runs on its own task)
}
