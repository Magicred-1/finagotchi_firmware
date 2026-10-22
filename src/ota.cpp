/*
  ota.cpp — OTA mode: a stripped-down firmware-update environment the device
  reboots into after an update is confirmed (see ota.h for the why).

  Nothing from the normal firmware is up: no BLE, no LVGL, no pet, no poll
  tasks. The progress screen is drawn with raw TFT_eSPI text (the same
  direct-to-TFT approach as the boot splash in main.cpp). Wi-Fi credentials
  come from NVS (app-provisioned "fina"/wssid+wpass) with the config.h
  compile-time defaults as fallback — the same sources loadWiFiCreds() uses.

  Security (OTA.md §6): the download goes over HTTPS validated against the
  pinned ISRG Root X1 (api_root_ca.h) — never setInsecure() — and the
  written image is SHA-256-checked against the manifest before
  Update.end(true) flips the boot slot. That means the manifest's url must
  terminate TLS with a Let's Encrypt-issued cert (api.finagotchi.app or an
  LE-fronted bucket); a raw github.com asset URL would fail validation.

  Boot safety is Tier 1 (OTA.md §5): Update only flips the boot partition
  after the full image verifies, so an interrupted/corrupt download always
  boots the old app. Tier 2 (bootloader rollback) remains future work.

  NVS marker (namespace "fina"): otaPend (u8), otaUrl, otaSha, otaSize,
  otaVer, otaTries (u8 attempt counter — a poisoned manifest must not boot
  loop the device, so after OTA_MAX_TRIES failures the marker is dropped
  and the device boots normally).
*/

#include "ota.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <mbedtls/sha256.h>
#include <time.h>
#include "config.h"
#include "api_root_ca.h"

namespace {

constexpr uint8_t  OTA_MAX_TRIES = 3;
constexpr uint32_t WIFI_JOIN_MS = 20000;
constexpr uint32_t NTP_WAIT_MS = 6000;
constexpr uint32_t STALL_MS = 30000;      // no download progress -> fail
constexpr uint32_t ERROR_HOLD_MS = 5000;

// Battery guard (same hardware as main.cpp's gauge: LiPo/2 on GPIO5, ADC1).
// The PCB has no VBUS sense pin, so "USB connected" can't be detected —
// the guard is battery-only: no battery present = USB power = OK to flash.
constexpr uint8_t BATTERY_ADC_PIN = 5;
constexpr float   BATTERY_DIVIDER = 2.0f;
constexpr int     BATTERY_FULL_MV = 4200;
constexpr int     BATTERY_EMPTY_MV = 3000;
constexpr int     BATTERY_PRESENT_MV = 2500;
constexpr int     OTA_MIN_BATTERY_PCT = 40;

// --- raw TFT progress screen -------------------------------------------------

struct OtaScreen {
  TFT_eSPI* tft;
  uint16_t bg, text, muted, accent, danger;
  int lastPct = -1;
};

OtaScreen scr;

void screenStatic(const char* subtitle) {
  TFT_eSPI& tft = *scr.tft;
  tft.fillScreen(scr.bg);
  tft.setTextDatum(TC_DATUM);
  tft.setTextFont(4);
  tft.setTextSize(1);
  tft.setTextColor(scr.text, scr.bg);
  tft.drawString("finagotchi update", tft.width() / 2, 60);
  tft.setTextFont(2);
  tft.setTextColor(scr.accent, scr.bg);
  tft.drawString(subtitle, tft.width() / 2, 92);
  // Progress bar outline (fill drawn by screenProgress).
  tft.drawRect(45, 130, 150, 14, scr.muted);
  scr.lastPct = -1;
}

void screenProgress(int pct) {
  if (pct == scr.lastPct) return;
  scr.lastPct = pct;
  TFT_eSPI& tft = *scr.tft;
  tft.fillRect(47, 132, 146 * pct / 100, 10, scr.accent);
  char line[40];
  snprintf(line, sizeof(line), "Updating... %d%%", pct);
  tft.setTextFont(2);
  tft.setTextColor(scr.text, scr.bg);
  tft.drawString(line, tft.width() / 2, 160);
  tft.setTextColor(scr.muted, scr.bg);
  tft.drawString("keep on charger", tft.width() / 2, 182);
}

void screenMessage(const char* line1, const char* line2, bool isError) {
  TFT_eSPI& tft = *scr.tft;
  tft.fillRect(0, 110, tft.width(), 90, scr.bg);
  tft.setTextFont(2);
  tft.setTextColor(isError ? scr.danger : scr.accent, scr.bg);
  tft.drawString(line1, tft.width() / 2, 150);
  if (line2 && line2[0]) {
    tft.setTextColor(scr.muted, scr.bg);
    tft.drawString(line2, tft.width() / 2, 172);
  }
}

// --- NVS marker ---------------------------------------------------------------

void clearMarker() {
  Preferences prefs;
  prefs.begin("fina", false);
  prefs.remove("otaPend");
  prefs.remove("otaUrl");
  prefs.remove("otaSha");
  prefs.remove("otaSize");
  prefs.remove("otaVer");
  prefs.remove("otaTries");
  prefs.end();
}

// Never returns: shows the error, clears the marker, reboots. (Not marked
// [[noreturn]] — core 2.0.17's ESP.restart() isn't, and the mismatch warns.)
void fail(const char* line1, const char* line2) {
  Serial.printf("OTA: FAILED: %s %s\n", line1, line2 ? line2 : "");
  if (Update.isRunning()) Update.abort();
  screenMessage(line1, line2, true);
  clearMarker();
  delay(ERROR_HOLD_MS);
  ESP.restart();
}

// --- battery ------------------------------------------------------------------

// Fresh read (no EMA — OTA mode wants the value now). Returns 100 when no
// battery is present (divider reads nothing = USB power).
int batteryPercentNow() {
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
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

// --- Wi-Fi + clock --------------------------------------------------------------

// App-provisioned NVS credentials win over the config.h defaults (same
// precedence as main.cpp's loadWiFiCreds).
void joinWiFi() {
  Preferences prefs;
  prefs.begin("fina", true);
  String s = prefs.getString("wssid", "");
  String p = prefs.getString("wpass", "");
  prefs.end();
  if (s.length() == 0 || s.length() > 32) {
    s = WIFI_SSID;
    p = WIFI_PASS;
  }
  if (s.length() == 0) fail("no wifi credentials", "provision via the app");

  WiFi.mode(WIFI_STA);
  WiFi.begin(s.c_str(), p.c_str());
  Serial.printf("OTA: joining WiFi '%s'\n", s.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_JOIN_MS) {
    esp_task_wdt_reset();
    delay(250);
  }
  if (WiFi.status() != WL_CONNECTED)
    fail("wifi join failed", "check credentials");

  // Pinned-CA validation needs a plausible wall clock (cert validity window).
  configTzTime(TZ_POSIX, "pool.ntp.org", "time.nist.gov");
  struct tm ti;
  start = millis();
  while (millis() - start < NTP_WAIT_MS) {
    if (getLocalTime(&ti, 200) && ti.tm_year > 120) return;
    esp_task_wdt_reset();
  }
  fail("clock sync failed", "no ntp, tls would fail");
}

// --- download + flash -----------------------------------------------------------

void toHex(const uint8_t* in, size_t n, char* out) {
  static const char* DIGITS = "0123456789abcdef";   // not HEX: Arduino macro
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = DIGITS[in[i] >> 4];
    out[i * 2 + 1] = DIGITS[in[i] & 0xF];
  }
  out[n * 2] = 0;
}

// Never returns: every failure path goes through fail() -> ESP.restart(),
// success ends in Update.end(true) + reboot.
void downloadAndFlash(const char* url, const char* sha256, uint32_t size) {
  WiFiClientSecure client;
  client.setCACert(kApiRootCa);   // pinned ISRG Root X1 — never setInsecure()
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, url)) fail("bad url", "");
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    char msg[32];
    snprintf(msg, sizeof(msg), "HTTP %d", code);
    http.end();
    fail("download failed", msg);
  }
  int served = http.getSize();
  if (served > 0 && static_cast<uint32_t>(served) != size) {
    http.end();
    fail("size mismatch", "manifest vs server");
  }

  if (!Update.begin(size)) {
    String err = Update.errorString();
    http.end();
    fail("update begin failed", err.c_str());
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);   // 0 = SHA-256 (not 224)

  static uint8_t buf[4096];   // static: keep the loop task stack small
  uint32_t written = 0;
  uint32_t lastProgressAt = millis();
  WiFiClient* stream = http.getStreamPtr();
  while (written < size) {
    size_t want = size - written < sizeof(buf) ? size - written : sizeof(buf);
    int rd = stream->readBytes(reinterpret_cast<char*>(buf), want);
    if (rd > 0) {
      lastProgressAt = millis();
      mbedtls_sha256_update_ret(&sha, buf, static_cast<size_t>(rd));
      if (Update.write(buf, static_cast<size_t>(rd)) != static_cast<size_t>(rd)) {
        mbedtls_sha256_free(&sha);
        String err = Update.errorString();
        http.end();
        fail("flash write failed", err.c_str());
      }
      written += static_cast<uint32_t>(rd);
      screenProgress(static_cast<int>(written * 100 / size));
    } else if (millis() - lastProgressAt > STALL_MS) {
      mbedtls_sha256_free(&sha);
      http.end();
      fail("download stalled", "network drop");
    }
    esp_task_wdt_reset();   // feed the watchdog each chunk
    yield();
  }
  http.end();

  // SHA-256 of the written image vs the manifest — before Update.end(true)
  // gets the chance to flip the boot slot.
  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&sha, digest);
  mbedtls_sha256_free(&sha);
  char hex[65];
  toHex(digest, sizeof(digest), hex);
  if (strcasecmp(hex, sha256) != 0) {
    Serial.printf("OTA: sha256 mismatch\n  got:  %s\n  want: %s\n", hex, sha256);
    fail("sha256 mismatch", "image rejected");
  }

  if (!Update.end(true)) {
    String err = Update.errorString();
    fail("update end failed", err.c_str());
  }
  Serial.printf("OTA: %lu bytes written, sha256 OK — rebooting into new slot\n",
                static_cast<unsigned long>(written));
  screenMessage("update complete", "rebooting...", false);
  clearMarker();
  delay(1500);
  ESP.restart();
}

} // namespace

bool ota::pending() {
  Preferences prefs;
  prefs.begin("fina", true);
  bool p = prefs.getUChar("otaPend", 0) != 0;
  prefs.end();
  return p;
}

void ota::schedule(const char* url, const char* sha256,
                   uint32_t size, const char* version) {
  Preferences prefs;
  prefs.begin("fina", false);
  prefs.putUChar("otaPend", 1);
  prefs.putString("otaUrl", url);
  prefs.putString("otaSha", sha256);
  prefs.putUInt("otaSize", size);
  prefs.putString("otaVer", version);
  prefs.putUChar("otaTries", 0);
  prefs.end();
  Serial.printf("OTA: update to %s scheduled (%lu bytes) — rebooting into OTA mode\n",
                version, static_cast<unsigned long>(size));
  delay(300);   // let the serial log + any pending BLE notify flush
  ESP.restart();
}

void ota::run(TFT_eSPI& tft) {
  disableLoopWDT();   // long TLS/download phases; chunks still feed the TWDT

  scr.tft = &tft;
  scr.bg = tft.color565(0x0B, 0x28, 0x58);
  scr.text = tft.color565(0xF6, 0xFA, 0xFF);
  scr.muted = tft.color565(0xE0, 0xEB, 0xF8);
  scr.accent = tft.color565(0x8D, 0xC9, 0xF6);
  scr.danger = tft.color565(0xF3, 0x6F, 0x7C);
  Serial.println("\nOTA mode: minimal bring-up (no BLE/LVGL/pet)");

  Preferences prefs;
  prefs.begin("fina", true);
  String url = prefs.getString("otaUrl", "");
  String sha = prefs.getString("otaSha", "");
  uint32_t size = prefs.getUInt("otaSize", 0);
  String ver = prefs.getString("otaVer", "?");
  uint8_t tries = prefs.getUChar("otaTries", 0);
  prefs.end();

  // A marker that keeps failing (poisoned URL, dead server) must not loop
  // the device forever: give up and boot the normal firmware.
  if (tries >= OTA_MAX_TRIES) {
    Serial.printf("OTA: %u failed attempts, dropping marker\n", tries);
    screenStatic(ver.c_str());
    screenMessage("update gave up", "booting normal firmware", true);
    clearMarker();
    delay(ERROR_HOLD_MS);
    ESP.restart();
  }
  prefs.begin("fina", false);
  prefs.putUChar("otaTries", tries + 1);
  prefs.end();

  if (url.length() == 0 || sha.length() != 64 || size == 0)
    fail("bad manifest data", "re-check from the menu");

  char sub[48];
  snprintf(sub, sizeof(sub), "version %s  (%.2f MB)", ver.c_str(),
           static_cast<double>(size) / 1048576.0);
  screenStatic(sub);
  screenProgress(0);

  if (batteryPercentNow() < OTA_MIN_BATTERY_PCT)
    fail("battery too low", "charge to 40% first");

  joinWiFi();
  downloadAndFlash(url.c_str(), sha.c_str(), size);   // never returns
}
