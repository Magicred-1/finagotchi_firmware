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
  passkey shown on screen, then write "ssid\npass" to the provisioning
  characteristic (encrypted writes only) — stored in NVS from then on.

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
#include <Preferences.h>
#include <array>
#include "config.h"
#include "logo.h"
#include "pet.h"

// Defaults so older config.h copies (pre-DCA) still build.
#ifndef RELAY_HOST
#define RELAY_HOST "relay.example.com"
#endif
#ifndef DEVICE_ID
#define DEVICE_ID "finagotchi-01"
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
uint32_t streak = 0;
uint8_t  mood = static_cast<uint8_t>(PetMoodId::MOOD_CALM);
uint8_t  item = static_cast<uint8_t>(PetItem::ITEM_NONE);
uint32_t points = 0;
uint8_t  happiness = 50;
bool     timeSynced = false;
Preferences prefs;

// Wi-Fi credentials: config.h defaults, overridden by app-provisioned NVS
// values (see the provisioning characteristic below).
char wifiSsid[33] = WIFI_SSID;
char wifiPass[65] = WIFI_PASS;

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
uint8_t  dcaCount = 0;
volatile bool plansDirty = false;          // poll task -> loop task handoff
volatile bool dcaNudgePending = false;     // (dis)connect -> re-eval mood nudge
float    solUsdRate = 0.0f;                // SOL/USD (BLE solusd: or price feed)
volatile bool priceDirty = false;          // price feed -> loop task handoff
void     fetchPrices();                    // defined in the poll section below

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
    pet.setDcaPlan(static_cast<uint8_t>(i), plans[i], planOverdue[i]);

  // SOL/USD rate for the amount toggle on the positions page (solusd:).
  prefs.begin("finagotchi", true);
  solUsdRate = prefs.getFloat("solUsd", 0.0f);
  prefs.end();
  pet.setSolUsd(solUsdRate);
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
  dcaCount = 0;
  pet.clearDcaPlans();
}


// ---------------------------------------------------------------------------
// Demo seed (sim build only): placeholder xStocks plans so the DCA carousel
// and positions page have content without an app/relay. Enabled via
// -D DCA_DEMO_SEED=1 (env:esp32-s3-sim) — never in the production build.
// ---------------------------------------------------------------------------

#ifdef DCA_DEMO_SEED
void seedDemoPlans() {
  if (dcaCount > 0) return;   // real NVS data wins
  uint32_t now = timeSynced ? static_cast<uint32_t>(time(nullptr)) : 0;
  uint32_t base = now ? now : 1780000000u;
  auto mk = [](uint32_t epoch, float amt, const char* tick,
               uint32_t buys, uint32_t held, float price) {
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
  dcaCount = 3;
  for (size_t i = 0; i < dcaCount; i++) {
    planOverdue[i] = (i == 2);
    pet.setDcaPlan(static_cast<uint8_t>(i), plans[i], planOverdue[i]);
  }
  dcaNudgePending = true;   // show the overdue mood nudge too
  pet.setSolUsd(212.40f);   // demo SOL/USD rate for the amount toggle
  Serial.println("DEMO: seeded placeholder plans (SPYX / GOOGLX / HOODX)");
}
#endif

bool setupWiFiTime();          // defined in the Wi-Fi section below
void updateStreakFromTime();

// Push "<stage>:<streak>:<mood>:<item>:<points>:<happy>[:<dcaCount>]"
// (read + notify). The 7th field is optional: old firmware parses only the
// first six, new firmware tolerates its absence. Can exceed 20 bytes — the
// app should negotiate MTU >= 64.
void blePushState(PetState s) {
  if (!pCharacteristic) return;
  char buf[48];
  snprintf(buf, sizeof(buf), "%s:%lu:%u:%u:%lu:%u:%u",
           STAGE_NAMES[static_cast<size_t>(s)], static_cast<unsigned long>(streak), mood, item,
           static_cast<unsigned long>(points), happiness, dcaCount);
  pCharacteristic->setValue(buf);
  pCharacteristic->notify();
  pet.setStats(streak, points, happiness);
  Serial.printf("BLE -> %s\n", buf);
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
    // App store uses numeric stages: 1 egg, 2/3 coinling, 4 hodler, 5 whale
    if (name[0] >= '1' && name[0] <= '5' && name[1] == 0) {
      static const std::array<PetState, 5> numMap = {
        PetState::PET_EGG, PetState::PET_COINLING, PetState::PET_COINLING,
        PetState::PET_HODLER, PetState::PET_WHALE
      };
      pet.setState(numMap[name[0] - '1'], nowSec);
      blePushState(pet.state());
      return;
    }
    Serial.printf("BLE: unknown stage '%s'\n", name);
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
    unsigned i, en;
    unsigned long epoch, buys, hold;
    float amt, price = 0.0f;
    char tick[7];
    int got = sscanf(cmd + 9, "%u:%u:%lu:%f:%6[^:]:%lu:%lu:%f",
                     &i, &en, &epoch, &amt, tick, &buys, &hold, &price);
    if (got >= 7 && i < kDcaMaxPlans) {
      DcaPlan& p = plans[i];
      p.enabled = en != 0;
      p.nextBuyEpoch = static_cast<uint32_t>(epoch);
      p.amountSol = amt;
      strlcpy(p.ticker, tick, sizeof(p.ticker));
      p.buys = static_cast<uint32_t>(buys);
      p.holdingsHeld = static_cast<uint32_t>(hold);
      p.priceUsd = got == 8 ? price : 0.0f;
      planOverdue[i] = false;   // fresh app data: app is authoritative
      if (i >= dcaCount) dcaCount = static_cast<uint8_t>(i + 1);
      prefs.begin("finagotchi", false);
      char key[8];
      snprintf(key, sizeof(key), "dca%u", i);
      prefs.putBytes(key, &p, sizeof(DcaPlan));
      prefs.putUChar("dcaCount", dcaCount);
      prefs.end();
      pet.setDcaPlan(static_cast<uint8_t>(i), p, false);
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
      pet.enqueueToast(t, nowSec);
      pet.react(PetReaction::REACT_DANCE, nowSec);
      pet.sparkleBurst(nowSec);
      Serial.printf("BLE: dca hit %s\n", t);
    }
  }
  else if (strncmp(cmd, "solusd:", 7) == 0) {
    // SOL/USD rate for the positions-page amount toggle (persisted).
    float rate = strtof(cmd + 7, nullptr);
    if (rate > 0.0f) {
      solUsdRate = rate;
      pet.setSolUsd(rate);
      prefs.begin("finagotchi", false);
      prefs.putFloat("solUsd", rate);
      prefs.end();
      Serial.printf("BLE: SOL/USD=%.2f\n", static_cast<double>(rate));
    }
  }
  else {
    // Full state snapshot in the same shape the device notifies:
    // "<stage>:<streak>:<mood>:<item>:<points>:<happy>[:<dcaCount>]". Lets
    // the app push everything in one write instead of field-by-field
    // commands. The 7th field (plan count, device-owned) is parsed but
    // ignored, so new and old app builds can share one format.
    char sname[12];
    unsigned long s, p;
    unsigned m, it, h, dc;
    if (sscanf(cmd, "%11[^:]:%lu:%u:%u:%lu:%u:%u", sname, &s, &m, &it, &p, &h, &dc) >= 6) {
      for (size_t i = 0; i < kPetStateCount; i++) {
        if (strcmp(sname, STAGE_NAMES[i]) == 0) {
          pet.setState(static_cast<PetState>(i), nowSec);
          break;
        }
      }
      streak = static_cast<uint32_t>(s);
      points = static_cast<uint32_t>(p);
      happiness = static_cast<uint8_t>(h > 100 ? 100 : h);
      mood = static_cast<uint8_t>(m < kMoodCount ? m : static_cast<unsigned>(PetMoodId::MOOD_CALM));
      item = static_cast<uint8_t>(it < kItemCount ? it : static_cast<unsigned>(PetItem::ITEM_NONE));
      pet.setMood(static_cast<PetMoodId>(mood), nowSec);
      pet.setItem(static_cast<PetItem>(item));
      prefs.begin("fina", false);
      prefs.putUInt("streak", streak);
      prefs.putUInt("points", points);
      prefs.putUChar("happy", happiness);
      prefs.end();
      blePushState(pet.state());
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
// it. The provisioning characteristic only accepts ENCRYPTED writes, so the
// credentials never travel on an unpaired link.
// ---------------------------------------------------------------------------

volatile bool passkeyPending = false;
uint32_t pairingPasskey = 0;

class SecCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t pass_key) override {
    pairingPasskey = pass_key;
    passkeyPending = true;
    Serial.printf("BLE pairing passkey: %06lu\n", static_cast<unsigned long>(pass_key));
  }
  bool onSecurityRequest() override { return true; }
  bool onConfirmPIN(uint32_t pin) override { (void)pin; return true; }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
    passkeyPending = false;
    Serial.printf("BLE pairing %s\n", cmpl.success ? "OK" : "FAILED");
  }
};

// Writes are stashed and processed on the loop task (see cmdBuf above).
char provBuf[98];
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

// Screen overlay for pairing/status messages, drawn over the pet frame.
char overlayMsg[40] = "";
uint32_t overlayUntil = 0;

void showOverlay(const char* msg, uint32_t ms) {
  strncpy(overlayMsg, msg, sizeof(overlayMsg) - 1);
  overlayMsg[sizeof(overlayMsg) - 1] = 0;
  overlayUntil = millis() + ms;
}

void drawOverlay() {
  int w = tft.width();
  if (passkeyPending) {
    tft.fillRoundRect(w / 2 - 80, 70, 160, 100, 8, TFT_NAVY);
    tft.drawRoundRect(w / 2 - 80, 70, 160, 100, 8, TFT_CYAN);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_NAVY);
    tft.setTextSize(1);
    tft.drawString("pairing code", w / 2, 84);
    char num[8];
    snprintf(num, sizeof(num), "%06lu", static_cast<unsigned long>(pairingPasskey));
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.setTextSize(3);
    tft.drawString(num, w / 2, 105);
    tft.setTextSize(1);
    tft.setTextColor(TFT_LIGHTGREY, TFT_NAVY);
    tft.drawString("enter it in the app", w / 2, 150);
  } else if (overlayMsg[0] && static_cast<int32_t>(millis() - overlayUntil) < 0) {
    tft.fillRoundRect(w / 2 - 90, 96, 180, 48, 8, TFT_NAVY);
    tft.drawRoundRect(w / 2 - 90, 96, 180, 48, 8, TFT_CYAN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.setTextSize(1);
    tft.drawString(overlayMsg, w / 2, 120);
  }
}

// Runs on the loop task: validate + persist "ssid\npass", then reconnect.
void processProvision() {
  char buf[98];
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
    showOverlay("WiFi setup failed", 2500);
    return;
  }
  *nl = 0;
  const char* ssid = buf;
  const char* pass = nl + 1;
  size_t sl = strlen(ssid), pl = strlen(pass);
  if (sl < 1 || sl > 32 || pl > 63 || (pl > 0 && pl < 8)) {
    Serial.printf("PROV: rejected (ssid %u chars, pass %u chars)\n",
                  static_cast<unsigned>(sl), static_cast<unsigned>(pl));
    showOverlay("WiFi setup failed", 2500);
    return;
  }

  prefs.begin("fina", false);
  prefs.putString("wssid", ssid);
  prefs.putString("wpass", pass);
  prefs.end();
  strlcpy(wifiSsid, ssid, sizeof(wifiSsid));
  strlcpy(wifiPass, pass, sizeof(wifiPass));
  Serial.printf("PROV: credentials for '%s' saved, reconnecting...\n", ssid);
  showOverlay("WiFi saved, joining...", 4000);

  WiFi.disconnect(true);
  timeSynced = setupWiFiTime();   // blocks up to ~10 s, once, user-triggered
  updateStreakFromTime();
  showOverlay(timeSynced ? "Online!" : "WiFi failed", 2500);
}

class SrvCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    appConnected = true;
    pet.setSyncWait(false, millis() / 1000.0f);
    Serial.println("App connected (demo paused).");
  }
  void onDisconnect(BLEServer* s) override {
    appConnected = false;
    pet.clearLook(millis() / 1000.0f);
    pet.setSyncWait(true, millis() / 1000.0f);
    dcaNudgePending = true;   // overdue nudge may apply again offline
    s->getAdvertising()->start();    // keep advertising for the next connection
    Serial.println("App disconnected (demo resumed).");
  }
};

void setupBLE() {
  BLEDevice::init("Finagotchi");
  BLEDevice::setMTU(128);   // state string + batched writes exceed 20 bytes

  // Secure Connections + bonding; the device displays the passkey.
  BLEDevice::setSecurityCallbacks(new SecCallbacks());
  BLESecurity* pSecurity = new BLESecurity();
  pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
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
  pCharacteristic->addDescriptor(new BLE2902());   // required for notifications
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
// ---------------------------------------------------------------------------

constexpr uint8_t BUTTON_1_PIN = 4;   // left button
constexpr uint8_t BUTTON_2_PIN = 37;  // right button

constexpr uint32_t DEBOUNCE_MS   = 50;
constexpr uint32_t LONG_PRESS_MS = 1000;

struct Button {
  uint8_t  pin;
  bool     lastRaw;
  bool     state;      // debounced state (LOW = pressed)
  uint32_t lastChange;
  uint32_t pressedAt;
  bool     longFired;
};

Button btn1 = { BUTTON_1_PIN, HIGH, HIGH, 0, 0, false };
Button btn2 = { BUTTON_2_PIN, HIGH, HIGH, 0, 0, false };

// Reaction cycle for button 1
const std::array<PetReaction, 4> REACT_CYCLE = {
  PetReaction::REACT_JUMP, PetReaction::REACT_SPIN,
  PetReaction::REACT_GLOW, PetReaction::REACT_DANCE
};
uint8_t reactCycleIdx = 0;

// Mood cycle for long press
const std::array<PetMoodId, 5> MOOD_CYCLE = {
  PetMoodId::MOOD_CALM, PetMoodId::MOOD_HAPPY, PetMoodId::MOOD_EXCITED,
  PetMoodId::MOOD_SLEEPY, PetMoodId::MOOD_SAD
};
uint8_t moodCycleIdx = 0;

void setupButtons() {
  pinMode(btn1.pin, INPUT_PULLUP);
  pinMode(btn2.pin, INPUT_PULLUP);
  Serial.printf("Buttons: GPIO%d (react), GPIO%d (feed)\n", btn1.pin, btn2.pin);
}

// Returns true on short-press release, sets longFired on long press.
bool handleButton(Button& b, bool& longPress) {
  bool raw = digitalRead(b.pin);
  uint32_t now = millis();

  if (raw != b.lastRaw) {
    b.lastChange = now;
    b.lastRaw = raw;
  }

  longPress = false;

  if ((now - b.lastChange) > DEBOUNCE_MS && raw != b.state) {
    b.state = raw;
    if (b.state == LOW) {           // pressed
      b.pressedAt = now;
      b.longFired = false;
    } else {                        // released
      if (!b.longFired && (now - b.pressedAt) < LONG_PRESS_MS) {
        return true;                // short press
      }
    }
  }

  if (b.state == LOW && !b.longFired && (now - b.pressedAt) >= LONG_PRESS_MS) {
    b.longFired = true;
    longPress = true;               // long press detected
  }

  return false;
}

void handleButtons(float nowSec) {
  bool long1, long2;
  bool short1 = handleButton(btn1, long1);
  bool short2 = handleButton(btn2, long2);

  // BTN1 = action, BTN2 = navigate (see WIRING.md).

  // BTN1 short: context action — feed/play on the pet page, next plan card
  // on the DCA positions page. Double-press: cycle reactions.
  static uint32_t lastShort1 = 0;
  if (short1) {
    if (millis() - lastShort1 < 500) {
      lastShort1 = 0;
      PetReaction r = REACT_CYCLE[reactCycleIdx];
      reactCycleIdx = (reactCycleIdx + 1) % REACT_CYCLE.size();
      pet.react(r, nowSec);
      Serial.printf("BTN1 double: reaction %u\n", static_cast<unsigned>(r));
    } else {
      lastShort1 = millis();
      if (pet.dcaPageVisible()) {
        pet.nextDcaCard(nowSec);
        Serial.println("BTN1: next DCA plan");
      } else {
        happiness = happiness > 90 ? 100 : happiness + 10;
        prefs.begin("fina", false);
        prefs.putUChar("happy", happiness);
        prefs.end();
        pet.setMood(PetMoodId::MOOD_HAPPY, nowSec);
        pet.react(PetReaction::REACT_JUMP, nowSec);
        blePushState(pet.state());
        Serial.printf("BTN1: feed, happy=%u\n", happiness);
      }
    }
  }

  // BTN2 short: switch page (pet <-> DCA positions). Double-press: resume
  // the carousel auto-rotate (drops a pinned plan).
  static uint32_t lastShort2 = 0;
  if (short2) {
    if (millis() - lastShort2 < 500) {
      lastShort2 = 0;
      pet.unpinDcaPlan();
      Serial.println("BTN2 double: carousel auto-rotate");
    } else {
      lastShort2 = millis();
      pet.toggleDcaPage();
      Serial.printf("BTN2: page %s\n", pet.dcaPageVisible() ? "DCA" : "pet");
    }
  }

  // BTN1 long press: cycle mood
  if (long1) {
    PetMoodId m = MOOD_CYCLE[moodCycleIdx];
    moodCycleIdx = (moodCycleIdx + 1) % MOOD_CYCLE.size();
    mood = static_cast<uint8_t>(m);
    pet.setMood(m, nowSec);
    blePushState(pet.state());
    Serial.printf("BTN1 long: mood=%u\n", mood);
  }

  // BTN2 long press: jump back to the pet page
  if (long2) {
    pet.showPetPage();
    Serial.println("BTN2 long: pet page");
  }
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
    pet.clearBattery();
    ema = -1.0f;
    return;
  }

  // Exponential smoothing — a LiPo under load wobbles tens of mV.
  ema = ema < 0.0f ? static_cast<float>(mv) : ema * 0.7f + mv * 0.3f;
  int pct = static_cast<int>((ema - BATTERY_EMPTY_MV) * 100.0f /
                             (BATTERY_FULL_MV - BATTERY_EMPTY_MV));
  pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
  pet.setBattery(static_cast<uint8_t>(pct));
}

// ---------------------------------------------------------------------------
// Wi-Fi + NTP
// ---------------------------------------------------------------------------

// App-provisioned credentials (NVS) win over the config.h defaults.
void loadWiFiCreds() {
  prefs.begin("fina", true);
  String s = prefs.getString("wssid", "");
  String p = prefs.getString("wpass", "");
  prefs.end();
  if (s.length() > 0 && s.length() <= 32) {
    strlcpy(wifiSsid, s.c_str(), sizeof(wifiSsid));
    strlcpy(wifiPass, p.c_str(), sizeof(wifiPass));
    Serial.printf("WiFi: using provisioned credentials (ssid '%s')\n", wifiSsid);
  }
}

bool setupWiFiTime() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid, wifiPass);
  Serial.print("WiFi connecting");

  // Non-blocking-ish: give up after 10 s so the pet still runs offline.
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi failed, running offline.");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return false;
  }

  Serial.printf("\nWiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());

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

// Toasts stashed by the poll task, drained to pet.enqueueToast on the loop
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

void pollDcaRelay() {
  if (WiFi.status() != WL_CONNECTED) return;

  char url[128];
  snprintf(url, sizeof(url), "http://%s/api/device/%s/dca", RELAY_HOST, DEVICE_ID);

  HTTPClient http;
  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("DCA poll: HTTP %d, keeping cache\n", code);
    http.end();
    return;
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
      plans[i].holdingsHeld  = static_cast<uint32_t>(strtoul(holdS, nullptr, 10));
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
}

// Polls only while the app is disconnected; when connected the app is
// authoritative and polls stay paused. 10 s idle tick — the render loop on
// the loop task is never blocked.
void dcaPollTask(void*) {
  bool first = true;
  uint32_t lastPoll = millis();
  for (;;) {
    if (!appConnected) {
      uint32_t wait = first ? DCA_FIRST_POLL_MS : DCA_POLL_MS;
      if (millis() - lastPoll >= wait) {
        lastPoll = millis();
        first = false;
        if (WiFi.status() == WL_CONNECTED) {
          pollDcaRelay();
          fetchPrices();
        } else {
          // Silent retry: rejoin with the current credentials, poll next cycle.
          WiFi.mode(WIFI_STA);
          WiFi.begin(wifiSsid, wifiPass);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10000));
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
size_t httpGetJson(const char* url, char* buf, size_t len) {
  for (int attempt = 0; attempt < 2; attempt++) {
    WiFiClientSecure client;           // task-local: fresh TLS session
    client.setInsecure();              // public read-only data, no certs to leak
    HTTPClient http;
    http.setTimeout(5000);
    if (!http.begin(client, url)) return 0;
    int code = http.GET();
    size_t n = 0;
    if (code == HTTP_CODE_OK)
      n = http.getStreamPtr()->readBytes(reinterpret_cast<uint8_t*>(buf), len - 1);
    http.end();
    if (n > 0) {
      buf[n] = 0;
      return n;
    }
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

void fetchPrices() {
  if (WiFi.status() != WL_CONNECTED) return;

  // One batched Jupiter call: SOL + every enabled plan with a known mint.
  // (api.jup.ag, not lite-api.jup.ag — lite is IPv6-only, ESP32 is IPv4.)
  char url[384];
  int n = snprintf(url, sizeof(url), "https://api.jup.ag/price/v3?ids=%s", SOL_MINT);
  size_t idx[kDcaMaxPlans], ni = 0;   // plan slots included in the call
  for (size_t i = 0; i < dcaCount; i++) {
    if (!plans[i].enabled) continue;
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
    if (!plans[i].enabled || mintFor(plans[i].ticker)) continue;
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
  tft.fillScreen(TFT_BLACK);

  int16_t x = (tft.width() - FINAGOTCHI_LOGO_WIDTH) / 2;
  int16_t y = (tft.height() - FINAGOTCHI_LOGO_HEIGHT) / 2 - 10;
  tft.pushImage(x, y, FINAGOTCHI_LOGO_WIDTH, FINAGOTCHI_LOGO_HEIGHT, finagotchi_logo);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextSize(1);
  tft.drawString("(C) Finagotchi", tft.width() / 2, tft.height() - 30);

  delay(durationMs);
}

void showBootStatus(const char* msg) {
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
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
  loadPlans();               // restore DCA plan slots (dcaN blobs)
  loadWiFiCreds();           // app-provisioned credentials override config.h

  showBootStatus("WiFi...");
  setupWiFiTime();
  updateStreakFromTime();    // may bump streak if a new day started
  showBootStatus(timeSynced ? "Time synced" : "Offline mode");
  delay(800);

#ifdef DCA_DEMO_SEED
  seedDemoPlans();           // sim build: placeholder xStocks plans
#endif

  pet.begin(&tft, 88.0f);    // smaller pet, room for the stats bar
  pet.setState(PetState::PET_EGG, millis() / 1000.0f);

  setupButtons();
  setupBattery();
  setupBLE();
  blePushState(pet.state());
  pet.setSyncWait(true, millis() / 1000.0f);   // advertise -> waiting scene

  // Standalone DCA mirror: polls the relay + price feeds while the app is
  // disconnected. 12 KB stack: TLS handshakes (WiFiClientSecure) are hungry.
  xTaskCreate(dcaPollTask, "dca", 12288, nullptr, 1, nullptr);

  lastEvolve = millis();
  Serial.println("Pet running.");
}

void loop() {
  uint32_t nowMs = millis();
  float nowSec = nowMs / 1000.0f;

  if (nowMs - lastFrame >= FRAME_MS) {
    lastFrame = nowMs;
    pet.render(nowSec);
    drawOverlay();   // pairing passkey / status messages ride over the frame
  }

  handleButtons(nowSec);
  updateBattery();
  if (cmdPending) processCommand();
  if (provPending) processProvision();

  // Wall clock for the DCA countdown text (0 = never synced -> "--").
  pet.setEpoch(timeSynced ? static_cast<uint32_t>(time(nullptr)) : 0);

  // Poll task handoffs: stashed gain toasts, then updated plan slots.
  while (toastTail != toastHead) {
    portENTER_CRITICAL(&toastMux);
    char t[sizeof(toastStash[0])];
    memcpy(t, toastStash[toastTail % 3], sizeof(t));
    toastTail++;
    portEXIT_CRITICAL(&toastMux);
    pet.enqueueToast(t, nowSec);
  }
  if (plansDirty) {
    plansDirty = false;
    savePlans();
    for (size_t i = 0; i < dcaCount; i++)
      pet.setDcaPlan(static_cast<uint8_t>(i), plans[i], planOverdue[i]);
    dcaNudgePending = true;
  }
  if (priceDirty) {
    priceDirty = false;
    pet.setSolUsd(solUsdRate);
    prefs.begin("finagotchi", false);
    prefs.putFloat("solUsd", solUsdRate);
    prefs.end();
  }
  if (dcaNudgePending) {
    dcaNudgePending = false;
    applyDcaMoodNudge(nowSec);
  }

  // Demo auto-evolve runs only while no app is connected.
  if (!appConnected && nowMs - lastEvolve >= EVOLVE_MS) {
    lastEvolve = nowMs;
    if (!pet.evolve(nowSec)) {
      pet.setState(PetState::PET_EGG, nowSec);   // loop demo
    }
    blePushState(pet.state());
  }

  // Day-boundary streak check. Paused while the app is connected: the app
  // is authoritative for the stats bar and pushes streak:/points:/happy:.
  if (!appConnected && nowMs - lastStreakCheck >= STREAK_CHECK_MS) {
    lastStreakCheck = nowMs;
    updateStreakFromTime();
  }
}
