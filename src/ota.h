// ota.h — reboot-into-OTA-mode firmware updates (see OTA.md §4).
//
// The N8 breadboard build idles with only ~19 KB free heap — a TLS
// handshake + download buffer cannot fit next to the running LVGL UI, so
// instead of suspending LVGL the confirmed update is stashed in NVS and
// the device REBOOTS into a minimal OTA mode: no BLE, no LVGL, no pet,
// just Serial + raw TFT_eSPI text + Wi-Fi. That guarantees the heap for
// the pinned-CA download and a clean failure path (abort, show the error,
// clear the marker, reboot back into the normal firmware).

#pragma once

#include <TFT_eSPI.h>

namespace ota {

// True when a confirmed update is stashed in NVS (checked first thing in
// setup(), before BLE/LVGL/pet start).
bool pending();

// Store the manifest fields + marker, then ESP.restart() into OTA mode.
// Never returns (not marked [[noreturn]] — see ota.cpp).
void schedule(const char* url, const char* sha256,
              uint32_t size, const char* version);

// OTA-mode routine: join Wi-Fi from the stored NVS credentials, download
// the binary over pinned-CA HTTPS into the Update partition in 4 KB chunks
// (watchdog fed per chunk), verify SHA-256 against the manifest value
// before Update.end(true), clear the marker, reboot. On ANY failure:
// Update.abort(), error on the raw screen for ~5 s, clear marker, reboot
// into the untouched normal firmware. Never returns.
void run(TFT_eSPI& tft);

} // namespace ota
