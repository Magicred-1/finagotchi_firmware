# Prompt — build the Finagotchi mobile companion app

Copy everything below the line into your AI coding assistant, then paste the
engine files where indicated.

---

## Role

You are a senior React Native (Expo) engineer. Build a production-quality
mobile companion app called **Finagotchi** for iOS and Android.

## Context

I have an ESP32-S3 device ("Finagotchi") with a 240×240 TFT screen that shows
an animated blob pet (lifecycle stages: `egg → coinling → hodler → whale`).
The pet is rendered by a deterministic, pure-function-of-time TypeScript
engine that outputs SVG paths (`engine.sample(t)` → `{ bodyPath, color,
glowColor, eyes[] }`). The same engine already runs on the web.

The app must:

1. Connect to the device over BLE.
2. Render the **same pet** using the **same engine** (app and device must look identical).
3. Send commands that control the on-device pet.
4. Receive state notifications (`<stage>:<streak>:<mood>`) from the device.

## Tech stack (fixed)

- Expo SDK (managed workflow) + TypeScript
- `react-native-ble-plx` for BLE
- `react-native-svg` for rendering engine output (`<Path d={frame.bodyPath}>` etc.)
- No other UI kit; style with the built-in `StyleSheet`

The engine is pure TypeScript with **no DOM dependencies** — port these files
into the app unchanged: `engine.ts`, `profiles.ts`, `shape.ts`,
`expressions.ts`, `face.ts`, `utils/math.ts` (I will paste them at the end).

## BLE contract (already implemented in firmware — match it exactly)

- Device name: **`Finagotchi`**
- Service UUID: `0000f1a0-0000-1000-8000-00805f9b34fb`
- Characteristic UUID: `0000f1a1-0000-1000-8000-00805f9b34fb` (READ + NOTIFY + WRITE)

**Device → app:** UTF-8 string `<stage>:<streak>:<mood>`, e.g. `coinling:3:0`.
Sent on connect and whenever stage/streak/mood changes.

**App → device:** write UTF-8 commands. One per write, or several separated
by `;`. Keep each write ≤ 20 bytes (default MTU) or negotiate a larger MTU.

| Command | Effect on device |
|---|---|
| `stage:egg` / `stage:coinling` / `stage:hodler` / `stage:whale` | Morph pet to that stage |
| `look:<yaw>,<pitch>` | Steer gaze, degrees, e.g. `look:-15,8` (yaw clamped ±30, pitch ±25) |
| `look:off` | Resume idle gaze wander |
| `mood:<n>` | Set mood id 0–255 (echoed back in state string) |
| `streak:<n>` | Override displayed streak |

**Session rule:** while an app is connected, the on-device demo auto-evolve
pauses — the app is authoritative. On disconnect the device clears its gaze,
resumes advertising, and resumes its demo. Implement auto-reconnect.

## App requirements

### 1. BLE layer (`ble/` module)

- Scan → filter by name `Finagotchi` → connect → discover services →
  subscribe to the characteristic.
- Parse notifications into `{ stage, streak, mood }` and expose via a small
  observable store (React context + `useSyncExternalStore` or zustand — your call).
- `sendCommand(cmd: string)` with a write queue (serialize writes; never
  overlap them) and error logging.
- Permissions: Android 12+ `BLUETOOTH_SCAN` / `BLUETOOTH_CONNECT`, plus
  location permission on older Android; iOS
  `NSBluetoothAlwaysUsageDescription` in `app.json`.
- Handle: Bluetooth off, device not found, mid-session disconnect (show
  status, retry with backoff).

### 2. Pet screen (main screen)

- Render the engine at ~60 fps with `requestAnimationFrame`:
  `engine.sample(performance.now() / 1000)` → draw `bodyPath` (fill
  `color`), an outer glow copy (`glowColor`, slight scale-up), and each eye
  (`<Path d={eye.d}>` with `transform={eye.matrix}` and `opacity={eye.alpha}`)
  inside an `<Svg>` viewBox centered on the pet.
- Mirror the app engine state to the device so both stay in sync:
  - On connect: push current stage (`stage:<name>`) and mood (`mood:<n>`).
  - On local stage/mood change: send the corresponding write.
  - Apply incoming `stage:`/`mood:` notifications to the local engine via
    `engine.setState(...)` / `engine.setExpression(...)` so device-initiated
    changes are reflected too.
- Status header: connection dot (green/red), device name, streak counter
  (🔥 N days) from notifications.

### 3. Interactions

- **Drag on the pet:** map finger offset from pet center to yaw/pitch
  (±30°/±25°), call `engine.setLook({ yaw, pitch, mix: 0.85, wander: 0 })`
  locally **and** write `look:<yaw>,<pitch>` throttled to ~10 Hz. On release:
  clear the look locally and write `look:off`.
- **Mood picker:** buttons for each mood in `expressions.ts`; sets the local
  expression and writes `mood:<n>` (use the mood's index).
- **Evolve button:** advances the local engine with `engine.evolve(t)` and
  writes the new `stage:<name>`.
- **Reconnect button** on the status header when disconnected.

### 4. Quality bar

- Smooth 60 fps pet (no per-frame allocations in the render path; reuse the
  engine's `sample()`).
- Clean dark UI matching the pet aesthetic (near-black background, accent
  color per stage).
- TypeScript strict mode, no `any` leaks in the BLE layer.
- A `__DEV__` mock BLE mode that simulates the device (fake notifications,
  log writes) so the UI can be developed without hardware.

## Engine files

Below are the engine sources to port unchanged. If anything is missing
(e.g. `expressions.ts`, `face.ts`, `utils/math.ts`), ask me for them before
writing stubs.

[PASTE engine.ts, profiles.ts, shape.ts, expressions.ts, face.ts, utils/math.ts HERE]

## Deliverables

1. Full Expo project structure with all files.
2. `app.json` with BLE permissions configured.
3. The BLE module, store, Pet screen, and interaction handlers.
4. Short README: how to run (`npx expo prebuild` + `npx expo run:ios/android`
   — BLE does not work in Expo Go), and how to use the mock mode.
