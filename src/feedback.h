/*
  feedback.h — PCB v1 sound + haptics (buzzer on GPIO17, coin motor on
  GPIO18). Non-blocking: patterns are stepped by update() from the main
  loop, so the UI never stalls. On the breadboard build (no
  FINAGOTCHI_PCB_V1) every call compiles to a no-op — nothing to wire.
*/
#pragma once

namespace feedback {

void begin();     // pin/channel setup — call from setup()
void update();    // pattern player — call every loop()

void click();     // crown detent tick: 4 kHz blip + 25 ms motor tap
void confirm();   // crown push / action: mid beep + tap
void back();      // crown long-push or optional cancel switch: low short blip
void chime();     // boot/wake: two-tone up-chime + tap

} // namespace feedback
