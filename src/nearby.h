#pragma once

#include <Arduino.h>
#include "pet.h"

constexpr size_t kNearbyMaxPeers = 8;
constexpr uint32_t kNearbyPeerTimeoutMs = 15000;

enum class NearbyAction : uint8_t {
  NONE = 0,
  WAVE = 1,
};

struct NearbyPeer {
  uint32_t id = 0;
  int16_t rssi = -100;
  uint8_t stage = 0;
  uint8_t mood = 0;
  uint8_t item = 0;
  NearbyAction action = NearbyAction::NONE;
  uint8_t sequence = 0;
  uint32_t lastSeen = 0;
};

void nearbyBegin();
void nearbyUpdate(bool appConnected, bool displaySleeping,
                  PetState stage, PetMoodId mood, PetItem item);
bool nearbySendWave();
bool nearbyTakeWave(uint32_t& fromId);
uint8_t nearbyCopyPeers(NearbyPeer* out, uint8_t maxPeers);
bool nearbyIsDemo();
bool nearbyRadioEnabled();
int nearbyZone(int16_t rssi);   // 0 near, 1 mid, 2 far
