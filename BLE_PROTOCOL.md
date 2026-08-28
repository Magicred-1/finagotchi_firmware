# Finagotchi BLE Protocol

Device advertises as **`Finagotchi`**.

- Service: `0000f1a0-0000-1000-8000-00805f9b34fb`
- Characteristic: `0000f1a1-0000-1000-8000-00805f9b34fb` (READ + NOTIFY + WRITE)

## Device -> App (read / notify)

Value is a UTF-8 string: `<stage>:<streak>:<mood>:<item>:<points>:<happy>`

Example: `coinling:3:1:2:12500:87`

Sent on connect and whenever any field changes, plus when the day-based
streak rolls over at midnight.

> The string can exceed 20 bytes — negotiate MTU >= 64 or notifications
> will truncate.

### Field ids

**stage**: `egg` | `coinling` | `hodler` | `whale`

**mood** (emotion — order matches `expressions.ts`):

| id | mood |
|---|---|
| 0 | calm |
| 1 | happy |
| 2 | excited |
| 3 | waiting |
| 4 | sleepy |
| 5 | sad |

**item** (collectible — matches `PetAccessory.tsx`):

| id | item |
|---|---|
| 0 | none |
| 1 | crown |
| 2 | glasses |
| 3 | bowtie |
| 4 | halo |
| 5 | diamond |

## App -> Device (write)

Write one command per write, or several separated by `;`.
Keep writes under 20 bytes each or request a larger MTU.

| Command | Effect |
|---|---|
| `stage:egg` / `coinling` / `hodler` / `whale` | Morph to stage (easeOutQuint) |
| `stage:<1-5>` | Numeric stage (app store mapping: 1 egg, 2/3 coinling, 4 hodler, 5 whale) |
| `mood:<0-5>` | Blend to emotion (0.45 s ease) |
| `item:<0-5>` | Equip collectible (instant) |
| `look:<yaw>,<pitch>` | Steer gaze, degrees (yaw ±30, pitch ±25) |
| `look:off` | Resume idle gaze wander |
| `react:jump` / `spin` / `glow` / `dance` | Play a reaction animation |
| `points:<n>` | Set points (persisted in NVS, shown in the stats bar) |
| `happy:<0-100>` | Set happiness (RAM only, shown in the stats bar) |
| `streak:<n>` | Override displayed streak |

## On-screen stats bar

The bottom of the TFT shows a stats bar: flame + streak days, sparkle +
points (k-suffix over 10k), heart + happiness. Pet is scaled to R=88 and
centered slightly above middle to make room.

## Firmware rendering notes (what the device reproduces)

- Radial bodies from `profiles.ts` (64 samples, same-angle morph lerp)
- Sphere-projected eyes from `face.ts` (`eyePoses` tangent basis, real 3D
  foreshortening, depth fade approximated by blending toward the body color)
- Seeded blink schedule (`createRng(0x5eed)`, incl. double blinks) and
  `loopNoise` liveliness — the device blinks in sync with the app
- Expressions override gaze/split/eyes, blended over 0.45 s
- Glow = body x1.15 at 22% (PetBody), eyes #f5f5f5 (PetEyes)
- Reactions as keyframe tracks; sparkle burst on stage-up (LevelUpAnimation)
- Idle float from PetCanvas (4 s loop)

While an app is connected, the on-device demo auto-evolve is paused so the
app has full control of the stage. On disconnect, gaze is cleared and the
demo resumes.
