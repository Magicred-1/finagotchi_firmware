/*
  finagotchi_firmware
  ESP32-S3 + 1.3" ST7789 TFT display.
  Splash screen with the Finagotchi logo, then the animated blob avatar
  (egg -> coinling -> hodler -> whale, looping as a demo).

  BLE: advertises as "Finagotchi", exposes one characteristic (READ + NOTIFY
  + WRITE). The app reads/subscribes to
  "<stage>:<streak>:<mood>:<item>:<points>:<happy>" updates and writes
  commands to drive the pet (see BLE_PROTOCOL.md):
    stage:egg|coinling|hodler|whale
    look:<yaw>,<pitch>   look:off
    mood:<0-5>  item:<0-5>  react:jump|spin|glow|dance
    points:<n>  happy:<0-100>  streak:<n>
  While the app is connected, the demo auto-evolve and the day-based streak
  check are paused (the app is authoritative for stage and stats).

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
#include <time.h>
#include <Preferences.h>
#include <array>
#include "config.h"
#include "logo.h"
#include "pet.h"

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

bool setupWiFiTime();          // defined in the Wi-Fi section below
void updateStreakFromTime();

// Push "<stage>:<streak>:<mood>:<item>:<points>:<happy>" (read + notify).
// Can exceed 20 bytes — the app should negotiate MTU >= 64.
void blePushState(PetState s) {
  if (!pCharacteristic) return;
  char buf[48];
  snprintf(buf, sizeof(buf), "%s:%lu:%u:%u:%lu:%u",
           STAGE_NAMES[static_cast<size_t>(s)], static_cast<unsigned long>(streak), mood, item,
           static_cast<unsigned long>(points), happiness);
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
  else {
    // Full state snapshot in the same shape the device notifies:
    // "<stage>:<streak>:<mood>:<item>:<points>:<happy>". Lets the app push
    // everything in one write instead of field-by-field commands.
    char sname[12];
    unsigned long s, p;
    unsigned m, it, h;
    if (sscanf(cmd, "%11[^:]:%lu:%u:%u:%lu:%u", sname, &s, &m, &it, &p, &h) == 6) {
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

constexpr uint8_t BUTTON_1_PIN = 37;  // reaction cycle
constexpr uint8_t BUTTON_2_PIN = 39;  // feed / play

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

  // Button 1: cycle reactions
  if (short1) {
    PetReaction r = REACT_CYCLE[reactCycleIdx];
    reactCycleIdx = (reactCycleIdx + 1) % REACT_CYCLE.size();
    pet.react(r, nowSec);
    Serial.printf("BTN1: reaction %u\n", static_cast<unsigned>(r));
  }

  // Button 2: feed/play — boost happiness + happy mood
  if (short2) {
    happiness = happiness > 90 ? 100 : happiness + 10;
    prefs.begin("fina", false);
    prefs.putUChar("happy", happiness);
    prefs.end();
    pet.setMood(PetMoodId::MOOD_HAPPY, nowSec);
    pet.react(PetReaction::REACT_JUMP, nowSec);
    blePushState(pet.state());
    Serial.printf("BTN2: feed, happy=%u\n", happiness);
  }

  // Long press either: cycle mood
  if (long1 || long2) {
    PetMoodId m = MOOD_CYCLE[moodCycleIdx];
    moodCycleIdx = (moodCycleIdx + 1) % MOOD_CYCLE.size();
    mood = static_cast<uint8_t>(m);
    pet.setMood(m, nowSec);
    blePushState(pet.state());
    Serial.printf("BTN long: mood=%u\n", mood);
  }
}

// ---------------------------------------------------------------------------
// Battery gauge (LiPo via 1:1 divider on ADC1)
// ---------------------------------------------------------------------------

// LiPo+ through a 1:1 divider (e.g. 2x 100k) to GPIO4. ADC1, because ADC2 is
// unusable while Wi-Fi is on.
constexpr uint8_t BATTERY_ADC_PIN = 4;
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
  tft.drawString("ESP32-S3", tft.width() / 2, tft.height() - 30);

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
  loadWiFiCreds();           // app-provisioned credentials override config.h

  showBootStatus("WiFi...");
  setupWiFiTime();
  updateStreakFromTime();    // may bump streak if a new day started
  showBootStatus(timeSynced ? "Time synced" : "Offline mode");
  delay(800);

  pet.begin(&tft, 88.0f);    // smaller pet, room for the stats bar
  pet.setState(PetState::PET_EGG, millis() / 1000.0f);

  setupButtons();
  setupBattery();
  setupBLE();
  blePushState(pet.state());
  pet.setSyncWait(true, millis() / 1000.0f);   // advertise -> waiting scene

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
