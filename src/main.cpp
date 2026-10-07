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
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <sys/time.h>
#include <Preferences.h>
#include <array>
#include "config.h"
#include "logo.h"
#include "pet.h"
#include "ui/ui.h"

// Defaults so older config.h copies (pre-DCA) still build.
#ifndef RELAY_HOST
#define RELAY_HOST "relay.example.com"
#endif
#ifndef DEVICE_ID
#define DEVICE_ID "finagotchi-01"
#endif
// Companion API endpoint the state sync task polls with the device token.
// Override in config.h to point at a self-hosted/staging API.
#ifndef STATE_SYNC_URL
#define STATE_SYNC_URL "https://api.finagotchi.app/device/state"
#endif

// Perf stats in loop(), off by default (build with -D UI_PERF_LOG=1).
#ifndef UI_PERF_LOG
#define UI_PERF_LOG 0
#endif

namespace {

TFT_eSPI tft;
FinagotchiPet pet;

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

// Push "<stage>:<streak>:<mood>:<item>:<points>:<happy>[:<dcaCount>]"
// (read + notify). The 7th field is optional: old firmware parses only the
// first six, new firmware tolerates its absence. Can exceed 20 bytes — the
// app should negotiate MTU >= 64.
void blePushState(PetState s) {
  if (!pCharacteristic) return;
  char buf[48];
  snprintf(buf, sizeof(buf), "%s:%lu:%u:%u:%lu:%u:%u:%u",
           STAGE_NAMES[static_cast<size_t>(s)], static_cast<unsigned long>(streak), mood, item,
           static_cast<unsigned long>(points), happiness, dcaCount, subStage);
  pCharacteristic->setValue(buf);
  pCharacteristic->notify();
  ui::setStats(streak, points, happiness);
  ui::setMenuAccessory(ITEM_NAMES[item]);   // keep the menu rows in sync
  ui::setMenuMood(MOOD_NAMES[mood]);
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
    if (sscanf(cmd + 5, "%f,%f", &yaw, &pitch) == 2) {
      pet.setLook(yaw, pitch, nowSec);
    }
  }
  else if (strncmp(cmd, "mood:", 5) == 0) {
    mood = static_cast<uint8_t>(atoi(cmd + 5));
    pet.setMood(static_cast<PetMoodId>(mood), nowSec);
    blePushState(pet.state());
  }
  else if (strncmp(cmd, "bg:", 3) == 0) {
    // Scene backdrop theme (PetCanvas BACKGROUND_COLORS names). Optional:
    // old apps never send it, unknown names are ignored.
    int t = FinagotchiPet::sceneThemeForName(cmd + 3);
    if (t >= 0) {
      pet.setSceneTheme(static_cast<uint8_t>(t));
      Serial.printf("BLE: scene theme '%s'\n", FinagotchiPet::sceneThemeName(static_cast<uint8_t>(t)));
    } else {
      Serial.printf("BLE: unknown bg '%s'\n", cmd + 3);
    }
  }
  else if (strncmp(cmd, "item:", 5) == 0) {
    item = static_cast<uint8_t>(atoi(cmd + 5));
    if (item >= kItemCount) item = 0;   // unknown ids degrade to none
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
    if (sscanf(cmd + 8, "%u:%6s", &n, tick) == 2) {
      char t[24];
      snprintf(t, sizeof(t), "+%u %s", n > 999 ? 999 : n, tick);
      ui::enqueueRewardToast(t);   // a buy landed: purple celebration toast
      pet.react(PetReaction::REACT_DANCE, nowSec);
      pet.sparkleBurst(nowSec);
      Serial.printf("BLE: dca hit %s\n", t);
    }
  }
  else if (strncmp(cmd, "solusd:", 7) == 0) {
    // SOL/USD rate for the DCA-screen amount unit toggle (persisted).
    float rate = strtof(cmd + 7, nullptr);
    if (rate > 0.0f) {
      solUsdRate = rate;
      ui::setSolUsd(rate);
      prefs.begin("finagotchi", false);
      prefs.putFloat("solUsd", rate);
      prefs.end();
      Serial.printf("BLE: SOL/USD=%.2f\n", static_cast<double>(rate));
    }
  }
  else if (strncmp(cmd, "epoch:", 6) == 0) {
    // Fallback clock set from the app (devices whose Wi-Fi has no NTP
    // access). Ignored once NTP has synced — NTP stays authoritative.
    if (!timeSynced) {
      time_t t = static_cast<time_t>(strtoul(cmd + 6, nullptr, 10));
      if (t > 1700000000) {   // sanity: reject clearly-bogus values
        struct timeval tv = { t, 0 };
        settimeofday(&tv, nullptr);
        timeSynced = true;
        Serial.printf("BLE: clock set from app (%lu)\n", static_cast<unsigned long>(t));
      }
    }
  }
  else {
    // "<stage>:<streak>:<mood>:<item>:<points>:<happy>[:<dcaCount>]". Lets
    // the app push everything in one write instead of field-by-field
    // commands. The 7th field (plan count, device-owned) is parsed but
    // ignored, so new and old app builds can share one format.
    char sname[12];
    unsigned long s, p;
    unsigned m, it, h, dc, ss;
    int parsed = sscanf(cmd, "%11[^:]:%lu:%u:%u:%lu:%u:%u:%u", sname, &s, &m, &it, &p, &h, &dc, &ss);
    if (parsed >= 6) {
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
                    static_cast<uint32_t>(p),
                    static_cast<uint8_t>(h > 100 ? 100 : h));
      return;
    }
    Serial.printf("BLE: unknown command '%s'\n", cmd);
  }
}

// Received writes are stashed here and processed on the loop task: the
// Bluedroid BTC task that runs onWrite has a small stack, and doing
// printf/strtok/sscanf/NVS/notify in the callback overflowed it (stack
// canary panic, BTC_TASK).
char cmdBuf[96];
volatile size_t cmdLen = 0;
volatile bool cmdPending = false;
portMUX_TYPE cmdMux = portMUX_INITIALIZER_UNLOCKED;

class CmdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    std::string v = c->getValue();
    if (v.empty()) return;
    portENTER_CRITICAL(&cmdMux);
    if (!cmdPending) {   // drop if the previous write is still queued
      size_t n = v.size() < sizeof(cmdBuf) - 1 ? v.size() : sizeof(cmdBuf) - 1;
      memcpy(cmdBuf, v.data(), n);
      cmdBuf[n] = 0;
      cmdLen = n;
      cmdPending = true;
    }
    portEXIT_CRITICAL(&cmdMux);
  }
};

// Runs on the loop task: log + dispatch one received write.
void processCommand() {
  char buf[96];
  size_t n;
  portENTER_CRITICAL(&cmdMux);
  n = cmdLen;
  memcpy(buf, cmdBuf, n + 1);
  cmdPending = false;
  portEXIT_CRITICAL(&cmdMux);

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
    // Numeric Comparison (both sides have displays): auto-yes.
    Serial.printf("BLE security: numeric comparison %06lu (auto-yes)\n",
                  static_cast<unsigned long>(pin));
    return true;
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
      blePushState(pet.state());
    }
  }
};

// Erase every BLE bond Bluedroid persists in NVS. Call right after
// BLEDevice::init() when the boot chord (both buttons held) fired.
void eraseBleBonds() {
  int num = esp_ble_get_bond_device_num();
  Serial.printf("BLE: %d bonded device(s) in NVS\n", num);
  if (num <= 0) return;
  esp_ble_bond_dev_t list[10];
  int count = num < 10 ? num : 10;
  if (esp_ble_get_bond_device_list(&count, list) == ESP_OK) {
    for (int i = 0; i < count; i++) esp_ble_remove_bond_device(list[i].bd_addr);
    Serial.printf("BLE: erased %d bond(s)\n", count);
  }
}

// Writes are stashed and processed on the loop task (see cmdBuf above).
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
  if (sl < 1 || sl > 32 || pl > 63 || (pl > 0 && pl < 8)) {
    Serial.printf("PROV: rejected (ssid %u chars, pass %u chars)\n",
                  static_cast<unsigned>(sl), static_cast<unsigned>(pl));
    ui::showOverlay("WiFi setup failed", 2500);
    return;
  }
  size_t tl = token ? strlen(token) : 0;
  if (token && (tl < 1 || tl > 128)) {
    Serial.printf("PROV: rejected (token %u chars)\n", static_cast<unsigned>(tl));
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
      ui::showOverlay(msg, 2500);
      ui::setWifiOnline(true);
      bleNotifyWifi(true, nullptr);
    } else {
      // Same creds but offline: attempt one rejoin and report the outcome.
      Serial.println("PROV: unchanged but offline, rejoining");
      ui::showOverlay("WiFi rejoining...", 2500);
      timeSynced = setupWiFiTime();   // blocks up to ~10 s, user-triggered
      if (timeSynced) updateStreakFromTime();
      bool ok = WiFi.status() == WL_CONNECTED;
      ui::showOverlay(ok ? "Online!" : "WiFi failed", 2500);
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
  Serial.printf("PROV: credentials for '%s' saved%s, reconnecting...\n", ssid,
                token ? " (+ device token)" : "");
  ui::showOverlay("WiFi saved, joining...", 4000);

  // Driver stays initialized: a plain disconnect (driver up), then
  // setupWiFiTime -> wifiRejoin brings STA back with the new credentials.
  WiFi.disconnect(false);
  timeSynced = setupWiFiTime();   // blocks up to ~10 s, once, user-triggered
  updateStreakFromTime();
  ui::showOverlay(timeSynced ? "Online!" : "WiFi failed", 2500);
  ui::setWifiOnline(WiFi.status() == WL_CONNECTED);
  // Report the real join result to the app (it monitors the state
  // characteristic) — wifi:ok:<ssid> or wifi:fail:<code>.
  bleNotifyWifi(WiFi.status() == WL_CONNECTED, wifiFailCode());
}

class SrvCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    appConnected = true;
    ui::setSyncWait(false);
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
    pet.clearLook(millis() / 1000.0f);
    ui::setSyncWait(true);
    dcaNudgePending = true;   // overdue nudge may apply again offline
    s->getAdvertising()->start();    // keep advertising for the next connection
    Serial.println("App disconnected (demo resumed).");
  }
};

void setupBLE(bool wipeBonds) {
  BLEDevice::init("Finagotchi");
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

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new SrvCallbacks());

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
  // Encrypted reads + writes on the STATE characteristic too: pairing (the
  // passkey) is then required on first contact — the phone initiates it on
  // the first state read/subscribe instead of only when provisioning Wi-Fi.
  // Base READ/WRITE bits included per the proven provisioning-char pattern;
  // the 2-byte CCCD descriptor below keeps default (unencrypted) perms, so
  // enabling notifications after bonding still works.
  pCharacteristic->setAccessPermissions(ESP_GATT_PERM_READ |
                                        ESP_GATT_PERM_READ_ENCRYPTED |
                                        ESP_GATT_PERM_WRITE |
                                        ESP_GATT_PERM_WRITE_ENCRYPTED);
  // CCCD (0x2902) as a plain 2-byte descriptor: BLE2902() allocates the
  // default ESP_GATT_MAX_ATTR_LEN (600 B) for a value that is only ever 2
  // bytes, and that oversized malloc is what starved on a full heap.
  BLEDescriptor* cccd = new BLEDescriptor(BLEUUID((uint16_t)0x2902), 2);
  uint8_t cccdInit[2] = {0, 0};
  cccd->setValue(cccdInit, 2);   // notifications start disabled
  pCharacteristic->addDescriptor(cccd);
  pCharacteristic->setValue("egg:0:0:0:0:50");

  // Wi-Fi provisioning: encrypted writes only — the stack rejects writes on
  // an unpaired link, which is what forces the passkey pairing first.
  BLECharacteristic* pProvChar = pService->createCharacteristic(
      PROV_CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_WRITE |
      BLECharacteristic::PROPERTY_WRITE_NR
  );
  pProvChar->setAccessPermissions(ESP_GATT_PERM_WRITE |
                                  ESP_GATT_PERM_WRITE_ENCRYPTED);
  pProvChar->setCallbacks(new ProvCallbacks());
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

// Reaction cycle for BTN1 double-press (pet screen)
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

// Menu "Open DCA": ask the app to open its DCA wizard/sheet.
void actionOpenDca() {
  if (appConnected) {
    pCharacteristic->setValue("dca:req");
    pCharacteristic->notify();
    blePushState(pet.state());
    ui::enqueueToast("opening DCA...");
    Serial.println("Menu: DCA open requested from app");
  } else {
    ui::enqueueToast("connect the app");
    Serial.println("Menu: DCA open requested (no app connected)");
  }
}

// Detail double-press: pause/resume a plan. Optimistic local toggle (the
// app rewrites the whole table back, reconciling), then dca:pause:<slot>.
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
    case 15:  return "4WAY_HANDSHAKE_TIMEOUT (wrong password?)";
    case 201: return "NO_AP_FOUND (SSID not found)";
    case 202: return "AUTH_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    default:  return "other";
  }
}

void hookWifiEvents() {
  if (wifiEventsHooked) return;
  wifiEventsHooked = true;
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    lastDisconnectReason = info.wifi_sta_disconnected.reason;
    Serial.printf("WiFi: STA disconnected, reason=%d (%s)\n",
                  lastDisconnectReason, wifiReasonStr(lastDisconnectReason));
    // Runs on the Wi-Fi event task: just flag it, loop() updates the icon.
    wifiOnlineState = false;
    wifiOnlinePending = true;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
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

void wifiRejoin() {
  if (!wifiSsid[0]) return;   // no SSID configured: stay fully off Wi-Fi
  hookWifiEvents();
  lastDisconnectReason = 0;
  WiFi.mode(WIFI_STA);
  wl_status_t st = WiFi.begin(wifiSsid, wifiPass);
  if (st == WL_NO_SHIELD) {   // driver not initialized / wedged
    Serial.println("WiFi: STA enable failed, full radio re-init");
    WiFi.mode(WIFI_OFF);
    delay(500);
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid, wifiPass);
  }
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
  if (!wifiSsid[0]) {
    Serial.println("WiFi: no credentials, running offline.");
    wifiBootOffline = true;
    return false;
  }
  wifiRejoin();
  Serial.print("WiFi connecting");

  // Non-blocking-ish: give up after 10 s so the pet still runs offline.
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("\nWiFi: join failed, status=%d reason=%d (%s)\n",
                  WiFi.status(), lastDisconnectReason,
                  wifiReasonStr(lastDisconnectReason));
    // disconnect(false): drop the association but keep the driver UP —
    // disconnect(true)/WIFI_OFF wedge the deinit on this stack and every
    // later esp_wifi_init fails (NO_MEM), killing standalone sync.
    WiFi.disconnect(false);
    wifiBootOffline = true;
    return false;
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
  prefs.end();
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
    if (syncRequested) {
      syncRequested = false;
      if (!appConnected) {
        if (WiFi.status() == WL_CONNECTED) {
          noteWifiLink();
          bool ok = pollDcaRelay();
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
          pollDcaRelay();
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
// HTTPS with an unverified TLS context: the data is public and read-only.
// Failures keep the last cached price.
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
size_t httpGetJson(const char* url, char* buf, size_t len,
                   const char* auth = nullptr, int* codeOut = nullptr) {
  for (int attempt = 0; attempt < 2; attempt++) {
    WiFiClientSecure client;           // task-local: fresh TLS session
    client.setInsecure();              // public read-only data, no certs to leak
    HTTPClient http;
    http.setTimeout(5000);
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
  if (httpGetJson(url, body, sizeof(body)) > 0) {
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
    if (httpGetJson(qurl, qbody, sizeof(qbody)) == 0) continue;
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
  uint8_t  mood;
  uint32_t points;
  uint8_t  happy;
};
CloudState     cloudStash = {};
volatile bool  stateDirty = false;       // sync task -> loop task handoff
portMUX_TYPE   stateMux = portMUX_INITIALIZER_UNLOCKED;
bool           lastSyncOk = false;

// One poll. Returns the HTTP status code (0 = transport failure).
int syncStateFromServer() {
  char auth[192];
  snprintf(auth, sizeof(auth), "Bearer %s", deviceToken);
  static char body[512];   // static: keep the sync task stack small
  int code = 0;
  size_t n = httpGetJson(STATE_SYNC_URL, body, sizeof(body), auth, &code);
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
  uint32_t upd = static_cast<uint32_t>(jsonNumber(body, "\"upd\":"));
  char moodName[12] = "";
  jsonString(body, "\"mood\":", moodName, sizeof(moodName));
  cs.mood = static_cast<uint8_t>(PetMoodId::MOOD_CALM);
  for (size_t i = 0; i < kMoodCount; i++) {
    if (strcmp(moodName, MOOD_NAMES[i]) == 0) {
      cs.mood = static_cast<uint8_t>(i);
      break;
    }
  }

  if (cs.stage < 1 || cs.stage > 12) {
    lastSyncOk = false;
    Serial.printf("State sync: bad stage %d, ignoring\n", cs.stage);
    return code;
  }
  if (cs.happy > 100) cs.happy = 100;

  portENTER_CRITICAL(&stateMux);
  cloudStash = cs;
  portEXIT_CRITICAL(&stateMux);
  stateDirty = true;
  lastSyncOk = true;
  Serial.printf("State sync: stage=%d sub=%d streak=%lu upd=%lu\n", cs.stage, cs.sub,
                static_cast<unsigned long>(cs.streak), static_cast<unsigned long>(upd));
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
// Display
// ---------------------------------------------------------------------------

// Demo timing
constexpr uint32_t FRAME_MS  = 33;      // ~30 fps target
constexpr uint32_t EVOLVE_MS = 8000;    // lifecycle stage duration
constexpr uint32_t STREAK_CHECK_MS = 60000;  // re-check the day every minute

uint32_t lastFrame = 0;
uint32_t lastEvolve = 0;
uint32_t lastStreakCheck = 0;

void showSplash(uint32_t durationMs) {
  // App navy #07111F (app.json splash background; the logo bitmap is
  // composited over the same color so the pushed rect is invisible).
  uint16_t navy = tft.color565(0x07, 0x11, 0x1F);
  tft.fillScreen(navy);

  // The logo array holds native RGB565; swapBytes puts the bytes in SPI
  // wire order for the push (the panel wants the high byte first).
  int16_t x = (tft.width() - FINAGOTCHI_LOGO_WIDTH) / 2;
  int16_t y = (tft.height() - FINAGOTCHI_LOGO_HEIGHT) / 2 - 10;
  tft.setSwapBytes(true);
  tft.pushImage(x, y, FINAGOTCHI_LOGO_WIDTH, FINAGOTCHI_LOGO_HEIGHT, finagotchi_logo);
  tft.setSwapBytes(false);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, navy);
  tft.setTextSize(1);
  tft.drawString("(C) Finagotchi", tft.width() / 2, tft.height() - 30);

  delay(durationMs);
}

void showBootStatus(const char* msg) {
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_CYAN, tft.color565(0x07, 0x11, 0x1F));
  tft.setTextSize(1);
  tft.drawString(msg, tft.width() / 2, tft.height() - 16);
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nfinagotchi_firmware starting...");

  tft.init();

#ifdef TFT_BL
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
#endif

  tft.setRotation(0);
  showSplash(1500);

  loadStats();               // restore streak/points before BLE advertises them
  loadWiFiCreds();           // app-provisioned credentials override config.h

  // Recovery chord: BOTH buttons held at boot -> erase stale BLE bonds from
  // NVS (a phone holding a dead bond after a re-flash fails authentication
  // silently and never shows a fresh passkey).
  bool wipeBonds = ui::bothButtonsHeld();
  if (wipeBonds) {
    showBootStatus("BLE bonds erased");
    delay(1200);
  }

  // BLE FIRST: Bluedroid + the GATT table need a large, contiguous heap
  // slice. Starting BLE before Wi-Fi/the 115 KB pet sprite/LVGL guarantees
  // it gets one — the later allocations all have graceful fallbacks, a
  // half-initialized BLE stack does not (f842b5f boot loop).
  setupBLE(wipeBonds);
  Serial.printf("heap after BLE: %u free\n", ESP.getFreeHeap());

  // Boot status is still drawn direct-to-TFT: LVGL takes over after Wi-Fi.
  showBootStatus("WiFi...");
  setupWiFiTime();
  updateStreakFromTime();    // may bump streak if a new day started
  showBootStatus(timeSynced ? "Time synced" : "Offline mode");
  delay(800);

  pet.begin(&tft, 88.0f);    // smaller pet, room for the stats bar
  pet.setState(PetState::PET_EGG, millis() / 1000.0f);

  // LVGL takes over the display: pet canvas + chrome widgets. Button
  // semantics that touch BLE/pet state stay here as callbacks.
  ui::Actions actions = { actionSyncNow, actionCycleReaction, actionFeedPet,
                          actionCycleItem, actionCycleMood, actionOpenDca,
                          actionTogglePause, actionCreatePlan };
  ui::begin(&tft, &pet, actions);
  ui::setSubStage(subStage);
  // Initial Wi-Fi glyph state (the boot attempt resolved before LVGL was up).
  ui::setWifiOnline(WiFi.status() == WL_CONNECTED);

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
  Serial.printf("heap after UI: %u free, %u min-free\n",
                ESP.getFreeHeap(), ESP.getMinFreeHeap());
  blePushState(pet.state());
  ui::setSyncWait(true);     // advertise -> waiting scene

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

  Serial.printf("heap after tasks: %u free\n", ESP.getFreeHeap());

  lastEvolve = millis();
  Serial.println("Pet running.");
}

void loop() {
  uint32_t nowMs = millis();
  float nowSec = nowMs / 1000.0f;

  // Lightweight frame stats, off by default (build with -D UI_PERF_LOG=1):
  // every 10 s, average pet-render time per frame and LVGL time per pass.
#if UI_PERF_LOG
  static uint32_t perfPetUs = 0, perfUiUs = 0, perfPasses = 0,
                  perfFrames = 0, perfT0 = 0;
  uint32_t p0 = micros();
#endif

  // Pet scene renders at the FRAME_MS cadence; LVGL (buttons, timers,
  // screen flush) runs every pass.
  if (nowMs - lastFrame >= FRAME_MS) {
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

  updateBattery();
  if (cmdPending) processCommand();
  if (provPending) processProvision();

  // Task-side Wi-Fi rejoin -> wifi:ok notify (queued by noteWifiLink; all
  // BLE work happens on the loop task).
  if (wifiNotifyPending) {
    wifiNotifyPending = false;
    bleNotifyWifi(true, nullptr);
  }

  // Wi-Fi icon updates (queued by noteWifiLink / the WiFi event hook).
  if (wifiOnlinePending) {
    wifiOnlinePending = false;
    ui::setWifiOnline(wifiOnlineState);
  }

  // Pairing passkey panel rides over everything while pairing is pending.
  // Re-shows on retry: pending->false (failed attempt) ->pending->true is
  // handled by the edge check, and a fresh code arriving while a panel is
  // still up (passkey changed) re-pushes the text.
  static bool passkeyShown = false;
  static uint32_t passkeyShownCode = 0;
  if (passkeyPending && (!passkeyShown || pairingPasskey != passkeyShownCode)) {
    ui::showPasskey(pairingPasskey);
    passkeyShownCode = pairingPasskey;
    passkeyShown = true;
  } else if (!passkeyPending && passkeyShown) {
    ui::hidePasskey();
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
                   cs.happy != happiness || cs.mood != mood;
    if (changed) {
      applySnapshot(ps, cs.sub, cs.streak, cs.mood, cs.points, cs.happy);
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
}
