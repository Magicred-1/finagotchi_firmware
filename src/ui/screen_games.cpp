/*
  screen_games.cpp — crown-friendly games with the Finagotchi.

  Catch Treats: rotate to move the pet paddle, 30 seconds, three misses.
  Quick Tap: push when GO appears; false starts and reaction time are scored.
  Dance Party: rotate quickly to fill the dance meter before time runs out.

  The lobby and games share one lazy screen. Back returns to the lobby, then
  to Apps; leaving the screen frees its widgets and timer. Crown detent clicks
  come from input.cpp, while wins and catches add their own short feedback.
*/

#include "ui_internal.h"
#include "feedback.h"
#include "../nearby_logo.h"

namespace {

enum class GameMode : uint8_t {
  Lobby,
  CatchPlay,
  CatchEnd,
  ReactionWait,
  ReactionGo,
  ReactionEnd,
  DancePlay,
  DanceEnd,
};

struct GameDef {
  const char* name;
  const char* glyph;
  const char* control;
  uint32_t accent;
};

constexpr uint8_t GAME_COUNT = 3;
constexpr uint8_t GAME_CATCH = 0;
constexpr uint8_t GAME_REACTION = 1;
constexpr uint8_t GAME_DANCE = 2;
const GameDef GAMES[GAME_COUNT] = {
  { "Catch treats", LV_SYMBOL_DOWNLOAD, "rotate", 0xFFD60A },
  { "Quick tap",    LV_SYMBOL_BELL,     "push",   0x30D158 },
  { "Dance party",  LV_SYMBOL_SHUFFLE,  "spin",   0xC4B5FD },
};

constexpr int32_t ROW_W = 184;
constexpr int32_t ROW_H = 42;
constexpr int32_t ROW_GAP = 6;
constexpr int32_t ROW_PITCH = ROW_H + ROW_GAP;
constexpr int32_t LOBBY_LIST_H = 132;
constexpr int32_t LOBBY_LIST_PAD = (LOBBY_LIST_H - ROW_H) / 2;
constexpr uint32_t CATCH_ROUND_MS = 30000;
constexpr uint32_t DANCE_ROUND_MS = 15000;

lv_obj_t* gamesScr;
lv_obj_t* titleLabel;
lv_obj_t* subtitleLabel;
lv_obj_t* hintLabel;
lv_obj_t* lobbyList;
lv_obj_t* lobbyRow[GAME_COUNT];
lv_obj_t* lobbyChip[GAME_COUNT];
lv_obj_t* lobbyGlyph[GAME_COUNT];
lv_obj_t* lobbyName[GAME_COUNT];
lv_obj_t* lobbyControl[GAME_COUNT];
lv_obj_t* hero;
lv_obj_t* bigLabel;
lv_obj_t* paddle;
lv_obj_t* coin;
lv_obj_t* energyTrack;
lv_timer_t* gameTimer;

GameMode mode = GameMode::Lobby;
uint8_t lobbySelected = 0;
uint32_t deadline = 0;
uint32_t goAt = 0;
uint32_t lastDecay = 0;
int paddleX = 120;
int coinX = 120;
int coinY = 82;
uint8_t catchScore = 0;
uint8_t catchMisses = 0;
uint8_t danceEnergy = 0;

void setHint(const char* pcbText, const char* breadboardText) {
#ifdef FINAGOTCHI_PCB_V1
  lv_label_set_text(hintLabel, pcbText);
#else
  lv_label_set_text(hintLabel, breadboardText);
#endif
}

void hideGameObjects() {
  lv_obj_set_hidden(hero, true);
  lv_obj_set_hidden(bigLabel, true);
  lv_obj_set_hidden(paddle, true);
  lv_obj_set_hidden(coin, true);
  lv_obj_set_hidden(energyTrack, true);
}

void lobbyPaint(uint8_t i, bool focused) {
  lv_obj_set_style_text_color(lobbyName[i], focused ? UI_COL_ON_PRIMARY : UI_COL_TEXT, 0);
  lv_obj_set_style_text_color(lobbyControl[i], focused ? UI_COL_ON_PRIMARY : UI_COL_MUTED, 0);
  lv_obj_set_style_bg_color(lobbyChip[i],
      focused ? UI_COL_ON_PRIMARY : lv_color_hex(GAMES[i].accent), 0);
  lv_obj_set_style_bg_opa(lobbyChip[i], focused ? LV_OPA_COVER : LV_OPA_70, 0);
  lv_obj_set_style_text_color(lobbyGlyph[i], focused ? UI_COL_PRIMARY : UI_COL_ON_PRIMARY, 0);
}

void lobbyCounter(uint8_t i) {
  char buf[36];
  snprintf(buf, sizeof(buf), "%u of %u  " LV_SYMBOL_BULLET "  with your Finagotchi",
           static_cast<unsigned>(i + 1), static_cast<unsigned>(GAME_COUNT));
  lv_label_set_text(subtitleLabel, buf);
}

void lobbyFocused(lv_event_t* e) {
  lv_obj_t* target = lv_event_get_target_obj(e);
  uint8_t i = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
  lobbySelected = i;
  lv_obj_scroll_to_y(lobbyList, static_cast<int32_t>(i) * ROW_PITCH, LV_ANIM_ON);
  lobbyPaint(i, true);
  lobbyCounter(i);
  uiThemeNudge(lobbyGlyph[i], -2);
}

void lobbyDefocused(lv_event_t* e) {
  lv_obj_t* target = lv_event_get_target_obj(e);
  uint8_t i = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
  lobbyPaint(i, false);
}

void showLobby() {
  mode = GameMode::Lobby;
  hideGameObjects();
  lv_obj_set_hidden(lobbyList, false);
  lv_label_set_text(titleLabel, "Play");
  const uint8_t focus = lobbySelected;
  uiGroupSet(lobbyRow, GAME_COUNT, false);
  lv_group_focus_obj(lobbyRow[focus]);
  lv_obj_scroll_to_y(lobbyList, static_cast<int32_t>(focus) * ROW_PITCH, LV_ANIM_OFF);
  lobbyCounter(focus);
  setHint("push play  " LV_SYMBOL_BULLET "  hold apps",
          "hold play  " LV_SYMBOL_BULLET "  R apps");
}

void movePaddle(int direction) {
  paddleX += direction * 26;
  if (paddleX < 30) paddleX = 30;
  if (paddleX > 210) paddleX = 210;
  lv_obj_set_pos(paddle, paddleX - 23, 166);
}

void spawnCoin() {
  coinX = 30 + static_cast<int>(esp_random() % 181);
  coinY = 82;
  lv_obj_set_pos(coin, coinX - 9, coinY - 9);
}

void catchScoreboard() {
  char buf[40];
  snprintf(buf, sizeof(buf), "%u treats  " LV_SYMBOL_BULLET "  %u misses",
           static_cast<unsigned>(catchScore), static_cast<unsigned>(catchMisses));
  lv_label_set_text(subtitleLabel, buf);
}

void endCatch() {
  mode = GameMode::CatchEnd;
  lv_obj_set_hidden(paddle, true);
  lv_obj_set_hidden(coin, true);
  lv_obj_set_hidden(bigLabel, false);
  lv_obj_align(bigLabel, LV_ALIGN_CENTER, 0, -6);
  lv_obj_set_style_text_color(bigLabel, UI_COL_TEXT, 0);
  lv_label_set_text(titleLabel, "Game over");
  char buf[32];
  snprintf(buf, sizeof(buf), "%u treats", static_cast<unsigned>(catchScore));
  lv_label_set_text(bigLabel, buf);
  lv_label_set_text(subtitleLabel, catchScore >= 10 ? "Treat master!" : "Nice catching!");
  setHint("push replay  " LV_SYMBOL_BULLET "  hold games",
          "hold replay  " LV_SYMBOL_BULLET "  R games");
  feedback::chime();
  Serial.printf("Game: catch score=%u misses=%u\n", catchScore, catchMisses);
}

void startCatch() {
  mode = GameMode::CatchPlay;
  hideGameObjects();
  lv_obj_set_hidden(lobbyList, true);
  lv_obj_set_hidden(paddle, false);
  lv_obj_set_hidden(coin, false);
  lv_obj_set_hidden(bigLabel, false);
  lv_obj_align(bigLabel, LV_ALIGN_TOP_MID, 0, 48);
  uiGroupSet(nullptr, 0, false);
  lv_label_set_text(titleLabel, "Catch treats");
  paddleX = 120;
  catchScore = 0;
  catchMisses = 0;
  movePaddle(0);
  spawnCoin();
  catchScoreboard();
  lv_label_set_text(bigLabel, "30s");
  deadline = millis() + CATCH_ROUND_MS;
  setHint("rotate to move  " LV_SYMBOL_BULLET "  hold games",
          "L moves right  " LV_SYMBOL_BULLET "  R games");
}

void endReaction(int32_t reactionMs) {
  mode = GameMode::ReactionEnd;
  lv_obj_set_hidden(hero, true);
  lv_obj_set_hidden(bigLabel, false);
  lv_obj_align(bigLabel, LV_ALIGN_CENTER, 0, -6);
  lv_obj_set_style_text_color(bigLabel, UI_COL_TEXT, 0);
  char buf[32];
  if (reactionMs >= 0) {
    snprintf(buf, sizeof(buf), "%ldms", static_cast<long>(reactionMs));
    lv_label_set_text(subtitleLabel, reactionMs < 250 ? "Lightning fast!" :
                                    (reactionMs < 450 ? "Great reflexes!" : "Good try!"));
    feedback::chime();
  } else if (reactionMs == -2) {
    strlcpy(buf, "Too soon!", sizeof(buf));
    lv_label_set_text(subtitleLabel, "Wait for GO next time");
    feedback::back();
  } else {
    strlcpy(buf, "Missed!", sizeof(buf));
    lv_label_set_text(subtitleLabel, "Push when GO appears");
    feedback::back();
  }
  lv_label_set_text(bigLabel, buf);
  setHint("push retry  " LV_SYMBOL_BULLET "  hold games",
          "hold retry  " LV_SYMBOL_BULLET "  R games");
  Serial.printf("Game: reaction=%ldms\n", static_cast<long>(reactionMs));
}

void startReaction() {
  mode = GameMode::ReactionWait;
  hideGameObjects();
  lv_obj_set_hidden(lobbyList, true);
  lv_obj_set_hidden(hero, false);
  uiGroupSet(nullptr, 0, false);
  lv_label_set_text(titleLabel, "Quick tap");
  lv_label_set_text(subtitleLabel, "Wait for GO...");
  deadline = millis() + 1500 + esp_random() % 2500;
  setHint("push on GO  " LV_SYMBOL_BULLET "  hold games",
          "hold on GO  " LV_SYMBOL_BULLET "  R games");
}

void endDance(bool won) {
  mode = GameMode::DanceEnd;
  lv_obj_set_hidden(hero, true);
  lv_obj_set_hidden(energyTrack, true);
  lv_obj_set_hidden(bigLabel, false);
  lv_obj_align(bigLabel, LV_ALIGN_CENTER, 0, -6);
  lv_obj_set_style_text_color(bigLabel, UI_COL_TEXT, 0);
  lv_label_set_text(titleLabel, won ? "Dance champion" : "Dance party");
  char buf[24];
  snprintf(buf, sizeof(buf), "%u%% hype", static_cast<unsigned>(danceEnergy));
  lv_label_set_text(bigLabel, buf);
  lv_label_set_text(subtitleLabel, won ? "Finagotchi nailed it!" : "Rotate faster next round!");
  if (won) {
    g_ui.pet->react(PetReaction::REACT_DANCE, millis() / 1000.0f);
    feedback::chime();
  } else {
    feedback::back();
  }
  setHint("push replay  " LV_SYMBOL_BULLET "  hold games",
          "hold replay  " LV_SYMBOL_BULLET "  R games");
  Serial.printf("Game: dance energy=%u won=%d\n", danceEnergy, won ? 1 : 0);
}

void startDance() {
  mode = GameMode::DancePlay;
  hideGameObjects();
  lv_obj_set_hidden(lobbyList, true);
  lv_obj_set_hidden(hero, false);
  lv_obj_set_hidden(energyTrack, false);
  uiGroupSet(nullptr, 0, false);
  lv_label_set_text(titleLabel, "Dance party");
  lv_label_set_text(subtitleLabel, "Rotate fast  " LV_SYMBOL_BULLET "  15s");
  danceEnergy = 0;
  lv_bar_set_value(energyTrack, 0, LV_ANIM_OFF);
  deadline = millis() + DANCE_ROUND_MS;
  lastDecay = millis();
  setHint("rotate to dance  " LV_SYMBOL_BULLET "  hold games",
          "L to dance  " LV_SYMBOL_BULLET "  R games");
}

void danceStep() {
  if (mode != GameMode::DancePlay) return;
  danceEnergy = danceEnergy > 92 ? 100 : danceEnergy + 8;
  lv_bar_set_value(energyTrack, danceEnergy, LV_ANIM_OFF);
  uiThemeNudge(hero, (danceEnergy / 8) % 2 ? -3 : 3);
  if (danceEnergy >= 100) endDance(true);
}

void startSelectedGame() {
  feedback::confirm();
  if (lobbySelected == GAME_CATCH) startCatch();
  else if (lobbySelected == GAME_REACTION) startReaction();
  else startDance();
}

void gameTick(lv_timer_t*) {
  if (!gamesScr || lv_screen_active() != gamesScr) return;
  uint32_t now = millis();

  if (mode == GameMode::CatchPlay) {
    if (now >= deadline) { endCatch(); return; }
    static uint32_t lastClock = 0;
    if (now - lastClock >= 200) {
      lastClock = now;
      char clock[12];
      snprintf(clock, sizeof(clock), "%lus", static_cast<unsigned long>((deadline - now + 999) / 1000));
      lv_label_set_text(bigLabel, clock);
    }
    coinY += 2 + catchScore / 4;
    lv_obj_set_pos(coin, coinX - 9, coinY - 9);
    if (coinY + 9 >= 166) {
      if (abs(coinX - paddleX) <= 34) {
        catchScore++;
        feedback::click();
        catchScoreboard();
        spawnCoin();
      } else if (coinY > 186) {
        catchMisses++;
        catchScoreboard();
        if (catchMisses >= 3) { endCatch(); return; }
        spawnCoin();
      }
    }
  } else if (mode == GameMode::ReactionWait) {
    if (now >= deadline) {
      mode = GameMode::ReactionGo;
      goAt = now;
      lv_obj_set_hidden(hero, true);
      lv_obj_set_hidden(bigLabel, false);
      lv_obj_align(bigLabel, LV_ALIGN_CENTER, 0, -6);
      lv_obj_set_style_text_color(bigLabel, UI_COL_SUCCESS, 0);
      lv_label_set_text(bigLabel, "GO!");
      lv_label_set_text(subtitleLabel, "PUSH NOW");
      feedback::chime();
    }
  } else if (mode == GameMode::ReactionGo) {
    if (now - goAt > 1500) endReaction(-1);
  } else if (mode == GameMode::DancePlay) {
    if (now >= deadline) { endDance(false); return; }
    if (now - lastDecay >= 180) {
      lastDecay = now;
      if (danceEnergy > 0) danceEnergy--;
      lv_bar_set_value(energyTrack, danceEnergy, LV_ANIM_OFF);
    }
    static uint32_t lastDanceClock = 0;
    if (now - lastDanceClock >= 200) {
      lastDanceClock = now;
      char buf[36];
      snprintf(buf, sizeof(buf), "Rotate fast  " LV_SYMBOL_BULLET "  %lus",
               static_cast<unsigned long>((deadline - now + 999) / 1000));
      lv_label_set_text(subtitleLabel, buf);
    }
  }
}

lv_obj_t* buildGamesScreen() {
  gamesScr = lv_obj_create(nullptr);
  uiThemeScreen(gamesScr);

  titleLabel = uiThemeLabel(gamesScr, &lv_font_montserrat_16, UI_COL_TEXT);
  lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 10);

  subtitleLabel = uiThemeLabel(gamesScr, &lv_font_montserrat_12, UI_COL_MUTED);
  uiThemeTextBounds(subtitleLabel, 200, 16);
  lv_obj_set_style_text_align(subtitleLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(subtitleLabel, LV_ALIGN_TOP_MID, 0, 30);

  lobbyList = lv_obj_create(gamesScr);
  lv_obj_set_size(lobbyList, ROW_W + 8, LOBBY_LIST_H);
  lv_obj_align(lobbyList, LV_ALIGN_CENTER, 0, 10);
  lv_obj_set_style_bg_opa(lobbyList, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(lobbyList, 0, 0);
  lv_obj_set_style_pad_hor(lobbyList, 4, 0);
  lv_obj_set_style_pad_ver(lobbyList, LOBBY_LIST_PAD, 0);
  lv_obj_set_style_pad_row(lobbyList, ROW_GAP, 0);
  lv_obj_set_flex_flow(lobbyList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(lobbyList, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_dir(lobbyList, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(lobbyList, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_scroll_elastic(lobbyList, false);

  for (uint8_t i = 0; i < GAME_COUNT; i++) {
    lobbyRow[i] = lv_obj_create(lobbyList);
    lv_obj_set_size(lobbyRow[i], ROW_W, ROW_H);
    lv_obj_set_scroll_on_focus(lobbyRow[i], false);
    uiThemeGlass(lobbyRow[i], true);
    lv_obj_set_style_radius(lobbyRow[i], UI_RADIUS_CARD, 0);
    lv_obj_set_style_bg_color(lobbyRow[i], UI_COL_PRIMARY, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(lobbyRow[i], LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_user_data(lobbyRow[i], reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    lv_obj_add_event_cb(lobbyRow[i], lobbyFocused, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(lobbyRow[i], lobbyDefocused, LV_EVENT_DEFOCUSED, nullptr);

    lobbyChip[i] = lv_obj_create(lobbyRow[i]);
    lv_obj_set_size(lobbyChip[i], 30, 30);
    lv_obj_align(lobbyChip[i], LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_radius(lobbyChip[i], UI_RADIUS_SMALL, 0);
    lv_obj_set_style_border_width(lobbyChip[i], 0, 0);
    lv_obj_set_style_pad_all(lobbyChip[i], 0, 0);
    lv_obj_set_scrollable(lobbyChip[i], false);

    lobbyGlyph[i] = uiThemeLabel(lobbyChip[i], &lv_font_montserrat_14, UI_COL_TEXT);
    lv_label_set_text(lobbyGlyph[i], GAMES[i].glyph);
    lv_obj_center(lobbyGlyph[i]);

    lobbyName[i] = uiThemeLabel(lobbyRow[i], &lv_font_montserrat_16, UI_COL_TEXT);
    uiThemeTextBounds(lobbyName[i], 98, 20);
    lv_label_set_text(lobbyName[i], GAMES[i].name);
    lv_obj_align(lobbyName[i], LV_ALIGN_LEFT_MID, 46, 0);

    lobbyControl[i] = uiThemeLabel(lobbyRow[i], &lv_font_montserrat_12, UI_COL_MUTED);
    uiThemeTextBounds(lobbyControl[i], 40, 16);
    lv_obj_set_style_text_align(lobbyControl[i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(lobbyControl[i], GAMES[i].control);
    lv_obj_align(lobbyControl[i], LV_ALIGN_RIGHT_MID, -10, 0);
    lobbyPaint(i, false);
  }

  hero = lv_image_create(gamesScr);
  lv_image_set_src(hero, &nearby_logo);
  lv_image_set_scale(hero, 150);
  lv_obj_align(hero, LV_ALIGN_CENTER, 0, -10);

  bigLabel = uiThemeLabel(gamesScr, &lv_font_montserrat_28, UI_COL_TEXT);
  uiThemeTextBounds(bigLabel, 200, 36);
  lv_obj_set_style_text_align(bigLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(bigLabel, LV_ALIGN_CENTER, 0, -6);
  lv_obj_set_style_text_color(bigLabel, UI_COL_TEXT, 0);

  paddle = lv_obj_create(gamesScr);
  lv_obj_set_size(paddle, 46, 20);
  lv_obj_set_style_radius(paddle, 9, 0);
  lv_obj_set_style_bg_color(paddle, UI_COL_PRIMARY, 0);
  lv_obj_set_style_bg_opa(paddle, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(paddle, 0, 0);
  lv_obj_set_scrollable(paddle, false);
  lv_obj_t* face = uiThemeLabel(paddle, &lv_font_montserrat_14, UI_COL_ON_PRIMARY);
  lv_label_set_text(face, ":)");
  lv_obj_center(face);

  coin = lv_obj_create(gamesScr);
  lv_obj_set_size(coin, 18, 18);
  lv_obj_set_style_radius(coin, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(coin, UI_COL_REWARD, 0);
  lv_obj_set_style_bg_opa(coin, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(coin, 0, 0);
  lv_obj_set_scrollable(coin, false);

  energyTrack = lv_bar_create(gamesScr);
  lv_obj_set_size(energyTrack, 160, 12);
  lv_obj_align(energyTrack, LV_ALIGN_BOTTOM_MID, 0, -42);
  lv_bar_set_range(energyTrack, 0, 100);
  lv_obj_set_style_bg_color(energyTrack, UI_COL_SURFACE, 0);
  lv_obj_set_style_bg_opa(energyTrack, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(energyTrack, UI_COL_SUCCESS, LV_PART_INDICATOR);
  lv_obj_set_style_radius(energyTrack, 6, 0);
  lv_obj_set_style_radius(energyTrack, 6, LV_PART_INDICATOR);

  hintLabel = uiThemeControlHint(gamesScr, "play");
  hideGameObjects();
  gameTimer = lv_timer_create(gameTick, 40, nullptr);
  return gamesScr;
}

} // namespace

void uiScreenGamesShow() {
  Serial.println("Games: open requested");
  bool freeMenu = g_ui.menuScreen && lv_screen_active() == g_ui.menuScreen;
  if (freeMenu) uiScreenMenuShrink();
  Serial.printf("Games: menu shrunk, heap=%u block=%u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  if (!gamesScr) g_ui.gamesScreen = buildGamesScreen();
  Serial.printf("Games: widgets built, heap=%u block=%u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  showLobby();
  Serial.println("Games: lobby ready");
  lv_screen_load_anim(gamesScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 160, 0, freeMenu);
  if (freeMenu) uiScreenMenuFreed();
  Serial.println("Games: transition queued");
}

void uiScreenGamesFreed() {
  if (gameTimer) { lv_timer_delete(gameTimer); gameTimer = nullptr; }
  for (uint8_t i = 0; i < GAME_COUNT; i++) {
    if (lobbyRow[i]) lv_anim_delete(lobbyRow[i], nullptr);
    if (lobbyGlyph[i]) lv_anim_delete(lobbyGlyph[i], nullptr);
    lobbyRow[i] = nullptr;
    lobbyChip[i] = nullptr;
    lobbyGlyph[i] = nullptr;
    lobbyName[i] = nullptr;
    lobbyControl[i] = nullptr;
  }
  if (hero) lv_anim_delete(hero, nullptr);
  gamesScr = nullptr;
  titleLabel = subtitleLabel = hintLabel = nullptr;
  lobbyList = nullptr;
  hero = bigLabel = paddle = coin = energyTrack = nullptr;
  g_ui.gamesScreen = nullptr;
}

bool uiScreenGamesActive() {
  return gamesScr && lv_screen_active() == gamesScr;
}

void uiScreenGamesNext() {
  if (mode == GameMode::Lobby) {
    lobbySelected = (lobbySelected + 1) % GAME_COUNT;
    lv_group_focus_obj(lobbyRow[lobbySelected]);
  } else if (mode == GameMode::CatchPlay) {
    movePaddle(1);
  } else {
    danceStep();
  }
}

void uiScreenGamesPrev() {
  if (mode == GameMode::Lobby) {
    lobbySelected = (lobbySelected + GAME_COUNT - 1) % GAME_COUNT;
    lv_group_focus_obj(lobbyRow[lobbySelected]);
  } else if (mode == GameMode::CatchPlay) {
    movePaddle(-1);
  } else {
    danceStep();
  }
}

void uiScreenGamesActivate() {
  switch (mode) {
    case GameMode::Lobby:       startSelectedGame(); break;
    case GameMode::CatchEnd:    startCatch(); break;
    case GameMode::ReactionWait: endReaction(-2); break;
    case GameMode::ReactionGo:   endReaction(static_cast<int32_t>(millis() - goAt)); break;
    case GameMode::ReactionEnd:  startReaction(); break;
    case GameMode::DanceEnd:     startDance(); break;
    default: break;
  }
}

void uiScreenGamesBack() {
  if (!gamesScr) return;
  if (mode == GameMode::Lobby) uiScreenMenuShow();
  else showLobby();
}
