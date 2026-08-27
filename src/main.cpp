/*
  finagotchi_firmware
  ESP32-S3 + 1.3" ST7789 TFT display.
  Splash screen with the Finagotchi logo, then the animated blob avatar
  (egg -> coinling -> hodler -> whale, looping as a demo).

  BLE: advertises as "Finagotchi", exposes one characteristic that the app
  reads/subscribes to. Value format: "<stage>:<streak>:<mood>".
*/

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include "logo.h"
#include "pet.h"

// ---------------------------------------------------------------------------
// BLE
// ---------------------------------------------------------------------------

#define SERVICE_UUID        "0000f1a0-0000-1000-8000-00805f9b34fb"
#define CHARACTERISTIC_UUID "0000f1a1-0000-1000-8000-00805f9b34fb"

static BLEServer*         pServer = nullptr;
static BLECharacteristic* pCharacteristic = nullptr;

static const char* STAGE_NAMES[PET_STATE_COUNT] = { "egg", "coinling", "hodler", "whale" };
static uint32_t streak = 0;
static uint8_t  mood = 0;   // expressions not wired yet

// Push "<stage>:<streak>:<mood>" to the app (read value + notify).
static void blePushState(PetState s) {
  if (!pCharacteristic) return;
  char buf[24];
  snprintf(buf, sizeof(buf), "%s:%lu:%u", STAGE_NAMES[s], (unsigned long)streak, mood);
  pCharacteristic->setValue(buf);
  pCharacteristic->notify();
  Serial.printf("BLE -> %s\n", buf);
}

static void setupBLE() {
  BLEDevice::init("Finagotchi");
  pServer = BLEDevice::createServer();
  BLEService* pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharacteristic->setValue("egg:0:0");
  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->start();

  Serial.println("BLE ready. Pair with Finagotchi app.");
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

TFT_eSPI tft = TFT_eSPI();
FinagotchiPet pet;

// Demo timing
const uint32_t FRAME_MS  = 33;     // ~30 fps target
const uint32_t EVOLVE_MS = 8000;   // lifecycle stage duration

static uint32_t lastFrame = 0;
static uint32_t lastEvolve = 0;

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
  showSplash(3000);

  pet.begin(&tft, 115.0f);
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

  // Demo: evolve through lifecycle stages, then loop back to egg.
  if (nowMs - lastEvolve >= EVOLVE_MS) {
    lastEvolve = nowMs;
    if (pet.evolve(nowSec)) {
      streak++;
    } else {
      pet.setState(PET_EGG, nowSec);   // loop demo
      streak = 0;
    }
    blePushState(pet.state());
  }
}
