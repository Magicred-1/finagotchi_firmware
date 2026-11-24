/*
  feedback.cpp — PCB v1 sound + haptics.

  Hardware (v1.6 schematic): piezo buzzer BZ1 is wired 3V3 -> BZ1 -> Q1
  (MMBT3904); GPIO17 PWM on Q1's base switches the piezo's low side (R17
  across the piezo is the discharge path so every PWM cycle swings).
  The coin motor sits between 3V3 and J4 pad 2 (MOTOR_NEG), switched to
  GND by Q2 (AO3400) from GPIO18 — GPIO18 HIGH = motor on.

  Both drivers are non-blocking: each call queues a short buzzer pattern and
  an independent 80%-duty motor pulse. Repeated events cannot extend a pulse
  or bypass its rest period, so rapid crown rotation cannot hold the motor on
  and drag down the 3V3 rail. Both outputs use the Arduino-ESP32 core 2.x
  LEDC API, with separate timers for the buzzer and motor.
*/

#include "feedback.h"

#ifdef FINAGOTCHI_PCB_V1

#include <Arduino.h>

namespace {

constexpr uint8_t BUZZER_PIN = 17;   // Q1 base (piezo low side)
constexpr uint8_t MOTOR_PIN  = 18;   // Q2 gate (motor low side)
constexpr uint8_t BUZZ_CH    = 0;
constexpr uint8_t MOTOR_CH   = 2;   // timer 1; channels 0/1 share the buzzer timer
constexpr int     BUZZ_RES   = 10;
constexpr int     MOTOR_RES  = 8;
constexpr uint32_t MOTOR_FREQ = 20000;
constexpr uint8_t MOTOR_DUTY = 204;  // 80% average drive
constexpr uint32_t MOTOR_REST_MS = 120;

struct Step { uint16_t freq; uint8_t ms; };

// steps per event (freq 0 = silence gap); patterns end with ms==0
const Step PAT_CLICK[]   = { {4000, 8},  {0, 17},  {0, 0} };
const Step PAT_CONFIRM[] = { {2600, 35}, {0, 40},  {0, 0} };
const Step PAT_BACK[]    = { {1800, 30}, {0, 0} };
const Step PAT_CHIME[]   = { {2620, 60}, {3300, 60}, {0, 0} };

const Step* active = nullptr;
uint8_t  stepIdx = 0;
uint32_t stepEnd = 0;
uint32_t motorPulseEnd = 0;
uint32_t motorNextStart = 0;
bool motorOn = false;

void play(const Step* pat, uint8_t motorMs) {
  active = pat;
  stepIdx = 0;
  stepEnd = 0;      // update() applies step 0 on the next loop pass

  uint32_t now = millis();
  if (!motorMs || motorOn || static_cast<int32_t>(now - motorNextStart) < 0) return;
  ledcWrite(MOTOR_CH, MOTOR_DUTY);
  motorOn = true;
  motorPulseEnd = now + motorMs;
  motorNextStart = motorPulseEnd + MOTOR_REST_MS;
}

void applyStep(const Step& s) {
  if (s.freq) ledcWriteTone(BUZZ_CH, s.freq);   // 50% square at freq
  else        ledcWrite(BUZZ_CH, 0);            // silent
}

} // namespace

namespace feedback {

void begin() {
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);
  ledcSetup(MOTOR_CH, MOTOR_FREQ, MOTOR_RES);
  ledcAttachPin(MOTOR_PIN, MOTOR_CH);
  ledcWrite(MOTOR_CH, 0);
  ledcSetup(BUZZ_CH, 4000, BUZZ_RES);
  ledcAttachPin(BUZZER_PIN, BUZZ_CH);
  ledcWrite(BUZZ_CH, 0);
  Serial.println("Feedback: buzzer GPIO17 + haptic GPIO18 (80% PWM, 120 ms rest)");
}

void update() {
  uint32_t now = millis();
  if (motorOn && static_cast<int32_t>(now - motorPulseEnd) >= 0) {
    ledcWrite(MOTOR_CH, 0);
    motorOn = false;
  }

  if (!active || now < stepEnd) return;
  const Step& s = active[stepIdx];
  if (s.ms == 0) {
    ledcWrite(BUZZ_CH, 0);
    active = nullptr;
    return;
  }
  applyStep(s);
  stepEnd = now + s.ms;
  stepIdx++;
}

void click()   { play(PAT_CLICK, 18); }
void confirm() { play(PAT_CONFIRM, 35); }
void back()    { play(PAT_BACK, 25); }
void chime()   { play(PAT_CHIME, 50); }

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
