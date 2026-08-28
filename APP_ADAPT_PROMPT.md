# Prompt — adapt the existing Finagotchi app to sync with the hardware

Copy everything below the line into your AI coding assistant, inside the
mobile app repository. The app already exists — this is an integration task,
not a greenfield build.

---

## Role

You are a senior React Native (Expo) engineer working in this repo. The app
already has a working pet (`RadialPet` / `PetCanvas` / `FinagotchiEngine`),
a pet store (`features/pet/store.ts`), accessories, and reactions. Your job
is to add a **BLE sync layer** so the app drives the physical Finagotchi
device (ESP32-S3 with a 240×240 TFT showing the same radial pet).

**Do not rebuild or restyle the pet UI.** Wire BLE into the existing
architecture.

## The device contract (firmware already implements this — match exactly)

- Advertised name: **`Finagotchi`**
- Service: `0000f1a0-0000-1000-8000-00805f9b34fb`
- Characteristic: `0000f1a1-0000-1000-8000-00805f9b34fb` (READ + NOTIFY + WRITE)

**Device → app** notification value: UTF-8 string
`<stage>:<streak>:<mood>:<item>:<points>:<happy>`, e.g. `coinling:3:1:2:12500:87`.
Sent on connect and on any change. Can exceed 20 bytes — request MTU ≥ 64.

**App → device** writes, UTF-8, one command per write or several separated
by `;`, ≤ 20 bytes per write unless a larger MTU was negotiated:

| Command | When to send |
|---|---|
| `stage:<1-5>` | Store stage changes (1 egg, 2/3 coinling, 4 hodler, 5 whale) |
| `mood:<0-5>` | Engine mood changes (0 calm, 1 happy, 2 excited, 3 waiting, 4 sleepy, 5 sad — `expressions.ts` order) |
| `item:<0-5>` | Accessory equipped (0 none, 1 crown, 2 glasses, 3 bowtie, 4 halo, 5 diamond) |
| `look:<yaw>,<pitch>` | Drag-to-look, already throttled to ~10 Hz inside `RadialPet` — forward `onLook(yaw, pitch)` |
| `look:off` | `RadialPet`'s `onLookEnd` |
| `react:jump` / `spin` / `glow` / `dance` | When `PetCanvas` plays the matching reaction |
| `points:<n>` | Points change (device persists to NVS and shows them in its stats bar) |
| `happy:<0-100>` | Happiness changes (device stats bar, RAM only) |
| `streak:<n>` | App authoritative streak override (rarely needed; device computes day streaks itself) |

Session rule: while connected, the device pauses its demo and the app is
authoritative. On disconnect the device resumes its own demo.

## Tasks

### 1. BLE module (`ble/` or `features/device/`)

Using `react-native-ble-plx`:

- `useFinagotchiDevice()` hook exposing `{ status, device, send(cmd), lastState }`
  where `status` is `off | scanning | connecting | connected | disconnected`.
- Scan filtered by name `Finagotchi`, connect, discover services, request
  MTU 64 (Android), subscribe to the characteristic.
- **Serialize all writes through a queue** — never overlap writes on one
  characteristic. Failed writes log and drop, never throw into the UI.
- Auto-reconnect with exponential backoff (1 s → 2 s → 4 s → … → 30 s cap)
  while the pet screen is mounted; stop when the app backgrounds.
- Handle Bluetooth powered-off with a visible status pill.

### 2. Store wiring (`features/pet/store.ts`)

Subscribe to store changes and mirror them to the device when connected:

- `stage` → `stage:<n>`
- mood (after the existing label → engine-mood mapping in `PetCanvas`:
  sleeping→sleepy, proud→happy) → `mood:<id>`
- `accessory` → `item:<id>`
- reactions (the `reaction` prop) → `react:<name>`

**Loop prevention:** when a device notification causes a store update, tag it
(e.g. `applyRemoteState()` action or an `origin: 'device'` flag) so the
outgoing subscription does not echo it back to the device.

On connect, push a snapshot of the current state, batched with `;`:
`stage:<n>;mood:<m>;item:<i>`

### 3. Notifications → store

Parse `<stage>:<streak>:<mood>:<item>:<points>:<happy>` from notifications and apply to the
store via the remote-tagged path: stage name → numeric stage
(egg=1, coinling=2, hodler=4, whale=5), streak → streak display, mood id →
engine mood, item id → accessory. This covers device-side changes (e.g. the
midnight streak rollover) and multi-client sanity.

### 4. Look gestures

`PetCanvas` already accepts `onLook` / `onLookEnd` and `RadialPet` throttles
to ~10 Hz and clamps ±30°/±25°. Wire them to `send(\`look:${yaw.toFixed(0)},${pitch.toFixed(0)}\`)`
and `send('look:off')`. Do not add your own throttle.

### 5. UX surface

- Small connection pill above the pet: green dot + "Linked" when connected,
  gray "Offline" otherwise; tap to rescan/reconnect.
- No blocking UI: the pet screen must work fully offline; BLE is additive.
- Battery-friendly: stop scanning when connected or backgrounded.

### 6. Permissions / build

- `app.json`: iOS `NSBluetoothAlwaysUsageDescription`; Android 12+
  `BLUETOOTH_SCAN` (`neverForLocation`) + `BLUETOOTH_CONNECT`, plus
  `ACCESS_FINE_LOCATION` maxSdkVersion 30 for legacy.
- BLE does **not** work in Expo Go — add `react-native-ble-plx` to plugins,
  `npx expo prebuild`, build a dev client (`npx expo run:android` / `run:ios`).
- `__DEV__` mock mode: fake connected device that logs writes and emits a
  notification echo, so UI work needs no hardware.

## Acceptance criteria

1. App connect → device pauses demo, receives snapshot, mirrors app pet.
2. Change stage/mood/accessory in the app → device morphs within a frame or
   two (0.45 s morph is expected).
3. Drag on the app pet → device pet's eyes track the finger; release →
   device resumes wander.
4. Trigger a reaction → device plays its matching animation.
5. Kill Bluetooth mid-session → app shows Offline, device resumes its demo;
   re-enable → auto-reconnect and re-sync.
6. No write ever reaches the UI thread unqueued; no echo loops (device →
   store → device) in logs.

## Reference

The full wire contract with field-id tables is in the firmware repo's
`BLE_PROTOCOL.md`; the device-side parity notes are in `APP_GUIDELINES.md`.
Paste them here if the assistant needs them.
