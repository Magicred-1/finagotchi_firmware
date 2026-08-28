# Finagotchi App Guidelines — driving the hardware pet

How the mobile app keeps the on-device radial pet in sync, including
emotions and collectibles. Read together with `BLE_PROTOCOL.md` (the exact
wire format) and `APP_PROMPT.md` (the build prompt).

## Architecture

```
┌───────────────┐  notify: stage:streak:mood:item  ┌──────────────┐
│  App engine   │ ◄───────────────────────────────  │  ESP32-S3    │
│ (MorphingPet) │                                   │  TFT pet     │
│               │ ───────────────────────────────►  │              │
│               │  write: stage/mood/item/look/...  │              │
└───────────────┘                                   └──────────────┘
```

- The app runs the **same radial engine** (`engine.sample(t)`) and renders it
  with `react-native-svg`. The device renders the same profiles on the TFT.
- **While connected, the app is authoritative**: its engine state (stage,
  mood, item) is pushed to the device; the device's demo auto-evolve pauses.
- **On disconnect**, the device clears its gaze and resumes its own demo.
  The app should show an offline badge and keep its local pet running.

## Sync rules

1. **On connect** (after subscribing to notifications):
   push the full current snapshot, batched with `;`:
   ```
   stage:coinling;mood:1;item:2
   ```
2. **On any local change** of stage / mood / item: send the matching write
   immediately. The device morphs over 0.45 s (`easeOutQuint`), so avoid
   spamming stage changes faster than ~2/s.
3. **On notification** from the device (e.g. midnight streak rollover, or a
   change made while another client was connected): parse
   `<stage>:<streak>:<mood>:<item>`, apply `stage` via
   `engine.setState(stage, t)` and mood via `engine.setExpression(...)` so
   device-initiated changes flow back into the app UI.
4. **Gaze (drag)**: while the user drags on the app pet, call
   `engine.setLook({ yaw, pitch, mix: 0.85, wander: 0 })` locally and write
   `look:<yaw>,<pitch>` throttled to ~10 Hz. On release: clear locally and
   write `look:off`. Angles: yaw ±30°, pitch ±25°.

## Emotion mapping

The device moods are the app's `PetMood` set, id-based in `expressions.ts`
order (see `BLE_PROTOCOL.md`): 0 calm, 1 happy, 2 excited, 3 waiting,
4 sleepy, 5 sad. The device blends expressions over 0.45 s exactly like
`blendExpression`, and moods fully override gaze/split/eyes — same as the
engine.

## Parity: what the device reproduces

The firmware (`src/pet.cpp`) is a line-level port of the radial engine:

- `profiles.ts` radial bodies (64 samples, morphs = same-angle lerp)
- `face.ts` sphere eyes (`eyePoses`), seeded blink schedule
  (`createRng(0x5eed)` — **the device blinks in sync with the app**),
  `loopNoise` liveliness
- `expressions.ts` six moods, `engine.ts` dated setters + look target
- `PetBody` glow (x1.15 @ 22%) and `PetEyes` white (#f5f5f5)
- `PetAccessory` crown/glasses/bowtie/halo/diamond at the app's exact
  R-relative positions and colors
- `PetCanvas` reactions (`react:jump|spin|glow|dance` over BLE) and the
  `LevelUpAnimation` sparkle burst on stage-up

Known LCD approximations: eye alpha becomes color blending (no alpha on
TFT), glow opacity becomes a dimmed color, reactions use keyframe tracks
instead of springs. Everything is a pure function of time on both sides.

## Reactions

Send `react:jump` / `spin` / `glow` / `dance` when the app plays the
matching PetCanvas reaction, so the device mirrors it. Stage-ups
automatically trigger the device's sparkle burst — no command needed.

## Collectibles

- Inventory lives in the app (owned/locked items, unlock logic, store).
- Only the **equipped item id** crosses BLE: `item:<0-4>`. The device draws a
  simple icon-fitted version (crown, glasses, halo, party hat).
- To add new collectibles: pick the next free id, add a case to
  `FinagotchiPet::drawItem()` in `src/pet.cpp`, and bump `ITEM_COUNT` in
  `src/pet.h`. Then document it in `BLE_PROTOCOL.md`.

## BLE implementation notes

- Keep each write ≤ 20 bytes unless you negotiate a larger MTU
  (`requestMTU` on Android; iOS negotiates automatically).
- Serialize writes through a queue — never overlap writes on one
  characteristic.
- Subscribe before sending the connect snapshot, so you don't miss the
  device's hello notification.
- Android 12+: request `BLUETOOTH_SCAN` + `BLUETOOTH_CONNECT` (with
  `neverForLocation` flag if you don't use location). Older Android:
  location permission is required for scanning. iOS:
  `NSBluetoothAlwaysUsageDescription` in `app.json`.
- BLE does **not** work in Expo Go — use a dev client
  (`npx expo prebuild && npx expo run:android`).

## Test flow (nRF Connect)

1. Connect to `Finagotchi`, subscribe to `f1a1`.
2. Write `stage:whale;mood:4;item:1` → device morphs to an excited whale
   wearing a crown, notification echoes `whale:<streak>:4:1`.
3. Write `look:20,-10` → pet gazes up-right; `look:off` → resumes wander.
4. Disconnect → device resumes the demo cycle.
