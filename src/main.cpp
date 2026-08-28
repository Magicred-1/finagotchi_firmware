/*
  finagotchi_firmware
  ESP32-S3 + 1.3" ST7789 TFT display.
  Splash screen with the Finagotchi logo, then the animated blob avatar
  (egg -> coinling -> hodler -> whale, looping as a demo).

  BLE: advertises as "Finagotchi", exposes one characteristic (READ + NOTIFY
  + WRITE). The app reads/subscribes to "<stage>:<streak>:<mood>" updates and
  writes commands to drive the pet (see BLE_PROTOCOL.md):
    stage:egg|coinling|hodler|whale
    look:<yaw>,<pitch>   look:off
    mood:<0-255>         streak:<n>
  While the app is connected, the demo auto-evolve is paused.

  Wi-Fi + NTP: syncs local time so the streak is day-based (consecutive days
  the device has been alive). Streak and last-active-day persist in NVS.
*/

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <time.h>
#include <Preferences.h>
#include "config.h"
#include "logo.h"
#include "pet.h"

TFT_eSPI tft = TFT_eSPI();
FinagotchiPet pet;

// ---------------------------------------------------------------------------
// BLE
// ---------------------------------------------------------------------------

#define SERVICE_UUID        "0000f1a0-0000-1000-8000-00805f9b34fb"
#define CHARACTERISTIC_UUID "0000f1a1-0000-1000-8000-00805f9b34fb"

static BLEServer*         pServer = nullptr;
static BLECharacteristic* pCharacteristic = nullptr;
static volatile bool      appConnected = false;

static const char* STAGE_NAMES[PET_STATE_COUNT] = { "egg", "coinling", "hodler", "whale" };
static uint32_t streak = 0;
static uint8_t  mood = MOOD_CALM;
static uint8_t  item = ITEM_NONE;
static uint32_t points = 0;
static uint8_t  happiness = 50;
static Preferences prefs;

// Push "<stage>:<streak>:<mood>:<item>:<points>:<happy>" (read + notify).
// Can exceed 20 bytes — the app should negotiate MTU >= 64.
static void blePushState(PetState s) {
  if (!pCharacteristic) return;
  char buf[40];
  snprintf(buf, sizeof(buf), "%s:%lu:%u:%u:%lu:%u",
           STAGE_NAMES[s], (unsigned long)streak, mood, item,
           (unsigned long)points, happiness);
  pCharacteristic->setValue(buf);
  pCharacteristic->notify();
  pet.setStats(streak, points, happiness);
  Serial.printf("BLE -> %s\n", buf);
}

// Handle one app command, e.g. "stage:coinling" or "look:-15,8".
static void handleCommand(const char* cmd) {
  float nowSec = millis() / 1000.0f;

  if (strncmp(cmd, "stage:", 6) == 0) {
    const char* name = cmd + 6;
    for (int i = 0; i < PET_STATE_COUNT; i++) {
      if (strcmp(name, STAGE_NAMES[i]) == 0) {
        pet.setState((PetState)i, nowSec);
        blePushState(pet.state());
        return;
      }
    }
    // App store uses numeric stages: 1 egg, 2/3 coinling, 4 hodler, 5 whale
    if (name[0] >= '1' && name[0] <= '5' && name[1] == 0) {
      static const PetState numMap[5] = {
        PET_EGG, PET_COINLING, PET_COINLING, PET_HODLER, PET_WHALE
      };
      pet.setState(numMap[name[0] - '1'], nowSec);
      blePushState(pet.state());
      return;
    }
    Serial.printf("BLE: unknown stage '%s'\n", name);
  }
  else if (strncmp(cmd, "react:", 6) == 0) {
    const char* name = cmd + 6;
    uint8_t r = REACT_NONE;
    if (strcmp(name, "jump") == 0) r = REACT_JUMP;
    else if (strcmp(name, "spin") == 0) r = REACT_SPIN;
    else if (strcmp(name, "glow") == 0) r = REACT_GLOW;
    else if (strcmp(name, "dance") == 0) r = REACT_DANCE;
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
    mood = (uint8_t)atoi(cmd + 5);
    pet.setMood(mood, nowSec);
    blePushState(pet.state());
  }
  else if (strncmp(cmd, "item:", 5) == 0) {
    item = (uint8_t)atoi(cmd + 5);
    pet.setItem(item);
    blePushState(pet.state());
  }
  else if (strncmp(cmd, "points:", 7) == 0) {
    points = (uint32_t)strtoul(cmd + 7, nullptr, 10);
    prefs.begin("fina", false);
    prefs.putUInt("points", points);
    prefs.end();
    blePushState(pet.state());
  }
  else if (strncmp(cmd, "happy:", 6) == 0) {
    int v = atoi(cmd + 6);
    happiness = (uint8_t)(v < 0 ? 0 : (v > 100 ? 100 : v));
    blePushState(pet.state());
  }
  else if (strncmp(cmd, "streak:", 7) == 0) {
    streak = (uint32_t)strtoul(cmd + 7, nullptr, 10);
    blePushState(pet.state());
  }
  else {
    Serial.printf("BLE: unknown command '%s'\n", cmd);
  }
}

class CmdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    std::string v = c->getValue();
    if (v.empty()) return;
    Serial.printf("BLE <- %s\n", v.c_str());

    // Allow several commands per write, separated by ';'
    char buf[64];
    strncpy(buf, v.c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char* tok = strtok(buf, ";");
    while (tok) {
      handleCommand(tok);
      tok = strtok(nullptr, ";");
    }
  }
};

class SrvCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    appConnected = true;
    Serial.println("App connected (demo paused).");
  }
  void onDisconnect(BLEServer* s) override {
    appConnected = false;
    pet.clearLook(millis() / 1000.0f);
    s->getAdvertising()->start();    // keep advertising for the next connection
    Serial.println("App disconnected (demo resumed).");
  }
};

static void setupBLE() {
  BLEDevice::init("Finagotchi");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new SrvCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_READ |
      BLECharacteristic::PROPERTY_NOTIFY |
      BLECharacteristic::PROPERTY_WRITE
  );
  pCharacteristic->setCallbacks(new CmdCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());   // required for notifications
  pCharacteristic->setValue("egg:0:0:0");
  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->start();

  Serial.println("BLE ready. Pair with Finagotchi app.");
}

// ---------------------------------------------------------------------------
// Wi-Fi + NTP
// ---------------------------------------------------------------------------

static bool timeSynced = false;

static bool setupWiFiTime() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
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

static void loadStats() {
  prefs.begin("fina", true);
  streak = prefs.getUInt("streak", 0);
  points = prefs.getUInt("points", 0);
  prefs.end();
}

// Recalculate the streak from the local date. Call after NTP sync and
// periodically; persists only when the day rolls over.
static void updateStreakFromTime() {
  if (!timeSynced) return;

  struct tm ti;
  if (!getLocalTime(&ti, 0)) return;

  // Monotonic-ish day index for day-boundary comparison.
  uint32_t today = (uint32_t)(ti.tm_year + 1900) * 400 + (uint32_t)ti.tm_yday;

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
    Serial.printf("Streak -> %lu\n", (unsigned long)streak);
    blePushState(pet.state());
  }
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

// Demo timing
const uint32_t FRAME_MS  = 33;      // ~30 fps target
const uint32_t EVOLVE_MS = 8000;    // lifecycle stage duration
const uint32_t STREAK_CHECK_MS = 60000;  // re-check the day every minute

static uint32_t lastFrame = 0;
static uint32_t lastEvolve = 0;
static uint32_t lastStreakCheck = 0;

void showSplash(uint32_t durationMs) {
  tft.fillScreen(TFT_BLACK);

  int16_t x = (tft.width() - FINAGOTCHI_LOGO_WIDTH) / 2;
  int16_t y = (tft.height() - FINAGOTCHI_LOGO_HEIGHT) / 2 - 10;
  tft.pushImage(x, y, FINAGOTCHI_LOGO_WIDTH, FINAGOTCHI_LOGO_HEIGHT, finagotchi_logo);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextSize(1);
  tft.drawString("ESP32-S3", tft.width() / 2, tft.height() - 30);

  delay(durationMs);
}

void showBootStatus(const char* msg) {
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(1);
  tft.drawString(msg, tft.width() / 2, tft.height() - 16);
}

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

  showBootStatus("WiFi...");
  setupWiFiTime();
  updateStreakFromTime();    // may bump streak if a new day started
  showBootStatus(timeSynced ? "Time synced" : "Offline mode");
  delay(800);

  pet.begin(&tft, 88.0f);    // smaller pet, room for the stats bar
  pet.setState(PET_EGG, millis() / 1000.0f);

  setupBLE();
  blePushState(pet.state());

  lastEvolve = millis();
  Serial.println("Pet running.");
}

void loop() {
  uint32_t nowMs = millis();
  float nowSec = nowMs / 1000.0f;

  if (nowMs - lastFrame >= FRAME_MS) {
    lastFrame = nowMs;
    pet.render(nowSec);
  }

  // Demo auto-evolve runs only while no app is connected.
  if (!appConnected && nowMs - lastEvolve >= EVOLVE_MS) {
    lastEvolve = nowMs;
    if (!pet.evolve(nowSec)) {
      pet.setState(PET_EGG, nowSec);   // loop demo
    }
    blePushState(pet.state());
  }

  // Day-boundary streak check.
  if (nowMs - lastStreakCheck >= STREAK_CHECK_MS) {
    lastStreakCheck = nowMs;
    updateStreakFromTime();
  }
}
