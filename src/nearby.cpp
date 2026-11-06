#include "nearby.h"

#ifndef NEARBY_BLE
#define NEARBY_BLE 0
#endif
#ifndef NEARBY_SIM
#define NEARBY_SIM 0
#endif
#ifndef NEARBY_SIM_COUNT
#define NEARBY_SIM_COUNT 3
#endif
#if NEARBY_SIM_COUNT < 0 || NEARBY_SIM_COUNT > 8
#error "NEARBY_SIM_COUNT must be between 0 and 8"
#endif

#if NEARBY_BLE && NEARBY_SIM
#error "NEARBY_BLE and NEARBY_SIM are mutually exclusive"
#endif

#if NEARBY_BLE
#include <BLEAdvertisedDevice.h>
#include <BLEAdvertising.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#endif

namespace {

constexpr size_t PACKET_SIZE = 13;
NearbyPeer peers[kNearbyMaxPeers];
portMUX_TYPE nearbyMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t localId = 0;
uint32_t waveFrom = 0;
PetState localStage = PetState::PET_EGG;
PetMoodId localMood = PetMoodId::MOOD_CALM;
PetItem localItem = PetItem::ITEM_NONE;
NearbyAction localAction = NearbyAction::NONE;
uint8_t localSequence = 0;
uint32_t actionUntil = 0;
bool stateDirty = true;

#if NEARBY_BLE
BLEScan* scan = nullptr;
bool scanning = false;
uint32_t scanUntil = 0;
uint32_t nextScan = 0;
uint32_t nextAdvUpdate = 0;
#endif

void buildPacket(uint8_t* p) {
  p[0] = 0xFF; p[1] = 0xFF;
  p[2] = 'F'; p[3] = 'G'; p[4] = 1;
  p[5] = static_cast<uint8_t>(localId >> 16);
  p[6] = static_cast<uint8_t>(localId >> 8);
  p[7] = static_cast<uint8_t>(localId);
  p[8] = static_cast<uint8_t>(localStage);
  p[9] = static_cast<uint8_t>(localMood);
  p[10] = static_cast<uint8_t>(localItem);
  p[11] = static_cast<uint8_t>(localAction);
  p[12] = localSequence;
}

#if NEARBY_BLE
void updateAdvertisement(bool force = false) {
  if (!stateDirty && !force) return;
  uint32_t now = millis();
  if (!force && now < nextAdvUpdate) return;
  uint8_t packet[PACKET_SIZE];
  buildPacket(packet);
  BLEAdvertisementData data;
  data.setManufacturerData(std::string(reinterpret_cast<char*>(packet), sizeof(packet)));
  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->stop();
  advertising->setScanResponseData(data);
  advertising->setScanResponse(true);
  advertising->start();
  stateDirty = false;
  nextAdvUpdate = now + 2000;
}

class NearbyCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice device) override {
    std::string raw = device.getManufacturerData();
    if (raw.size() != PACKET_SIZE) return;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(raw.data());
    if (p[0] != 0xFF || p[1] != 0xFF || p[2] != 'F' || p[3] != 'G' || p[4] != 1) return;
    uint32_t id = (static_cast<uint32_t>(p[5]) << 16) |
                  (static_cast<uint32_t>(p[6]) << 8) | p[7];
    if (!id || id == localId) return;
    int16_t rssi = static_cast<int16_t>(device.getRSSI());
    portENTER_CRITICAL(&nearbyMux);
    size_t slot = kNearbyMaxPeers;
    size_t oldest = 0;
    for (size_t i = 0; i < kNearbyMaxPeers; i++) {
      if (peers[i].id == id) { slot = i; break; }
      if (!peers[i].id && slot == kNearbyMaxPeers) slot = i;
      if (peers[i].lastSeen < peers[oldest].lastSeen) oldest = i;
    }
    if (slot == kNearbyMaxPeers) slot = oldest;
    NearbyPeer& peer = peers[slot];
    peer.id = id;
    peer.rssi = peer.lastSeen ? static_cast<int16_t>((peer.rssi * 3 + rssi) / 4) : rssi;
    peer.stage = p[8];
    peer.mood = p[9];
    peer.item = p[10];
    NearbyAction action = static_cast<NearbyAction>(p[11]);
    if (action == NearbyAction::WAVE && peer.sequence != p[12]) waveFrom = id;
    peer.action = action;
    peer.sequence = p[12];
    peer.lastSeen = millis();
    portEXIT_CRITICAL(&nearbyMux);
  }
};

NearbyCallbacks callbacks;
#endif

} // namespace

void nearbyBegin() {
  localId = static_cast<uint32_t>(ESP.getEfuseMac() & 0xFFFFFFULL);
  stateDirty = true;
#if NEARBY_SIM
  auto seed = [](size_t i, uint32_t id, int16_t rssi, uint8_t stage,
                 uint8_t mood, uint8_t item) {
    peers[i].id = id;
    peers[i].rssi = rssi;
    peers[i].stage = stage;
    peers[i].mood = mood;
    peers[i].item = item;
    peers[i].action = NearbyAction::NONE;
    peers[i].sequence = 0;
    peers[i].lastSeen = millis();
  };
  const uint32_t ids[8] = { 0xA11001, 0xB22002, 0xC33003, 0xD44004,
                            0xE55005, 0xF66006, 0xAB7007, 0xBC8008 };
  const int16_t signals[8] = { -52, -72, -88, -58, -68, -78, -82, -92 };
  for (size_t i = 0; i < NEARBY_SIM_COUNT; i++)
    seed(i, ids[i], signals[i], i % 4, i % 6, i % 6);
#endif
#if NEARBY_BLE
  scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&callbacks, true);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(30);
  nextScan = millis() + 1000;
#endif
}

void nearbyUpdate(bool appConnected, bool displaySleeping,
                  PetState stage, PetMoodId mood, PetItem item) {
  if (stage != localStage || mood != localMood || item != localItem) {
    localStage = stage;
    localMood = mood;
    localItem = item;
    stateDirty = true;
  }
  if (localAction != NearbyAction::NONE && millis() >= actionUntil) {
    localAction = NearbyAction::NONE;
    stateDirty = true;
  }
#if NEARBY_SIM
  (void)appConnected;
  (void)displaySleeping;
#endif
#if NEARBY_BLE
  if (appConnected || displaySleeping) {
    if (scanning) {
      scan->stop();
      scanning = false;
    }
    return;
  }
  updateAdvertisement();
  uint32_t now = millis();
  if (scanning && now >= scanUntil) {
    scan->stop();
    scan->clearResults();
    scanning = false;
  }
  if (!scanning && now >= nextScan) {
    scanning = true;
    scanUntil = now + 1100;
    nextScan = now + 4000;
    scan->start(1, [](BLEScanResults) {}, false);
  }
#else
  (void)appConnected;
  (void)displaySleeping;
#endif
}

bool nearbySendWave() {
#if !NEARBY_SIM && !NEARBY_BLE
  return false;
#else
  localAction = NearbyAction::WAVE;
  localSequence++;
  actionUntil = millis() + 5000;
  stateDirty = true;
#if NEARBY_SIM
  waveFrom = 0xB22002;
#endif
#if NEARBY_BLE
  updateAdvertisement(true);
#endif
  return true;
#endif
}

bool nearbyTakeWave(uint32_t& fromId) {
  if (!waveFrom) return false;
  portENTER_CRITICAL(&nearbyMux);
  fromId = waveFrom;
  waveFrom = 0;
  portEXIT_CRITICAL(&nearbyMux);
  return true;
}

uint8_t nearbyCopyPeers(NearbyPeer* out, uint8_t maxPeers) {
  if (!out || maxPeers == 0) return 0;
  NearbyPeer sorted[kNearbyMaxPeers];
  uint8_t count = 0;
  uint32_t now = millis();
  portENTER_CRITICAL(&nearbyMux);
  for (size_t i = 0; i < kNearbyMaxPeers; i++) {
#if !NEARBY_SIM
    if (peers[i].id && now - peers[i].lastSeen > kNearbyPeerTimeoutMs) peers[i].id = 0;
#endif
    if (!peers[i].id) continue;
    uint8_t at = count;
    while (at > 0 && sorted[at - 1].rssi < peers[i].rssi) {
      sorted[at] = sorted[at - 1];
      at--;
    }
    sorted[at] = peers[i];
    if (count < kNearbyMaxPeers) count++;
  }
  portEXIT_CRITICAL(&nearbyMux);
  if (count > maxPeers) count = maxPeers;
  for (uint8_t i = 0; i < count; i++) out[i] = sorted[i];
  return count;
}

bool nearbyIsDemo() {
#if NEARBY_SIM
  return true;
#else
  return false;
#endif
}

bool nearbyRadioEnabled() {
#if NEARBY_BLE
  return true;
#else
  return false;
#endif
}

int nearbyZone(int16_t rssi) {
  if (rssi >= -65) return 0;
  if (rssi >= -80) return 1;
  return 2;
}
