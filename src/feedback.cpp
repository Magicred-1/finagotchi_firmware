/*
  feedback.cpp — PCB v1 sound + haptics.

  Hardware (v1.6 schematic): piezo buzzer BZ1 is wired 3V3 -> BZ1 -> Q1
  (MMBT3904); GPIO17 PWM on Q1's base switches the piezo's low side (R17
  across the piezo is the discharge path so every PWM cycle swings).
  The coin motor sits between 3V3 and J4 pad 2 (MOTOR_NEG), switched to
  GND by Q2 (AO3400) from GPIO18 — GPIO18 HIGH = motor on.

  Both drivers are non-blocking: each call queues a short pattern of
  (freq, ms, motor) steps and update() walks it with millis(). The piezo
  is driven with a 50% square wave at ~4 kHz (its resonant frequency) via
  ledc channel 0 (Arduino-ESP32 core 2.x API).
*/

#include "feedback.h"

#ifdef FINAGOTCHI_PCB_V1

#include <Arduino.h>

namespace {

constexpr uint8_t BUZZER_PIN = 17;   // Q1 base (piezo low side)
constexpr uint8_t MOTOR_PIN  = 18;   // Q2 gate (motor low side)
constexpr uint8_t BUZZ_CH    = 0;    // ledc channel
constexpr int     BUZZ_RES   = 10;   // 10-bit duty

struct Step { uint16_t freq; uint8_t ms; bool motor; };

// steps per event (freq 0 = silence gap); patterns end with ms==0
const Step PAT_CLICK[]   = { {4000, 8, true},  {0, 17, true},  {0, 0, false} };
const Step PAT_CONFIRM[] = { {2600, 35, true}, {0, 40, true},  {0, 0, false} };
const Step PAT_BACK[]    = { {1800, 30, true}, {0, 0, false} };
const Step PAT_CHIME[]   = { {2620, 60, true}, {3300, 60, true}, {0, 0, false} };

const Step* active = nullptr;
uint8_t  stepIdx = 0;
uint32_t stepEnd = 0;

void play(const Step* pat) {
  active = pat;
  stepIdx = 0;
  stepEnd = 0;      // update() applies step 0 on the next loop pass
}

void applyStep(const Step& s) {
  if (s.freq) ledcWriteTone(BUZZ_CH, s.freq);   // 50% square at freq
  else        ledcWrite(BUZZ_CH, 0);            // silent
  digitalWrite(MOTOR_PIN, s.motor ? HIGH : LOW);
}

} // namespace

namespace feedback {

void begin() {
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);
  ledcSetup(BUZZ_CH, 4000, BUZZ_RES);
  ledcAttachPin(BUZZER_PIN, BUZZ_CH);
  ledcWrite(BUZZ_CH, 0);
  Serial.println("Feedback: buzzer GPIO17 + haptic GPIO18 (PCB v1)");
}

void update() {
  if (!active) return;
  uint32_t now = millis();
  if (now < stepEnd) return;
  const Step& s = active[stepIdx];
  if (s.ms == 0) {               // pattern over: everything off
    ledcWrite(BUZZ_CH, 0);
    digitalWrite(MOTOR_PIN, LOW);
    active = nullptr;
    return;
  }
  applyStep(s);
  stepEnd = now + s.ms;
  stepIdx++;
}

void click()   { play(PAT_CLICK); }
void confirm() { play(PAT_CONFIRM); }
void back()    { play(PAT_BACK); }
void chime()   { play(PAT_CHIME); }

} // namespace feedback

#else  // breadboard build: no buzzer/motor hardware — all no-ops

namespace feedback {
void begin() {}
void update() {}
void click() {}
void confirm() {}
void back() {}
void chime() {}
} // namespace feedback

#endif // FINAGOTCHI_PCB_V1
