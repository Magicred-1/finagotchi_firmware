/*
  ui_util.cpp — formatting helpers shared by the screens, ported from the
  old pet.cpp chrome (fmtVal / fmtCountdown / stageNameForSubStage).
*/

#include "ui_internal.h"

void uiFmtVal(uint32_t v, char* buf, size_t n) {
  if (v >= 100000)    snprintf(buf, n, "%luk", static_cast<unsigned long>(v / 1000));
  else if (v >= 10000) snprintf(buf, n, "%.1fk", v / 1000.0f);
  else                snprintf(buf, n, "%lu", static_cast<unsigned long>(v));
}

// Countdown text for a plan: "in 2d 14h" / "in 3h 25m"; "overdue" once the
// epoch passes, "--" when the wall clock was never synced.
void uiFmtCountdown(uint32_t nextBuyEpoch, uint32_t nowEpoch, char* buf, size_t n) {
  if (nowEpoch == 0 || nextBuyEpoch == 0) { strlcpy(buf, "--", n); return; }
  int32_t rem = static_cast<int32_t>(nextBuyEpoch - nowEpoch);
  if (rem <= 0) { strlcpy(buf, "overdue", n); return; }
  if (rem >= 86400)
    snprintf(buf, n, "in %ldd %ldh", static_cast<long>(rem / 86400),
             static_cast<long>(rem % 86400 / 3600));
  else
    snprintf(buf, n, "in %ldh %ldm", static_cast<long>(rem / 3600),
             static_cast<long>(rem % 3600 / 60));
}

// Plan amounts are USDC-denominated (shown as $); the SOL mode divides by
// the SOL/USD rate and falls back to USD while the rate is unknown.
void uiFmtAmount(const DcaPlan& p, char* buf, size_t n) {
  if (g_ui.amountInSol && g_ui.solUsd > 0.0f)
    snprintf(buf, n, "%.4g SOL", static_cast<double>(p.amountSol / g_ui.solUsd));
  else
    snprintf(buf, n, "$%.2f", static_cast<double>(p.amountSol));
}

// USD money text: "$12.3k" for five figures and up, "$123.45" below.
void uiFmtUsd(double usd, char* buf, size_t n) {
  if (usd >= 10000.0) snprintf(buf, n, "$%.1fk", usd / 1000.0);
  else snprintf(buf, n, "$%.2f", usd);
}

// Position value of one plan in USD; <= 0 when the price is unknown.
double uiPlanValueUsd(const DcaPlan& p) {
  if (p.priceUsd <= 0.0f) return 0.0;
  return static_cast<double>(p.holdingsHeld) * static_cast<double>(p.priceUsd);
}

// 12-stage name lookup used by the on-screen stage badge.
const char* uiStageName(uint8_t subStage) {
  switch (subStage) {
    case 1:  return "Egg";
    case 2:  return "Hatching";
    case 3:  return "Hatchling";
    case 4:  return "Tiny Saver";
    case 5:  return "Coinling";
    case 6:  return "Staker";
    case 7:  return "Saver";
    case 8:  return "HODLer";
    case 9:  return "Disciplined";
    case 10: return "Accumu-whale";
    case 11: return "Alpha Whale";
    case 12: return "Whale Legend";
    default: return "";
  }
}

// Amber whenever past due: flagged by the poll, or the epoch simply passed.
// Paused plans (disabled, epoch 0) are never overdue.
bool uiPlanOverdue(uint8_t slot) {
  if (slot >= g_ui.planCount) return false;
  if (!g_ui.plans[slot].enabled) return false;
  if (g_ui.overdue[slot]) return true;
  const DcaPlan& p = g_ui.plans[slot];
  return g_ui.epoch != 0 && p.nextBuyEpoch != 0 &&
         static_cast<int32_t>(p.nextBuyEpoch - g_ui.epoch) <= 0;
}

// Paused plans arrive as en=0, epoch=0 and render a PAUSED state, never a
// countdown.
bool uiPlanPaused(const DcaPlan& p) {
  return !p.enabled || p.nextBuyEpoch == 0;
}
