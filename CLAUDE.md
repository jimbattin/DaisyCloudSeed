# DaisyCloudSeed Project Documentation

## Project Overview

This project implements a single guitar effect DSP for the Electrosmith Daisy Seed board mounted in a PedalPCB Terrarium guitar pedal enclosure:

1. **CloudSeed** - Advanced algorithmic reverb with up to 5 delay lines, 10 presets, and extensive control

## Directory Structure

```
DaisyCloudSeed/
├── CloudSeed/             # Core reverb algorithm library (builds libcloudseed.a)
│   ├── AudioLib/          # Audio utilities (Biquad, filters, ShaRandom)
│   ├── Utils/             # SHA256 utilities
│   ├── build/             # Library build artifacts (generated, not checked in)
│   └── Makefile           # Library build configuration
│
├── libdaisy/              # Hardware abstraction layer (Git submodule)
├── DaisySP/               # DSP library (Git submodule)
├── Terrarium/             # Hardware definitions (Git submodule)
│
├── build/                 # Application build artifacts (generated, not checked in)
├── Makefile               # App build (BOOT_SRAM; dependent libraries built with the `libs` target)
├── src/                   # Firmware sources
│   ├── cloudseed.cpp          # main(), pedal state, controls, audio callback, preset load
│   ├── pedal_leds.h/.cpp      # LED1/LED2: preset blink, save/restore confirmation, FatalErrorLoop()
│   ├── pedal_storage.h/.cpp   # QSPI Settings + UserPresets (PedalStorage), firmware hash
│   ├── sdram_pool.h/.cpp      # custom_pool_allocate() SDRAM bump pool + boot TOML parse arena
│   ├── knob_bank.h            # KnobBank absolute knob takeover (park, 50 ms glide, track; host-portable)
│   ├── toggle_bank.h          # ToggleBank parked toggle takeover (host-portable)
│   ├── footswitch_gestures.h  # FootswitchGestures: FS1/FS2 tap, hold, 5 s save and restore chords (host-portable)
│   ├── preset_bank.h          # PresetBank/PresetData types + knob/toggle-map types + ParsePresetBank()
│   ├── preset_bank.cpp        # TOML -> PresetBank parser (host-portable, no libdaisy)
│   └── presets_toml.s         # .incbin that embeds presets.toml into the firmware image
├── tests/                 # Host unit tests (`make test`)
│   ├── check.h                # Minimal CHECK() macro + summary
│   ├── knob_bank_test.cpp
│   ├── toggle_bank_test.cpp
│   ├── footswitch_gestures_test.cpp
│   ├── preset_bank_test.cpp
│   └── fixtures/two_presets.toml  # Parser fixture (Chorus + Through the Looking Glass)
├── docs/
│   ├── PERFORMANCE.md         # Record of applied performance/correctness fixes (with file/line anchors)
│   └── pedal.png              # Pedal/control artwork
├── presets.toml           # THE preset data - all 10 presets, parsed at boot
├── third_party/tomlc99/   # Vendored TOML parser (MIT, commit in README.txt)
├── tools/                 # preset_check.cpp (host-side presets.toml validator)
├── CLAUDE.md              # This file - agent-facing project documentation
├── README.md              # User-facing control table and build/flash instructions
├── license.txt            # License
└── .gitmodules            # Submodule definitions (DaisySP, libdaisy, Terrarium)
```

## Hardware Platform

### Daisy Seed Specifications
- **MCU**: STM32H750 (Cortex-M7, 480MHz)
- **RAM**: 512KB internal SRAM
- **SDRAM**: 64MB external (48MB allocated to CloudSeed)
- **Flash**: 8MB QSPI
- **FPU**: Hard float, double precision
- **Audio**: 24-bit, up to 96kHz (typically 48kHz)

### Terrarium Pedal Controls

**Knobs** (6 potentiometers):
- KNOB_1 through KNOB_6 = ADC channels 0,2,4,1,3,5 (`Terrarium/terrarium.h:20-28`); these are
  indices into `hw.knob[]`, wired in `DaisyPetal::InitAnalogControls()`
  (`libdaisy/src/daisy_petal.cpp:313-335`)

**Switches** (4 toggles):
- SWITCH_1 through SWITCH_4 = 2, 1, 0, 6 (`Terrarium/terrarium.h:10-18`). These are indices
  into `hw.switches[]`, **not** pin numbers; `DaisyPetal::InitSwitches()` maps them to Daisy
  Seed pins D10, D9, D8, D7 (`libdaisy/src/daisy_petal.cpp:12-18`, `:279-292`)

**Footswitches** (2):
- FOOTSWITCH_1 = `hw.switches[4]` (seed pin D25) - Bypass/Active
- FOOTSWITCH_2 = `hw.switches[5]` (seed pin D26) - Preset cycling

**LEDs** (2):
- LED_1 = seed pin 22, used via `hw.seed.GetPin(Terrarium::LED_1)` - Active indicator (on when not bypassed)
- LED_2 = seed pin 23 - Preset indicator (blinks N times for preset N, continuous with 5s pause)

**Audio**:
- Mono input/output (uses left channel only)

## CloudSeed Effect

### Architecture

CloudSeed is based on the open-source CloudSeed VST plugin by ValdemarOrn, modified for mono processing on embedded hardware.

**Key Features**:
- Delay line count toggled per preset between `default_delay_lines` and `max_delay_lines`
  (both set in presets.toml; assignable via `[preset.toggle_map]`, default SWITCH_1)
- 10 presets defined in [presets.toml](presets.toml), embedded in the image and parsed at boot
- Early and late reverberation stages
- Extensive modulation and diffusion
- 48MB SDRAM buffer allocation
- Preset and bypass state persisted to QSPI flash
- Per-preset user save (FS1 held 5 s) and factory restore (FS1 + FS2 held 5 s), stored in QSPI
  and discarded when a different firmware image is flashed
- Equal-power makeup gain on the wet path
- FPU flush-to-zero enabled at boot (denormal stall elimination)

### Control Mapping

File: [src/cloudseed.cpp](src/cloudseed.cpp) + `[preset.knob_map]` / `[preset.toggle_map]` in
[presets.toml](presets.toml)

Knobs and toggle switches are **not** hard-wired. Each preset's `[preset.knob_map]` /
`[preset.toggle_map]` names a primary (`_a`) and a secondary (`_b`) target per knob/toggle; the
secondary bank is selected while **the preset footswitch (FOOTSWITCH_2) is held**. The shipped
default map in every preset reproduces the historical assignment:

```cpp
// Knob read: src/cloudseed.cpp:453-455; scan + dispatch: src/cloudseed.cpp:343-352;
// applyKnobTarget(): src/cloudseed.cpp:215-248
//                        primary (_a)                      secondary (_b)
KNOB_1: output.DryOut                        | input.PreDelay
KNOB_2: output.EarlyOut                      | input.HighPass   (+ HiPassEnabled on first touch)
KNOB_3: output.MainOut                       | input.LowPass    (+ LowPassEnabled on first touch)
KNOB_4: late_diffusion.LateDiffusionFeedback | late.LineModAmount
KNOB_5: early.TapDecay                       | late.LineModRate
KNOB_6: late.LineDecay                       | reverse.delay    (20-2000 ms reverse window)

// Toggle read: src/cloudseed.cpp:456-458; scan + dispatch: src/cloudseed.cpp:356-360;
// applyToggleTarget(): src/cloudseed.cpp:252-267. Primary (_a) = secondary (_b) in every shipped preset.
SWITCH_1: "delay_lines.max"    off = the preset's default_delay_lines, on = its max_delay_lines
SWITCH_2: "early.isReverse"    Bloom: reverses the early tap gain order
SWITCH_3: "reverse.enabled"    reverse voice on/off (window length set by whichever knob maps to
          "reverse.delay"; see CloudSeed/ReverseDelay.h)
SWITCH_4: "reverse.direct_mix" off = reverse feeds the reverb wet path, on = direct output mix

FOOTSWITCH_1 tap: Bypass toggle on RELEASE (persisted to flash 3 s later)
FOOTSWITCH_1 held 5 s: save the current engine state into the current preset (fires while held;
          the release does not toggle bypass)
FOOTSWITCH_2 tap: Preset cycle on RELEASE
FOOTSWITCH_2 held: secondary knob and toggle bank; the release cycles the preset only if no
          secondary knob or toggle wrote a value during the hold
FOOTSWITCH_1 + FOOTSWITCH_2 held 5 s: restore the current preset to its presets.toml values
          (fires while held). Once both have been down together neither release toggles
          bypass or cycles the preset (FootswitchGestures, src/footswitch_gestures.h)
```

**Footswitch gestures** ([src/footswitch_gestures.h](src/footswitch_gestures.h)): libdaisy's `Switch` is
an 8-bit shift register clocked once per audio block: `Pressed()` is `state_ == 0xff` and clears
1 ms after a release, while `FallingEdge()` is `state_ == 0x80` and only fires 7 ms later
(`libdaisy/src/hid/switch.h:70-79`, `switch.cpp:44-47`). `FootswitchGestures` tracks both
switches with private `bypassHeld` / `presetHeld` flags, each set on `Pressed()` and cleared
only on that switch's `FallingEdge()`, so a contact bounce mid-hold (which never produces `0x80`)
cannot drop a hold, and FS2's 6 ms tail before the edge stays in bank 1 (`PresetHeld()`).
`Update()` is called once per callback with all four accessors and returns a
`FootswitchEvents` struct:
- `toggleBypass`: FS1's release edge, unless its 5 s save already fired or the press was part of
  a chord
- `cyclePreset`: FS2's release edge, unless a secondary knob or toggle wrote since the press
  (`MarkEdited()`) or the press was part of a chord
- `savePreset`: FS1 held alone for `kLongHoldBlocks` (5000 blocks = 5 s), counted from its
  `Pressed()` latch; fires once per hold, while still held
- `restorePreset`: both held for `kLongHoldBlocks` consecutive blocks; fires once, while held

A **chord** starts on the first block both holds are set and lasts until both are released, so
presses and releases may be arbitrarily staggered: while it lasts neither release fires, and a
chord shorter than 5 s does nothing at all. An FS1 hold that turns into a chord no longer counts
toward a save. A falling edge without a prior `Pressed()` is ignored. The callback calls
`MarkEdited()` when a bank-1 `Scan()` returns at least one knob or toggle write
(`src/cloudseed.cpp:365-366`), i.e. a secondary knob moved past `kKnobMoveThreshold` and wrote, or a
secondary toggle was flipped; brushing a knob below the threshold does not cancel the preset
change, and entering bank 1 re-parks both (zero writes), so pressing FS2 is never itself an edit.
The bank is derived from `PresetHeld()` after `Update()` in the same callback (`src/cloudseed.cpp:460`),
so the block that ends a hold already scans in bank 0. `processFootswitches()`
(`src/cloudseed.cpp:306-331`) reads each accessor once per callback,
because edge flags are only valid for the update in which they occur, and turns the events into
`state.bypass`, `state.triggerPresetChange`, `gSaveRequested`, and `state.triggerPresetRestore`.

**Knob take-over** ([src/knob_bank.h](src/knob_bank.h)): knobs are **absolute** - once a knob has taken
over, its position *is* the parameter value - but **parked** across transitions.
`KnobBank` is a class whose only public member is `Scan()`; its private `Reset()` snapshots every knob position and parks it; a parked knob writes nothing, so
power-up, a preset load, and entering or leaving the secondary bank never change a parameter on
their own, and a freshly loaded preset sounds exactly as authored until a knob is turned. A knob
goes live once it moves `kKnobMoveThreshold` (0.01 of travel) from its snapshot. It then
**glides** its target linearly from the target's current value to the pot's (possibly still
moving) position over `kKnobGlideBlocks` (50) audio blocks = 50 ms, landing exactly on the pot.
The glide exists because engine parameters are not smoothed downstream
(`ReverbChannel::SetParameter` assigns the output levels directly,
`CloudSeed/ReverbChannel.h:308-318`), so a step would click. After the glide the knob writes its
own position whenever that changes by `kKnobApplyEpsilon` (0.001), which keeps ADC noise off the
engine.

Positions within `kKnobRailWindow` (0.003) of a stop are written as exactly 0.0 / 1.0: the top
ADC reading is at most 65535/65536, libdaisy documents ~0.002 of bleed at the bottom of the pots
(`libdaisy/src/hid/ctrl.cpp:3-4`), and the `Response2Dec` output levels are only silent below
0.00025. Rail values bypass the epsilon, so the last fraction of travel into a stop always lands
- this is what makes KNOB_1/2/3 at their CCW stops silence the pedal. The parked test uses the
raw position, so a knob *resting* against a stop at a transition stays parked and cannot slam its
target to the rail. A `Reset()` during a glide cancels it and leaves the target where the glide
had got to.

Leaving the secondary bank re-parks too: a knob dialled in bank 1 is parked in bank 0, and its
next turn glides its primary target to the knob's position - a smooth jump, by design. There is
deliberately no pickup/crossing rule (it reintroduces the dead-zone feel).

`KnobBank::Scan()` is the whole per-callback decision, called from `updateEngineControls()`
(`src/cloudseed.cpp:343-352`): it re-parks on a bank change (`bank != activeBank`), when
`state.controlResetPending` is set (by `loadPresetGated()` before the load,
`src/cloudseed.cpp:202-212`), or on the first call; a re-parking call returns zero writes. It never
runs while `state.presetChangeInProgress` is set (the callback passes audio through and returns
first, `src/cloudseed.cpp:448-451`) because the re-park must not straddle a `state.knobMap` rewrite;
`controlResetPending` simply stays set until the load completes.
The glide start values come from `knobTargetValue()` (`src/cloudseed.cpp:272-293`), read for the
active bank every callback: knob values are stored verbatim in `parameters[]` by `SetParameter`,
so `GetAllParameters()` read-back is exact; the `reverse.delay` pseudo-target has no
`parameters[]` slot and is tracked in `state.reverseDelayNorm` instead (set by `loadPreset()`
from the preset's `[preset.params.reverse] delay`, then by the knob). A disabled input filter
reports its open end instead of its stored cutoff (`HighPass` 0.0 while `HiPassEnabled` < 0.5,
`LowPass` 1.0 while `LowPassEnabled` < 0.5): the engine skips it, so that is what is heard, and
`applyKnobTarget()` enables it on the first write, so the glide opens from there rather than
stepping to the stored cutoff (about 2 kHz in Chorus and Dark Plate).

`main()` settles the knob one-poles for 200 ms between `hw.StartAdc()` and `hw.StartAudio()`
(`src/cloudseed.cpp:562-575`). `AnalogControl` starts at 0.0 and only converges while
`ProcessAnalogControls()` runs, which otherwise first happens inside the audio callback: without
the settle loop the first snapshot would capture ~5 % of each real knob position and the
settling ramp itself would be read as a deliberate turn at every power-up.

**ADC smoothing**: libdaisy's default `AnalogControl` slew computes to `coeff_ = 1.0` at this
callback rate (`libdaisy/src/hid/ctrl.cpp:16` with a 1 kHz update and the 0.002 s default),
i.e. no filtering. `main()` re-tunes every knob to `KNOB_SMOOTHING_COEFF` (0.05, ~20 ms) at
`src/cloudseed.cpp:546-547`. Nothing may call `hw.SetAudioBlockSize()` / `hw.SetAudioSampleRate()`
afterwards: both re-run `SetHidUpdateRates()` and overwrite the coefficient.

**Toggle take-over** ([src/toggle_bank.h](src/toggle_bank.h)): the four switches are also assignable
per preset (`[preset.toggle_map]`) and take over the same way knobs park, but with no glide -
every toggle target is on/off. `ToggleBank::Scan()` snapshots and parks every lever on the first
call, on a bank change, or when `forceReset` is set, exactly like `KnobBank::Scan()`; a
re-parking call returns zero writes. Once primed, a lever that differs from its parked snapshot
writes its position (up = `true`) immediately (no move threshold, no epsilon: the target is
binary so there is nothing to debounce beyond the snapshot compare) and keeps writing on every
later change. `updateEngineControls()` calls `gToggles.Scan(bank, reset, togglePositions,
toggleWrites)` right after the knob scan, reusing the same `reset` flag
(`state.controlResetPending`) and `bank` (`src/cloudseed.cpp:356-360`), then dispatches each write
through `applyToggleTarget()` (`src/cloudseed.cpp:252-267`): the three pseudo targets
(`delay_lines.max`, `reverse.enabled`, `reverse.direct_mix`) set a `PedalState` bool read
elsewhere in the callback, and `ToggleTarget_Param` calls `reverb->SetParameter()` directly (no
`outputLevelsDirty`: no toggle parameter feeds the makeup gain).

A toggle that targets `input.HiPassEnabled` or `input.LowPassEnabled` **owns** that filter's
enable: `loadPreset()` computes `state.hiPassOwnedByToggle` / `state.lowPassOwnedByToggle` from
`state.toggleMap` (`src/cloudseed.cpp:157-164`, `:191-192`), and while true `applyKnobTarget()` no
longer auto-enables the filter on the knob's first touch and `knobTargetValue()` glides the knob
from the stored cutoff instead of the open end (`src/cloudseed.cpp:229-238`, `:280-288`) - the toggle
is the only thing that switches the filter on or off.

### Presets

All preset data lives in [presets.toml](presets.toml) at the repo root. The file is embedded
into the firmware image by `src/presets_toml.s` (`.incbin`, lands in `.rodata` → SRAM) and parsed
once at boot into the static `gPresets` bank (`src/cloudseed.cpp:57`). There is no filesystem:
presets.toml is the **factory** version of every preset and is never modified on the pedal.
Changing a factory preset means editing the TOML and reflashing; a sound saved on the pedal
(FS1 held 5 s) is stored separately in QSPI and overrides its preset's engine values until it is
restored (see "User preset save / factory restore" below).

1. Chorus (1 blink)
2. Dull Echos (2 blinks)
3. Hyperplane (3 blinks)
4. Medium Space (4 blinks)
5. Noise in the Hallway (5 blinks)
6. Rubi Ka Fields (6 blinks)
7. Small Room (7 blinks)
8. 90s Are Back (8 blinks)
9. Through the Looking Glass (9 Blinks)
10. Dark Plate (10 Blinks)

All presets allow 5 delay lines except "Through the Looking Glass"
(`max_delay_lines = 4.0`) - it is CPU-intensive and crackles above that.

**Preset System Architecture**:
- Each `[[preset]]` carries `name`, `blinks`, `led_on_ms`, `led_off_ms`, `led_pause_ms`,
  `default_delay_lines`, `max_delay_lines`, a `[preset.knob_map]` table, a `[preset.toggle_map]`
  table, eight `[preset.params.*]` groups holding all 46 file-controlled parameters (see the
  parameter reference in the TOML header - `isReverse` (Bloom) is now a file parameter, in
  `[preset.params.early]`), a ninth group `[preset.params.reverse]` (`delay`, `enabled`,
  `direct_mix`), and a tenth `[preset.params.delay_lines]` (`max`). None of the last two groups
  has a `Parameter` slot, so `parseParams()` parses them separately from `kGroups`
  ([src/preset_bank.cpp](src/preset_bank.cpp))
- `[preset.knob_map]` requires all twelve `knobN_a` / `knobN_b` keys. Each value is a quoted
  `"group.Parameter"` naming a parameter of that same group, or the pseudo-target
  `"reverse.delay"`. Parsed by `parseKnobMap()`/`parseKnobTarget()`
  ([src/preset_bank.cpp](src/preset_bank.cpp)) into `PresetData::knobMap[bank][knob]`
- `[preset.toggle_map]` requires all eight `toggleN_a` / `toggleN_b` keys (N = 1..4 =
  SWITCH_1..SWITCH_4). Each value is a quoted `"group.Parameter"` naming an on/off parameter
  (`kToggleParams`: `isReverse`, the four filter/diffusion/shelf/cutoff enables, the two
  diffusion stage counts, `LateStageTap`, `Interpolation`) or one of the three pseudo-targets
  `"delay_lines.max"`, `"reverse.enabled"`, `"reverse.direct_mix"`. Parsed by
  `parseToggleMap()`/`parseToggleTarget()` ([src/preset_bank.cpp](src/preset_bank.cpp)) into
  `PresetData::toggleMap[bank][toggle]`; `splitTarget()` / `resolveParamTarget()` / `groupIs()`
  are shared with the knob-map parser
- `LineCount` alone must NOT appear in the file: it is driven live by the `"delay_lines.max"`
  toggle target, which selects `default_delay_lines` (off) or `max_delay_lines` (on). The parser
  rejects it
- `default_delay_lines` and `max_delay_lines` are each a whole number 1..5 (`readLineCount()`,
  [src/preset_bank.cpp](src/preset_bank.cpp)), and `default_delay_lines` must not exceed
  `max_delay_lines` - the toggle's off state must never exceed a preset's CPU cap
- Parsed by `ParsePresetBank()` ([src/preset_bank.cpp](src/preset_bank.cpp)) into `PresetBank`
  (`count` + `PresetData[16]`); `PresetData::params[]` is indexed by `(int)Parameter`
- Applied with `CloudSeed::ReverbController::LoadPreset()`
  (`CloudSeed/ReverbController.h:47`), which copies every slot except `LineCount`
  and then re-applies all 47 through `SetParameter`
- Cycles using modulo operator: `(currentPreset + 1) % gPresets.count` (`src/cloudseed.cpp:602`)
- Preset index is persisted to QSPI flash, so **the order in presets.toml is frozen**;
  reordering or deleting entries requires bumping `SETTINGS_VERSION` (`src/pedal_storage.cpp:5`)
- `loadPreset()` (`src/cloudseed.cpp:168-198`) is the only reader of `gPresets` after boot. It loads
  the preset's saved user edit if its `UserPreset` slot is valid, else the factory values:
  `LoadPreset()` with the chosen `params`, `applyReverseWindow()` with the chosen reverse window,
  and `state.delayLinesMax` / `state.reverseEnabled` / `state.reverseDirectMix` from the chosen
  toggle pseudo-values. It then copies the preset's `knobMap`, `toggleMap`, `defaultDelayLines`,
  `maxDelayLines` and blink timings (never user-edited) into `PedalState`, derives
  `hiPassOwnedByToggle` / `lowPassOwnedByToggle` via `toggleMapTargets()`
  (`src/cloudseed.cpp:157-164`), and sets `state.outputLevelsDirty`. The audio callback and
  `ServicePresetBlink()` (handed `state.blinkPattern` by the main loop) read only those cached copies
- The delay-line count is applied in the audio callback (`updateEngineControls()`) as
  `state.delayLinesMax ? state.maxDelayLines : state.defaultDelayLines` (`src/cloudseed.cpp:371-376`)
- LED2 blinks continuously to indicate active preset (N blinks = preset N)
- A parse failure is unrecoverable: `FatalErrorLoop()` (`src/pedal_leds.cpp:45-54`, called at
  `src/cloudseed.cpp:527`) blinks both LEDs at 5 Hz forever and never starts audio. The same loop reports an
  exhausted SDRAM pool and an unexpected audio block size. `make` validates presets.toml
  before embedding it, so a rejected file cannot be built into firmware in the first place

**Boot-time memory**: the parser allocates exclusively from a 512 KB bump arena carved from the
head of `custom_pool` (`src/sdram_pool.cpp:48-57`), used between `hw.Init()` and
`new CloudSeed::ReverbController(...)`. Peak measured usage is 141,696 B on x86-64 (smaller on
32-bit ARM); the arena is abandoned - not freed - so the SDRAM pool starts at offset 0 for the
reverb. Permanent SDRAM cost of the TOML system: zero.

### Preset Persistence

The current preset and the bypass state are both automatically saved to and loaded from QSPI flash memory, so they persist across power cycles. Writes are coalesced: flash is written `SETTINGS_SAVE_DELAY_MS` (3000 ms) after the last change.

**Implementation** ([src/pedal_storage.h](src/pedal_storage.h),
[src/pedal_storage.cpp](src/pedal_storage.cpp)): `PedalStorage` owns both
`PersistentStorage` instances. It is constructed in `src/cloudseed.cpp` as
`gStorage(hw.seed.qspi)` (`:118`), right after `hw`, because `PersistentStorage` needs the
board's `QSPIHandle&` at construction.

**Settings Structure** (`src/pedal_storage.h:14-28`):
```cpp
constexpr int SETTINGS_VERSION = 2;                 // src/pedal_storage.cpp:5
constexpr uint32_t SETTINGS_SAVE_DELAY_MS = 3000;  // src/pedal_storage.cpp:9

struct Settings {
    int  version;        // SETTINGS_VERSION for compatibility checking
    int  currentPreset;  // 0 .. gPresets.count-1
    bool bypass;         // Persisted bypass state (true = bypassed at last save)
    bool operator!=(const Settings& a) const;  // Required by PersistentStorage
};
```

**Storage Management**:
- Uses Daisy's `PersistentStorage<Settings>` class with QSPI flash
- Version control ensures compatibility when Settings struct changes
- Automatic defaults restoration if version mismatch detected
- Settings validated on load (invalid presets default to 0)

**Save/Load Workflow**:
1. **On startup**: `main()` (`src/cloudseed.cpp:549-554`) calls `gStorage.Init()`
   (`src/pedal_storage.cpp:33-46`), then takes `RestoredPreset(gPresets.count)` /
   `RestoredBypass()` into `state` and calls `loadPreset()`
2. **On preset change / bypass toggle**: nothing is written yet. `loadPresetGated()` updates
   `state.currentPreset` in the main loop; the audio callback flips `state.bypass`
   (`src/cloudseed.cpp:314-317`)
3. **In main loop**: `gStorage.ServiceSettingsSave(state.currentPreset, state.bypass)`
   (`src/pedal_storage.cpp:61-74`, called on every pass at `src/cloudseed.cpp:606`) mirrors
   both values into the RAM copy and restarts a `SETTINGS_SAVE_DELAY_MS` timer whenever they
   differ. Once 3 s pass without another change it saves the `Settings` store, outside the
   audio callback. A burst of preset/bypass changes therefore costs one QSPI sector erase, and
   `Save()` skips the erase entirely when flash already matches; a power-off within 3 s of a
   change loses that change (the pedal boots in the last saved state)

**Resilience Features**:
- Invalid preset indices automatically default to preset 0 (Chorus)
  (`PedalStorage::RestoredPreset()`, `src/pedal_storage.cpp:48-51`);
  the corrected index is written back to flash 3 s after boot
- Version mismatch triggers `RestoreDefaults()` (`src/pedal_storage.cpp:43-44`); the defaults are then
  read straight-line, with no reload
- First boot / version mismatch defaults to preset 0 and `bypass = true` (`src/pedal_storage.cpp:34`)
- LED1 is re-synced to the restored bypass state after load (`src/cloudseed.cpp:558-559`)
- Non-blocking: flash writes happen in the main loop, not the audio callback
- Settings survive power cycles, firmware updates, and manual resets

**To add more persistent settings**: add the field to the `Settings` struct (`src/pedal_storage.h`), extend its `operator!=`, increment `SETTINGS_VERSION`, add a `PedalStorage` accessor that `main()` restores into `state` (like `RestoredBypass()`), and extend `ServiceSettingsSave()` (a new parameter, mirrored into the RAM copy and included in the change test).

### User preset save / factory restore

A sound can be saved into the current preset on the pedal and later reverted to its
presets.toml version. Saved edits survive power cycles but not a firmware change.

**Storage** (`src/pedal_storage.cpp:11-13`, `src/pedal_storage.h:10-11`, `:30-52`, `:55-80`):
`PedalStorage`'s `PersistentStorage<UserPresets>` sits at QSPI offset `USER_PRESETS_QSPI_OFFSET` (0x1000). That is the 4 KB sector
after `Settings`, which sits at offset 0 in sector 0. The Daisy bootloader keeps programs at
0x90040000 and never touches the first 256 KB
(`libdaisy/doc/md/_a7_Getting-Started-Daisy-Bootloader.md:82`), and `QSPIHandle::Erase` works in
4 KB sectors, so each store erases only its own sector.
```cpp
struct UserPreset {
    float    params[(int)::Parameter::Count];  // reverb->GetAllParameters() at save time
    float    reverseDelay;                     // state.reverseDelayNorm at save time
    float    delayLinesMax;                    // "delay_lines.max", 0.0 or 1.0
    float    reverseEnabled;                   // "reverse.enabled", 0.0 or 1.0
    float    reverseDirectMix;                 // "reverse.direct_mix", 0.0 or 1.0
    uint32_t valid;                            // USER_PRESET_VALID, else load from presets.toml
};  // 208 B
struct UserPresets { uint32_t firmwareHash; UserPreset presets[kMaxPresets]; }; // 3332 B
```

- Every field is 4 bytes wide, so there is no padding and `operator!=` is a `memcmp`. A
  `static_assert` keeps `sizeof(UserPresets)` + the 4 B `PersistentStorage` state word within
  one sector; it breaks if `kMaxPresets` goes above 19
- A slot is used only when `valid == USER_PRESET_VALID` (1); erased flash reads 0xFFFFFFFF.
  `valid` is the last field, and QSPI pages are programmed in ascending order, so a write cut
  off by power loss can leave a slot invalid (factory) but never half-written
- `params` is the full `GetAllParameters()` snapshot. `LineCount` comes along but is ignored,
  because `LoadPreset()` skips it. `isReverse` is preset data now and is saved for real. The
  knob map, toggle map, `default_delay_lines`/`max_delay_lines`, and blink timing are not saved

**Firmware identity** (`firmwareImageHash()`, `src/pedal_storage.cpp:24-31`): FNV-1a over the words
between the linker symbols `_stext` and `_etext`, which bound `.text` + `.rodata`
(`libdaisy/core/STM32H750IB_sram.lds:36,47`) and include the embedded presets.toml.
`PedalStorage::Init()` (`src/pedal_storage.cpp:33-46`, called from `main()` at
`src/cloudseed.cpp:551` before the first `loadPreset()`) calls `RestoreDefaults()` (one sector erase) when the stored hash differs.
Any code or presets.toml change therefore discards every saved preset. A byte-identical reflash
keeps them, because nothing distinguishes it from a reboot. `Settings` (preset index, bypass)
are unaffected by the hash.

**Save** (FS1 held 5 s): `processFootswitches()` sets `gSaveRequested`. On the next callback
with the preset-change gate open, the callback copies `GetAllParameters()`, `state.reverseDelayNorm`
and the three toggle pseudo-values into `gSaveSnapshot`, marks it valid, and publishes it with
`state.saveSnapshotReady` (`src/cloudseed.cpp:465-475`). The main loop (`:580-587`) copies the
snapshot into `gStorage.UserSlot(state.currentPreset)`, clears the flag, calls
`gStorage.SaveUserPresets()` (a
blocking erase + write in the main loop; audio keeps running from SRAM), and starts the
confirmation blink.

**Restore** (FS1 + FS2 held 5 s): the callback sets `state.triggerPresetRestore`. The main
loop (`src/cloudseed.cpp:588-594`) clears the slot to `UserPreset{}` (invalid), reloads the preset through
`loadPresetGated()`, which parks the knobs and toggles and drops any unsaved tweaks, calls
`SaveUserPresets()`, and starts the confirmation blink. The save is skipped when the slot was already empty.

Both handlers run before the preset-change block in the main loop, so a snapshot is always
stored into the preset that was loaded when it was taken.

**Confirmation**: `StartConfirmBlink()` / `ServiceConfirmBlink()` (`src/pedal_leds.cpp:116-139`) drive
LED1 and LED2 together for `CONFIRM_BLINKS` (3) on/off cycles of `CONFIRM_BLINK_MS` (80 ms),
even when bypassed. While the blink runs, `ServicePresetBlink()` is skipped. When it ends, LED1 is
re-set to the bypass state and LED2 goes back to the preset pattern. This is distinct from
`FatalErrorLoop()`, which blinks both LEDs at 5 Hz forever.

### Parameters

47 reverb parameters enumerated in [CloudSeed/Parameter.h](CloudSeed/Parameter.h) (`Parameter::Count` == 47, `CloudSeed/Parameter.h:8-88`):

**Input Stage**: InputMix, PreDelay, HighPass, LowPass

**Early Reverb**: TapCount, TapLength, TapGain, TapDecay, isReverse, DiffusionEnabled, DiffusionStages, DiffusionDelay, DiffusionFeedback

**Late Reverb**: LineCount, LineDelay, LineDecay, LateDiffusionEnabled, LateDiffusionStages, LateDiffusionDelay, LateDiffusionFeedback

**Modulation**: EarlyDiffusionModAmount, EarlyDiffusionModRate, LineModAmount, LineModRate, LateDiffusionModAmount, LateDiffusionModRate

**Output**: DryOut, PredelayOut, EarlyOut, MainOut

**Frequency Response**: PostLowShelfGain, PostLowShelfFrequency, PostHighShelfGain, PostHighShelfFrequency, PostCutoffFrequency

**Seeds/Switches/Effects**: TapSeed, DiffusionSeed, DelaySeed, PostDiffusionSeed, CrossSeed, HiPassEnabled, LowPassEnabled, LowShelfEnabled, HighShelfEnabled, CutoffEnabled, LateStageTap, Interpolation

### Memory Architecture

CloudSeed requires massive delay buffers:

```cpp
// src/sdram_pool.cpp:19-38
static constexpr size_t alignUp8(size_t n) {
    return (n + 7u) & ~static_cast<size_t>(7u);
}

constexpr size_t CUSTOM_POOL_SIZE = 48u * 1024u * 1024u;  // 48MB
DSY_SDRAM_BSS __attribute__((aligned(32))) static char custom_pool[CUSTOM_POOL_SIZE];
static size_t pool_index = 0;

void* custom_pool_allocate(size_t size) {
    const size_t aligned = alignUp8(size);
    if (aligned > CUSTOM_POOL_SIZE - pool_index)
        FatalErrorLoop();  // callers placement-new into the result; 0x0 is ITCMRAM on the H750
    void* ptr = &custom_pool[pool_index];
    pool_index += aligned;
    return ptr;
}
```

This custom allocator manages SDRAM for delay lines. It is declared in
[src/sdram_pool.h](src/sdram_pool.h). The reverse voice's `reverseDelayBuffer` is a separate
`DSY_SDRAM_BSS` array in `src/cloudseed.cpp:129`, outside the pool.

Bump allocator, no free. The pool is aligned to the 32-byte M7 D-cache line and every block is
rounded up to 8 bytes. Exhaustion is a fatal boot error (`FatalErrorLoop()`, both LEDs at 5 Hz)
rather than a null return, because a write through 0x0 would land silently in ITCMRAM.
`custom_pool_allocate` keeps external linkage and this exact signature: the CloudSeed headers
declare it `extern`. Callers placement-new into it (e.g.
`CloudSeed/ModulatedDelay.h:41-42`), so destructors of pool-backed objects must not call
`delete` - see [docs/PERFORMANCE.md](docs/PERFORMANCE.md) §6.

The first 512 KB of this pool doubles as the boot-time TOML parse arena
(`toml_arena_alloc`, `src/sdram_pool.cpp:51-57`), driven by `LoadEmbeddedPresetBank()`
(`:61-71`). That arena is abandoned before
`new CloudSeed::ReverbController(...)` runs, so `pool_index` is still 0 when the reverb starts
allocating - the two uses never overlap in time.

### Core Classes

**ReverbController** ([CloudSeed/ReverbController.h](CloudSeed/ReverbController.h)):
- Main reverb controller
- Preset application: `LoadPreset(const float* values)` (`:47-60`) copies a parsed
  `PresetData::params[]` into `parameters[]`, skipping only `LineCount` (written at audio rate
  from the `"delay_lines.max"` toggle target; `isReverse`/Bloom is preset data like everything
  else), then re-applies all 47 slots through `SetParameter`. The
  constructor no longer loads any preset - `main()` parses presets.toml and calls `LoadPreset()`
  before audio starts
- Single channel: `channelR` and all right-channel buffers are commented out
  (`CloudSeed/ReverbController.h:27`, `:37`, `:72`, `:173`, `:181`, `:194`, `:198`, `:200`, `:206`)
- Fixed internal block size `static const int bufferSize = 48` (`CloudSeed/ReverbController.h:23`),
  backing fixed-size member arrays (`:28-32`)
- Public API: `LoadPreset(const float*)` `:47`, `SetParameter(Parameter, float)` `:167`,
  `ClearBuffers()` `:178`, `Process(float* input, float* output, int bufferSize)` `:184`
- Parameter scaling (the normalized 0.0-1.0 → real-unit mapping documented in presets.toml):
  `GetScaledParameter` `:85`. `isReverse` follows the same `< 0.5 ? 0 : 1` rule as every other
  on/off parameter (`:101`)

**ReverbChannel** ([CloudSeed/ReverbChannel.h](CloudSeed/ReverbChannel.h)):
- `static const int TotalLineCount = 5;` for the mono implementation (`CloudSeed/ReverbChannel.h:39`)
- Contains delay lines, diffusers, modulation

**DelayLine** ([CloudSeed/DelayLine.h](CloudSeed/DelayLine.h)):
- Core delay buffer implementation (255 lines)
- The delay buffer itself is SDRAM-pool-backed (placement-new into `custom_pool_allocate`,
  `CloudSeed/ModulatedDelay.h:41-42`)
- `tempBuffer`, `mixedBuffer`, and `filterOutputBuffer` are real heap allocations
  (`CloudSeed/DelayLine.h:44-46`, freed at `:66-68`)

**AllpassDiffuser, MultitapDiffuser**: Diffusion stages
**ModulatedAllpass, ModulatedDelay**: Modulated processing

## Build System

### Building Libraries

```bash
# Rebuild all libraries (libcloudseed, DaisySP, libdaisy).
# Makefile:103-106 runs `clean all` in each, so this is a full rebuild of all three.
make libs

# Or build individually:
cd CloudSeed && make
cd libdaisy && make
cd DaisySP && make
```

### Building Effects

```bash
# CloudSeed
make
```

`make` also runs `$(MAKE) -C CloudSeed` on every invocation (`Makefile:31-38`): the sub-make
is incremental and rebuilds `libcloudseed.a` only when a library source changed, and the ELF
relinks only when the archive's mtime moved. A CloudSeed edit therefore never needs `make libs`.

### Checking presets

```bash
# Validity check with the firmware's own parser, without building firmware.
# The same check runs automatically as part of `make`.
make presets-check
```

It builds `build/preset_check` from `tools/preset_check.cpp`, `src/preset_bank.cpp`, and the
vendored tomlc99 (compiled as C) using `HOSTCC`/`HOSTCXX` (default `gcc`/`g++`), then runs
`preset_check --validate presets.toml`. Unlike the stamp-gated check inside `make`, it re-runs
every time. `preset_check` requires exactly one mode: `--validate` (one-line summary; exit 1
with the parser's message on stderr if the file is rejected), `--print-knob-map`, or
`--print-toggle-map`; anything else prints usage and exits 2.

`make` cannot produce firmware from a presets.toml the parser would reject: the embedded blob
(`$(BUILD_DIR)/presets_toml.o`) depends on `$(BUILD_DIR)/presets.valid`, whose recipe is
`preset_check --validate presets.toml` (`Makefile:53-70`). Validation covers TOML syntax,
missing/unknown/misplaced parameters, unknown preset- and root-level keys, out-of-range
scalars (a parameter value that is non-finite or outside 0..1; `max_delay_lines` /
`default_delay_lines` that are not whole numbers 1..5 or where the default exceeds the max;
`blinks`, the ms fields), an unresolvable `[preset.knob_map]` / `[preset.toggle_map]` target
(unknown group/parameter, wrong group, a runtime parameter, or - for toggles - a parameter not
in `kToggleParams`), and a document too large for the boot parse arena - the host tool allocates
through a replica of `TOML_ARENA_SIZE` (512 KB, 8-byte aligned, no reuse), so `presets.toml: 10
presets valid, boot arena peak 141696 of 524288 bytes` is the same peak the pedal sees. Host
pointers are 64-bit, so the reported peak over-estimates the 32-bit target: a pass here implies
a fit on hardware. A near-miss should be fixed by raising `TOML_ARENA_SIZE` (`src/sdram_pool.cpp:48`),
not by loosening the host check.

Preset values are free to change: `make presets-check` fails only on input the parser rejects.
There is no golden-value comparison; the original factory values are recoverable from git
history.

A file that somehow reaches the pedal broken is unrecoverable at runtime: `FatalErrorLoop()`
(`src/pedal_leds.cpp:45-54`, called at `src/cloudseed.cpp:527`) blinks both LEDs at 5 Hz forever and never starts
audio.

### Host unit tests

```bash
make test
```

Builds four host executables into `build/` with `HOSTCXX` (`Makefile:80-101`) and runs them;
no ARM toolchain or firmware build is involved. Each prints `<suite>: N checks, 0 failed` and
exits non-zero on any failed `CHECK()` ([tests/check.h](tests/check.h)):
- `knob_bank_test` - `KnobBank` parking, move threshold, 50-block takeover glide, apply
  epsilon, rails, bank change, reset mid-glide
- `toggle_bank_test` - `ToggleBank` parking on first scan / bank change / `forceReset`, and
  immediate writes after a flip
- `footswitch_gestures_test` - `FootswitchGestures` driven through a model of libdaisy's 8-bit
  `Switch` shift register: taps, 5 s save, FS2 hold + `MarkEdited()`, staggered chords, the
  restore chord, bounces, and a falling edge without a prior `Pressed()`
- `preset_bank_test tests/fixtures/two_presets.toml` - `ParsePresetBank()` on a two-preset
  fixture (Chorus + Through the Looking Glass), then a table of single-line mutations that
  must each be rejected with a specific message, plus two that must be accepted

The parser test deliberately uses its own fixture, not presets.toml, so editing preset values
never breaks `make test`; the live file stays covered by `make presets-check` and the build
gate.

### Build Outputs

Located in `build/`:
- **{target}.bin** - Binary for DFU flashing via USB
- **{target}.elf** - ELF executable with debug symbols
- **{target}.hex** - Intel HEX format
- **{target}.map** - Linker map file

This project builds with `APP_TYPE = BOOT_SRAM` (`Makefile:6`): the application is loaded into
SRAM from QSPI flash by the Daisy bootloader, which is what allows room for all ten presets.

### Flashing to Hardware

```bash
# One time (or after bootloader updates): flash the Daisy bootloader over DFU
make program-boot

# Then reset the Daisy, hold BOOT until the LED blinks rapidly, and flash the app
make program-dfu
```

Both targets come from `libdaisy/core/Makefile:343-347` and require `dfu-util`. A `BOOT_SRAM`
build cannot be flashed with `make program` (openocd) - libdaisy errors out on that path
(`libdaisy/core/Makefile:335-336`). Same procedure as `README.md:36-43`.

### Compiler Configuration

**Platform**: ARM GCC (`arm-none-eabi-gcc`)
**CPU**: Cortex-M7 (`-mcpu=cortex-m7`, `CloudSeed/Makefile:69`)
**Optimization**: `-O3` (`Makefile:20`, `CloudSeed/Makefile:19`)
**FPU**: Hard float (`-mfpu=fpv5-d16 -mfloat-abi=hard`, `CloudSeed/Makefile:72-75`)
**Language**: C++14 (`-std=gnu++14`, `CloudSeed/Makefile:66`)
**Float flags**: `-ffast-math` on both the app (`Makefile:44`) and the library
(`CloudSeed/Makefile:105`); the library additionally uses `-fno-exceptions`,
`-finline-functions`, and `-fno-aggressive-loop-optimizations` (`CloudSeed/Makefile:103-107`)
**App type**: `BOOT_SRAM` (`Makefile:6`)

## Common Modifications

### 1. Changing Control Mappings

**File**: [presets.toml](presets.toml) - no C++ changes required for a remap.

**Example**: Swap KNOB_1 and KNOB_2 in one preset - edit that preset's `[preset.knob_map]`:

```toml
knob1_a = "output.EarlyOut"
knob2_a = "output.DryOut"
```

Then `make` (which validates the file) and `make program-dfu`. The map is per preset, so the
same two lines must be edited in every preset that should share the scheme. Verify the
resolved map with `./build/preset_check --print-knob-map presets.toml`.

**Example**: Put the input low-pass on KNOB_6's secondary bank:

```toml
knob6_b = "input.LowPass"
```

`applyKnobTarget()` (`src/cloudseed.cpp:215-248`) turns `LowPassEnabled` on the first time that
knob is moved, so the filter is audible even in presets that ship with it off; the takeover
glide starts from the open filter (`knobTargetValue()`), so enabling it does not step the tone.
`HighPass` gets the same treatment via `HiPassEnabled`; no other gated parameter does - for the
shelves, the in-loop cutoff, and the diffusers, set the matching `*Enabled` value in
`[preset.params.*]`. Either owned by a toggle target instead (see below), in which case
`applyKnobTarget()` leaves the enable alone and the knob only moves the cutoff.

`applyKnobTarget()` is also where the output-level cache is invalidated: its `switch` sets
`state.outputLevelsDirty` for `DryOut`/`EarlyOut`/`MainOut`. Any new code path that writes one
of those three parameters outside `loadPreset()` must set that flag, or the makeup gain and the
dry-cancellation scale will stay at their old values.

**Adding a non-parameter knob target** (like `reverse.delay`) requires C++: a new
`KnobTargetKind` in [src/preset_bank.h](src/preset_bank.h), a branch in `parseKnobTarget()`
([src/preset_bank.cpp](src/preset_bank.cpp)), and a branch in `applyKnobTarget()`
(`src/cloudseed.cpp:215-248`).

**Example**: reassign SWITCH_2 (Bloom by default) to enable the late low-pass instead - edit
that preset's `[preset.toggle_map]`:

```toml
toggle2_a = "late_eq.CutoffEnabled"
toggle2_b = "late_eq.CutoffEnabled"
```

Verify with `./build/preset_check --print-toggle-map presets.toml`. A toggle value is either one
of the three pseudo-targets or a quoted `"group.Parameter"` naming one of the twelve on/off
parameters in `kToggleParams` ([src/preset_bank.cpp](src/preset_bank.cpp)): `early.isReverse`,
`input.HiPassEnabled`, `input.LowPassEnabled`, `early_diffusion.DiffusionEnabled`,
`early_diffusion.DiffusionStages`, `late_diffusion.LateDiffusionEnabled`,
`late_diffusion.LateDiffusionStages`, `late_eq.LowShelfEnabled`, `late_eq.HighShelfEnabled`,
`late_eq.CutoffEnabled`, `late.LateStageTap`, `late.Interpolation` - plus `delay_lines.max`,
`reverse.enabled`, `reverse.direct_mix`. A continuous parameter (e.g. `late.LineDecay`) is
rejected with `'Name' is not an on/off parameter`: a lever would slam it to 0 or 1.

**Adding a non-parameter toggle target** (like `delay_lines.max`) requires C++: a new
`ToggleTargetKind` in [src/preset_bank.h](src/preset_bank.h), a branch in `parseToggleTarget()`
([src/preset_bank.cpp](src/preset_bank.cpp)), and a branch in `applyToggleTarget()`
(`src/cloudseed.cpp:252-267`).

### 2. Modifying Parameter Ranges

Knobs are read raw: `hw.knob[kKnobIndex[i]].Value()` (`src/cloudseed.cpp:453-455`) yields 0.0-1.0
and is handed straight to `SetParameter`, which applies the engine's own scaling
(`ReverbController::GetScaledParameter`). There is no per-knob min/max any more - the six
`::daisy::Parameter` wrappers were removed when knob targets became data.

To restrict a knob's travel, scale in `applyKnobTarget()` (`src/cloudseed.cpp:215-248`) before the
`SetParameter` call, e.g. `value = 0.5f + 0.5f * value;` for the upper half of the range. Note
that this affects every preset that maps a knob to that parameter.

The knob response constants: `KNOB_SMOOTHING_COEFF` (`src/cloudseed.cpp:48`, the ADC one-pole), and in
[src/knob_bank.h](src/knob_bank.h) `kKnobMoveThreshold` (how far a parked knob must move to take over),
`kKnobApplyEpsilon` (the smallest change a live knob re-writes), `kKnobRailWindow` (how close to a
stop reads as exactly 0.0 / 1.0), and `kKnobGlideBlocks` (takeover glide length in 1 ms blocks;
100 if the takeover still clicks, 25 if it feels laggy).

### 3. Adding or Editing Presets

**File**: [presets.toml](presets.toml) - no C++ changes required.

Append a new `[[preset]]` table at the end of the file (appending keeps the existing indices,
which are persisted to QSPI flash):

```toml
# --------------------------------------------------------------------------
[[preset]]
name = "Your New Preset"
blinks = 11
led_on_ms = 150
led_off_ms = 150
led_pause_ms = 5000
default_delay_lines = 2.0
max_delay_lines = 5.0

# Knob assignments. Primary (_a) is the knob's normal function; secondary (_b)
# is active only while the preset footswitch (FS2) is held down. All twelve keys required.
[preset.knob_map]
knob1_a = "output.DryOut"
knob2_a = "output.EarlyOut"
knob3_a = "output.MainOut"
knob4_a = "late_diffusion.LateDiffusionFeedback"
knob5_a = "early.TapDecay"
knob6_a = "late.LineDecay"
knob1_b = "input.PreDelay"
knob2_b = "input.HighPass"
knob3_b = "input.LowPass"
knob4_b = "late.LineModAmount"
knob5_b = "late.LineModRate"
knob6_b = "reverse.delay"

# Toggle assignments. Primary (_a) is the switch's normal function; secondary (_b)
# is written by flipping the switch while the preset footswitch (FS2) is held down.
# All eight keys required.
[preset.toggle_map]
toggle1_a = "delay_lines.max"
toggle2_a = "early.isReverse"
toggle3_a = "reverse.enabled"
toggle4_a = "reverse.direct_mix"
toggle1_b = "delay_lines.max"
toggle2_b = "early.isReverse"
toggle3_b = "reverse.enabled"
toggle4_b = "reverse.direct_mix"

# Input stage: pre-delay and the input filters feeding the whole reverb.
[preset.params.input]
InputMix       = 0.0
PreDelay       = 0.07
HiPassEnabled  = 0.0
HighPass       = 0.0
LowPassEnabled = 0.0
LowPass        = 0.29

# Early reflections: the multi-tap delay that follows the input stage.
[preset.params.early]
# ...
isReverse = 0.0

# ... the remaining five [preset.params.*] groups, every key required

# Delay lines: stored state of the "delay_lines.max" toggle target
# (off = this preset's default_delay_lines, on = its max_delay_lines).
[preset.params.delay_lines]
max = 0.0

# Reverse voice: window length (knob target "reverse.delay") and the stored state of
# the toggle targets "reverse.enabled" (voice on) and "reverse.direct_mix" (routing).
[preset.params.reverse]
delay      = 0.4771
enabled    = 0.0
direct_mix = 0.0
```

Rules the parser enforces (a violation fails `make` before the blob is embedded; if one were
ever flashed it would stop boot and blink both LEDs):
- All eight `[preset.params.*]` parameter groups must be present, each containing exactly its
  own keys - 46 parameters total. Group membership is defined by `kGroups` in
  [src/preset_bank.cpp](src/preset_bank.cpp) and mirrored by the reference comment at the top of
  presets.toml
- `[preset.params.reverse]` must be present with exactly `delay`, `enabled` and `direct_mix`,
  each a number 0..1 (`missing [preset.params.reverse]`, `missing or non-numeric
  'reverse.delay'`, `'reverse.delay' = V out of range 0..1`,
  `[preset.params.reverse]: unknown key 'K'`). `delay` is the value `reverse.delay` starts at
  when the preset loads: 20 + Response3Oct(delay) × 1980 ms; `enabled` and `direct_mix` are the
  toggle pseudo-targets' stored starting state
- `[preset.params.delay_lines]` must be present with exactly `max`, a number 0..1: the stored
  starting state of the `"delay_lines.max"` toggle target
- `LineCount` alone must not appear (the `"delay_lines.max"` toggle target owns it at audio rate)
- `[preset.knob_map]` must be present with all twelve `knobN_a` / `knobN_b` keys. Each value
  is a quoted `"group.Parameter"` whose parameter really belongs to that group, or
  `"reverse.delay"`. Unknown knob keys, unknown groups/parameters, cross-group targets, and
  runtime parameters are all rejected
- `[preset.toggle_map]` must be present with all eight `toggleN_a` / `toggleN_b` keys
  (N = 1..4 = SWITCH_1..SWITCH_4). Each value is a quoted `"group.Parameter"` naming one of the
  twelve on/off parameters in `kToggleParams` ([src/preset_bank.cpp](src/preset_bank.cpp)), or one of
  the three pseudo-targets `"delay_lines.max"`, `"reverse.enabled"`, `"reverse.direct_mix"`.
  Unknown toggle keys, unknown groups/parameters, cross-group targets, runtime parameters, and
  continuous (non-boolean) parameters are all rejected
- `blinks` is 1..20; `default_delay_lines` and `max_delay_lines` are each a whole number 1..5
  (a fraction or `nan` fails with `preset N: <key> must be a whole number 1..5`), and
  `default_delay_lines` must not exceed `max_delay_lines`
  (`preset N: default_delay_lines exceeds max_delay_lines`); the ms fields are 0..60000 and
  optional (defaults 150/150/5000); at most `kMaxPresets` (16) presets
- Every parameter value must be finite and within 0.0-1.0 (`preset N: 'Name' = V out of range
  0..1`): `AudioLib::ValueTables::Get` indexes its tables with the raw value, so anything else
  would read out of bounds on the pedal. The real-unit ranges are listed in the TOML header and
  implemented by `ReverbController::GetScaledParameter`

Then rebuild and reflash: `make && make program-dfu` - validation is part of `make`
(`make presets-check` runs the same check without building firmware).
`NUM_PRESETS` no longer exists - the count comes from `gPresets.count` and the modulo cycling
adapts automatically.

**Reordering or deleting presets** invalidates saved settings: bump `SETTINGS_VERSION`
(`src/pedal_storage.cpp:5`) in the same change so stale flash contents are discarded. Saved user
presets need nothing: any presets.toml change alters the firmware hash, which discards them all
at the next boot.

**Blink Pattern Customization**: per preset, via `blinks`, `led_on_ms`, `led_off_ms`, and
`led_pause_ms`.

### 4. Changing Number of Delay Lines

**File**: [CloudSeed/ReverbChannel.h](CloudSeed/ReverbChannel.h)

```cpp
// Current: 5 delay lines for mono
static const int TotalLineCount = 5;

// To change to 4:
static const int TotalLineCount = 4;
```

**Warning**: Changing delay line count affects memory usage and processing load.

### 5. Modifying Switch Behavior

**File**: [presets.toml](presets.toml) for reassigning a switch (see "1. Changing Control
Mappings" above); [src/cloudseed.cpp](src/cloudseed.cpp) for the mechanism itself
(`src/cloudseed.cpp:112-113` for `kToggleIndex`, the presets.toml-order -> `hw.switches[]` map;
`src/cloudseed.cpp:338-377` for `updateEngineControls()`, which scans and dispatches both knobs and
toggles; `src/cloudseed.cpp:252-267` for `applyToggleTarget()`; `src/cloudseed.cpp:478-486` for
SWITCH_3/SWITCH_4 (`reverse.enabled`/`reverse.direct_mix`) in the callback)

Every switch's function is data, resolved per preset by `[preset.toggle_map]`. The default map
reproduces the historical assignment: SWITCH_1 selects the delay-line count
(`state.delayLinesMax ? state.maxDelayLines : state.defaultDelayLines`); SWITCH_2 is Bloom
(`Parameter::isReverse`); SWITCH_3 toggles the reverse voice (`state.reverseEnabled`); SWITCH_4
selects reverse routing (`state.reverseDirectMix`; off = into the reverb, on = direct mix). The
reverse window length is not a toggle target: it starts at the preset's
`[preset.params.reverse] delay` and is then set by whichever knob maps to `"reverse.delay"`.
Footswitch gestures (tap, hold, the 5 s save and the restore chord, including
`kLongHoldBlocks`) live in [src/footswitch_gestures.h](src/footswitch_gestures.h).

To change what a switch does without touching presets.toml (e.g. a completely different scheme
for every preset, or a fifth pseudo-target), add a case to `ToggleTargetKind`
([src/preset_bank.h](src/preset_bank.h)), a branch in `parseToggleTarget()`
([src/preset_bank.cpp](src/preset_bank.cpp)), and a branch in `applyToggleTarget()`
(`src/cloudseed.cpp:252-267`); see "1. Changing Control Mappings" above.

`hw.switches[...].Pressed()` — an 'ON' toggle counts as pressed
(`src/cloudseed.cpp:456-458`) — is read for every toggle, not `.Read()`.

### 6. Adjusting Audio Buffer Size

**File**: [src/cloudseed.cpp](src/cloudseed.cpp) (`src/cloudseed.cpp:562-575`)

```cpp
// Current: 48 samples per block
hw.StartAdc();
// ... 200 ms knob one-pole settle loop (see "Knob take-over" above) ...
hw.StartAudio(audioCallback);
```

Three sizes are coupled and must change together:
- `DaisyPetal::Init()` sets the hardware block size to 48 (`libdaisy/src/daisy_petal.cpp:90`);
  override it with `hw.SetAudioBlockSize(n)` before `StartAudio()`
- `AUDIO_BUFFER_SIZE` (`src/cloudseed.cpp:29`) sizes the file-scope `gInputBuffer`, `gWetBuffer`,
  `gReverseBuffer` and `gReverbInputBuffer` (`src/cloudseed.cpp:301-304`) and every callback loop;
  `main()` stops in `FatalErrorLoop()` if `hw.AudioBlockSize()` differs (`src/cloudseed.cpp:519-520`)
- `ReverbController::bufferSize` (`CloudSeed/ReverbController.h:23`) sizes the controller's
  fixed member arrays

Raising the hardware block size alone stops boot at that guard; raising it with
`AUDIO_BUFFER_SIZE` but not `bufferSize` overruns the controller's arrays.
Smaller blocks = lower latency, higher CPU load; larger blocks = higher latency, lower CPU load.

### 7. Adding CV Control

Terrarium has knob inputs that can accept CV (0-3.3V or 0-5V with voltage divider):

```cpp
// Read CV directly (identical to what the knob scan uses):
float cv_value = hw.knob[Terrarium::KNOB_1].Value();
// cv_value is 0.0-1.0 regardless of input voltage
```

### 8. LED2 Preset Indicator System

**Current Implementation**: LED2 uses a state machine to blink the preset number continuously (runs in main loop).

The system is defined in [src/pedal_leds.cpp](src/pedal_leds.cpp) (`BlinkPattern` in
[src/pedal_leds.h](src/pedal_leds.h):9-14, `BlinkState` `src/pedal_leds.cpp:10-16`;
`StartPresetBlink` :57-64; `ServicePresetBlink` :67-113). The LED objects are file statics
there, reachable only through the `pedal_leds.h` functions. The pattern itself is
`state.blinkPattern`, cached by `loadPreset()` and passed in by the main loop; there is no
per-call lookup function. Both functions only `Set()` LED2; the audio callback's
`UpdateLeds()` pushes it to the pin.

**Key Components**:
- `BlinkPattern` struct: Defines blink timing parameters
- `BlinkState` struct: Maintains blink state machine
- `ServicePresetBlink(bypass, pattern)`: Main loop function that manages LED transitions
- `state.blinkPattern`: the active preset's pattern, refreshed by `loadPreset()`
- `StartPresetBlink()`: Initiates a new blink sequence

**Behavior**:
- Blinks continuously when pedal is active (not bypassed)
- Turns off completely when bypassed
- Blinks N times where N = preset number (1-10)
- 150ms on, 150ms off per blink
- 5 second pause between sequences
- Automatically restarts sequence after pause
- Save / restore confirmation: `ServiceConfirmBlink()` takes over LED1 and LED2 for 3 × 80 ms
  on/off (even when bypassed) and `ServicePresetBlink()` is skipped until it finishes; LED2 then
  restarts its preset pattern and LED1 returns to the bypass state

**To modify LED2 behavior for custom purposes**, you would need to modify or replace `ServicePresetBlink()` (`src/pedal_leds.cpp`), called from the main loop. The current implementation is tightly integrated with preset indication.

## Code Navigation Tips

### Key Files for Modification

**Most Common**:
- [presets.toml](presets.toml) - all preset data **and the knob map** (parsed at boot;
  validated by `make`)
- [src/cloudseed.cpp](src/cloudseed.cpp) - `main()`, pedal state, audio callback, knob/toggle dispatch, preset load
- [src/knob_bank.h](src/knob_bank.h) - absolute, parked knob take-over state machine (park, 50 ms glide, track)
- [src/footswitch_gestures.h](src/footswitch_gestures.h) - FS1/FS2 gesture state machine: taps, FS2 hold (secondary bank), FS1 5 s save, FS1 + FS2 5 s restore
- [src/pedal_leds.cpp](src/pedal_leds.cpp) - LED1/LED2: preset blink, save/restore confirmation, `FatalErrorLoop()`
- [src/pedal_storage.cpp](src/pedal_storage.cpp) - QSPI `Settings` and `UserPresets` (`PedalStorage`), firmware hash

**Advanced**:
- [src/preset_bank.cpp](src/preset_bank.cpp) - TOML schema, group membership, and validation errors
- [src/sdram_pool.cpp](src/sdram_pool.cpp) - `custom_pool_allocate()` SDRAM pool and the boot TOML parse arena
- [tests/](tests/) - host unit tests (`make test`)
- [CloudSeed/ReverbController.h](CloudSeed/ReverbController.h) - `LoadPreset`, parameter scaling
- [CloudSeed/Parameter.h](CloudSeed/Parameter.h) - All available reverb parameters
- [CloudSeed/ReverbChannel.h](CloudSeed/ReverbChannel.h) - Core reverb architecture

**Reference Only** (usually don't modify):
- [CloudSeed/DelayLine.h](CloudSeed/DelayLine.h) - Delay buffer implementation
- [CloudSeed/AudioLib/](CloudSeed/AudioLib/) - Audio utilities
- Submodules (libdaisy, DaisySP, Terrarium)

### Understanding Audio Flow

**CloudSeed Audio Callback** (`audioCallback()` at `src/cloudseed.cpp:438-489` - runs once per
48-sample block, i.e. at 1 kHz):
1. Process analog/digital controls and push both LEDs with `Led::Update()` (`:441-443`). Once
   audio runs this is the only caller of `Update()`: it is a read-modify-write that would race
   with the main loop
2. `processFootswitches()` (`:306-331`): one `gFootswitches.Update()` call with both switches'
   `Pressed()` / `FallingEdge()` returns `FootswitchEvents`. `toggleBypass` flips
   `state.bypass` and sets LED1 (the main loop persists it later), `cyclePreset` sets
   `triggerPresetChange`, `savePreset` sets `gSaveRequested`, `restorePreset` sets
   `triggerPresetRestore`
3. Read `state.bypass` once. While `state.presetChangeInProgress` is set the main loop is
   rewriting `state.knobMap` / `state.toggleMap` and every engine parameter, so the callback
   copies the input to the output and returns (`:448-451`): no knob/toggle scan, no reverb,
   `reverseMix` does not advance
4. Read all six knobs with `hw.knob[kKnobIndex[i]].Value()` and all four toggles with
   `hw.switches[kToggleIndex[i]].Pressed()` (`:453-458`) and call
   `updateEngineControls(gFootswitches.PresetHeld() ? 1 : 0, knobPositions, togglePositions)`
   (`:460-461`, body `:338-377`), which holds every reverb write:
   - read every knob's current target value for the active bank with `knobTargetValue()`, then
     one `gKnobs.Scan(bank, reset, …)` call (`:343-350`) re-parks on any bank or preset
     transition (zero writes that block) and otherwise returns the knobs to write: glide steps
     for a knob taking over, then the knob's own position; `reset` is
     `state.controlResetPending`, read once and shared with the toggle scan (`:347`)
   - dispatch each returned write through `applyKnobTarget()` using `state.knobMap[bank][i]` —
     the cached copy, never `gPresets` (`:351-352`)
   - one `gToggles.Scan(bank, reset, togglePositions, …)` call (`:356-357`) re-parks the same
     way (`toggle_bank.h`), then returns the levers to write: a flipped lever writes its
     position (up = on) immediately, no glide; dispatch each through `applyToggleTarget()`
     using `state.toggleMap[bank][i]` (`:359-360`)
   - any knob or toggle write while in bank 1 calls `gFootswitches.MarkEdited()`, cancelling
     that hold's preset change (`:365-366`)
   - select the delay line count as `state.delayLinesMax ? state.maxDelayLines :
     state.defaultDelayLines` (the `"delay_lines.max"` toggle target, applied above if it was
     just flipped) and write `Parameter::LineCount` when it differs from
     `state.prevNumDelayLines` (`:368-376`). Both candidates are exact copies, so the `!=` test
     is exact; a preset with different default/max counts is re-applied on the first callback
     after the change
   Then, if `gSaveRequested` is set and the previous snapshot has been consumed, copy
   `GetAllParameters()`, `state.reverseDelayNorm` and the three toggle pseudo-values into
   `gSaveSnapshot` and publish it with `state.saveSnapshotReady` (`:465-475`). This runs only
   past the preset-change gate, so a half-loaded preset is never captured
5. `refreshOutputLevels()` (`:476`, body `:381-396`): `makeupGain` and `scaledDryOut` are
   **not** recomputed per block: they depend only on `DryOut`/`EarlyOut`/`MainOut`, so they are
   derived into `state.makeupGain` / `state.scaledDryOut` only when `state.outputLevelsDirty` is
   set, and the output stage just reads them. The flag is set by `applyKnobTarget()` on a write
   to any of those three parameters and by `loadPreset()`. The dry/wet mix itself happens inside
   the reverb; the callback only scales the result by
   `makeupGain = OUTPUT_VOLUME_BOOST * (1 + sinf(wetBalance * HALF_PI) * MAKEUP_GAIN_STRENGTH)`,
   where `wetBalance = (earlyOut + mainOut) / (dryOut + earlyOut + mainOut + FLOAT_EPSILON)` and
   `HALF_PI` is a `float` constant, so no double-precision math is emitted. Those three levels
   are read from `reverb->GetAllParameters()`, not from knob positions - with
   `[preset.knob_map]` a knob need not be mapped to any of them
6. `state.reverseEnabled` (the `"reverse.enabled"` toggle target) becomes `reverseTarget` (0 or
   1), and the left input channel is copied into `gInputBuffer` (`:480-481`)
7. `state.reverseDirectMix` (the `"reverse.direct_mix"` toggle target) picks the render helper
   (`:483-486`), each of which writes the output scaled by `makeupGain`:
   - Into-reverb, `reverseDirectMix` off, `renderReverseIntoReverb()` (`:403-420`): reverse the
     dry input into `gReverseBuffer`, scale it to the injected reverse, add that to the reverb
     input (`gReverbInputBuffer`), run `reverb->Process(gReverbInputBuffer, gWetBuffer, …)`,
     then `out = (wet − scaledDryOut * injectedReverse) * makeupGain`; subtracting
     `scaledDryOut * injectedReverse` cancels the reverb's dry pass-through of the injected
     reverse, so the forward dry stays clean and the reverse is heard only through the wet tail
   - Direct-mix, `reverseDirectMix` on, `renderReverseDirect()` (`:424-435`):
     `reverb->Process(gInputBuffer, gWetBuffer, …)`, record the reverb output into
     `reverseDelay` for backward playback, then
     `out = (wet + reversed * REVERSE_LEVEL * mix) * makeupGain`, the reverse audible on its own
   Both ramp the reverse via a smoothed mix toward `reverseTarget`, kept in a local during the
   sample loop and stored back to `state.reverseMix` afterwards, so the `"reverse.enabled"`
   toggle switches click-free (engine in `CloudSeed/ReverseDelay.h`)
8. If bypassed, overwrite the output with the input (`:487-488`)

When `state.bypass` is set, output is a straight copy of the input — but the reverb (and the
reverse mix) is still processed, deliberately, to suppress an audible 1 kHz whine (the
`reverb->Process()` call inside whichever render helper runs, `:403-435`). When
`state.presetChangeInProgress` is set, the reverb is skipped entirely and the input is passed
through (`:448-451`).

**CloudSeed Main Loop** (`main()` while loop at `src/cloudseed.cpp:577-613` - free-running, no sleep):
1. **Save / restore** (`:580-594`): a published `gSaveSnapshot` is copied into the current
   preset's `UserPreset` slot (`gStorage.UserSlot()`) and written with `gStorage.SaveUserPresets()`; a restore clears the
   slot, reloads the preset with `loadPresetGated()`, and saves. Both start the confirmation
   blink. See "User preset save / factory restore"
2. **Handle preset changes** (`:600-604`): clear `triggerPresetChange`, then
   `loadPresetGated(next)` (`:202-212`) sets `presetChangeInProgress` and
   `state.controlResetPending` (so the knob and toggle positions are re-snapshotted before the
   new preset loads), and between two `std::atomic_signal_fence(std::memory_order_seq_cst)`
   compiler barriers updates `state.currentPreset` and runs `loadPreset()`, then clears the flag
   to re-enable audio processing; `StartPresetBlink(state.blinkPattern)` follows.
   `loadPreset()` is synchronous, so there is no delay: the gate reopens as soon as it returns.
   The six flags shared with the callback (`bypass`, `triggerPresetChange`,
   `triggerPresetRestore`, `saveSnapshotReady`, `presetChangeInProgress`, `controlResetPending`)
   are `volatile bool` (`:66-71`)
3. **Persist settings**: `gStorage.ServiceSettingsSave()` (`:606`) - the coalesced flash write described
   under Preset Persistence
4. **Update LEDs**: `ServiceConfirmBlink()`, else `ServicePresetBlink()` (`:609-610`)
5. `keepCoreBusy()` (`:612`, defined at `:502-508`) — deliberate busy work that keeps the core
   out of idle between callbacks and reduces an audible 1 kHz whine: it advances a `volatile`
   phase by 0.001 (wrapped at `TWO_PI`) and stores `sinf(phase)` to a `volatile` sink, so every
   pass performs a real FPU `sinf()` plus real loads and stores (a constant argument would be
   folded away at compile time). There is no loop delay; the loop spins

**Performance Architecture**:
- **Audio callback**: Time-critical, optimized for low latency
  - Only parameter smoothing and audio processing
  - Sets trigger flags for heavy operations
  - No flash writes or preset loading
- **Main loop**: Non-critical background tasks
  - Preset switching (includes buffer clearing)
  - Flash memory writes (settings, user preset save/restore)
  - LED blink state machine and save/restore confirmation blink
- **FPU flush-to-zero** is enabled once at the top of `main()` before `hw.Init()`
  (`src/cloudseed.cpp:511`) — see [docs/PERFORMANCE.md](docs/PERFORMANCE.md) §1

This separation prevents audio glitches during preset changes and flash writes.

## Debugging

### Serial Debug Output

```cpp
// In src/cloudseed.cpp, add:
#include "daisy_seed.h"
using namespace daisy;

// In setup:
hw.seed.StartLog(true);

// Anywhere:
hw.seed.PrintLine("Debug: value = %f", some_value);
```

### LED Indicators

```cpp
// Terrarium LEDs are daisy::Led file statics gLed1/gLed2 in src/pedal_leds.cpp (:18-19,
// init LedsInit() :25-31). DaisyPetal has no led1/led2 members - it exposes
// SetRingLed/SetFootswitchLed/ClearLeds for the Daisy Petal board's own I2C LED driver,
// which Terrarium does not use. For a debug value, add a setter next to SetBypassLed():
gLed2.Set(parameter_value);  // 0.0-1.0
```

`Set()` is enough once audio runs: the audio callback calls `Led::Update()` for both LEDs every
block (`src/cloudseed.cpp:443`), and nothing else may call it after `hw.StartAudio()` (it is a
read-modify-write that races with the callback). Only before `StartAudio()` must you call
`UpdateLeds()` yourself.

Caveat: `ServicePresetBlink()` (`src/pedal_leds.cpp:67-113`) drives LED2 on every main-loop pass and
will overwrite debug values unless that call is removed. LED1 is likewise re-set on every
bypass toggle (`src/cloudseed.cpp:316`), and both LEDs are driven by the save/restore confirmation
blink (`ServiceConfirmBlink()`, `src/pedal_leds.cpp:124-139`).

### Common Issues

**Build Errors**:
- Ensure submodules are initialized: `git submodule update --init --recursive`
- Rebuild libraries: `make libs` (runs `clean all` in CloudSeed, DaisySP, and libdaisy)
- Clean build: `make clean && make`

**Audio Issues**:
- Check buffer size (48 samples typical)
- Verify SDRAM allocation for CloudSeed
- Check parameter ranges (0.0-1.0 typical)

**Control Issues**:
- Verify ADC channel mapping in Terrarium
- Check the knob smoothing coefficient (`KNOB_SMOOTHING_COEFF`, `src/cloudseed.cpp:48`) and the
  takeover constants in [src/knob_bank.h](src/knob_bank.h): a knob brushed by accident taking over its
  target means `kKnobMoveThreshold` is too low (raise it to 0.02); a parameter that is re-written
  while nobody touches a live knob means `kKnobApplyEpsilon` is below the ADC noise; a stop that
  does not reach exact silence / full level means `kKnobRailWindow` is narrower than the pot's
  bleed or top gap. Measure the ADC before changing any of them
- Confirm the knob is mapped where you expect: `./build/preset_check --print-knob-map presets.toml`
- Test with direct reads: `hw.knob[x].Value()`

## Performance Considerations

### CPU Usage

**CloudSeed**: Heavy processing
- 5 delay lines with modulation
- Multiple diffusion stages
- Extensive filtering
- Estimated at ~70-80% CPU. **This is an unmeasured estimate** - no profiling artifact exists
  in the repo, and it predates the denormal-stall (FPU flush-to-zero) and `std::map`
  parameter-lookup fixes documented in [docs/PERFORMANCE.md](docs/PERFORMANCE.md), so real headroom is
  better than this figure suggests


### Memory Usage

**CloudSeed** (from the linker's `--print-memory-usage` report, `make libs && make` on this tree):
- SDRAM: 52,510,752 B of 64MB (78.25%), entirely static `DSY_SDRAM_BSS`: `custom_pool`
  50,331,648 B + `reverseDelayBuffer` 768,000 B (192,000 floats = 4 s @ 48 kHz, sized so the
  2000 ms max reverse window clears `ReverseDelay`'s `size / 2` clamp) +
  `CloudSeed::FastSin::data` 131,072 B +
  `AudioLib::ValueTables` tables 1,280,032 B (see the `.sdram_bss` section in
  `build/cloudseed.map`). The TOML parse arena adds nothing: it is carved from `custom_pool`
  and abandoned before the reverb allocates
- The runtime heap is not in this report: it grows from `end` in RAM_D2
  (`libdaisy/core/STM32H750IB_sram.lds:244-251`), which is where `DelayLine`'s `tempBuffer`,
  `mixedBuffer`, and `filterOutputBuffer` (`CloudSeed/DelayLine.h:44-46`) land
- SRAM (`.text`+`.data`, `BOOT_SRAM` region): 205,188 B of 480KB (41.75%). Of that, the
  embedded `presets.toml` blob is 47,644 B (`build/presets_toml.o` - it carries the
  per-preset `[preset.knob_map]`, `[preset.toggle_map]`, `[preset.params.reverse]` and
  `[preset.params.delay_lines]` tables), tomlc99 is 14,371 B, and `preset_bank.o` is 7,786 B
- DTCMRAM: 27,988 B of 128KB (21.35%) — includes the 4,804 B `gPresets` bank (40 B of that per
  preset slot is the knob + toggle maps: 24 B knobs, 16 B toggles), the 6,676 B
  `gStorage` user-preset store (`PersistentStorage` keeps a defaults copy and a live copy of the 3,332 B
  `UserPresets`), and the 208 B `gSaveSnapshot`; RAM_D2_DMA: 16,968 B of 32KB (51.78%)
- QSPI: `Settings` in sector 0 (offset 0) and `UserPresets` in sector 1 (offset 0x1000), both
  inside the 256 KB below the bootloader's program area at 0x90040000

### Optimization Tips

See [docs/PERFORMANCE.md](docs/PERFORMANCE.md) for the concrete list of performance/correctness
fixes applied to the CloudSeed DSP (FPU denormal handling, parameter storage, hot-loop
modulo/precision fixes, placement-new/delete destructor safety, and build flags), each
with the exact file/line and pattern it addresses.

## Further Resources

### Documentation
- Daisy Wiki: https://github.com/electro-smith/DaisyWiki/wiki
- libdaisy API: https://electro-smith.github.io/libDaisy/
- DaisySP API: https://electro-smith.github.io/DaisySP/

### Source Projects
- CloudSeed VST: https://github.com/ValdemarOrn/CloudSeed
- GuitarML fork: https://github.com/GuitarML/DaisyCloudSeed

### Hardware
- Daisy Seed: https://www.electro-smith.com/daisy/daisy
- PedalPCB Terrarium: https://www.pedalpcb.com/product/pcb351/

## Project-Specific Notes

### Mono vs Stereo

This fork differs from the original CloudSeed and its predecessors (`CloudSeed/ReverbChannel.h:35-39`):
- **Original CloudSeed plugin**: 8 (or 12) delay lines, stereo
- **DaisyCloudSeed (Daisy Patch)**: 2 delay lines, stereo
- **GuitarML Terrarium fork**: 4 delay lines, mono
- **This fork**: 5 delay lines, mono (`static const int TotalLineCount = 5;`)

The trade-off: More delay lines in mono = richer reverb tail.

### GuitarML Modifications

Key changes in this fork:
1. Adapted for Terrarium hardware (mono, 6 knobs, 4 switches)
2. Increased delay line count from 2 to 5
3. Added preset cycling via footswitch
4. Simplified control scheme for guitar pedal use
5. Added delay line switching via toggle switches
6. **Bypass state persisted to flash** alongside the preset (`SETTINGS_VERSION = 2`, `src/pedal_storage.h:14-28`)
7. **Persistent preset storage** in QSPI flash memory with version control
8. **LED2 blink pattern system** for visual preset indication
9. **TOML-defined presets**: all preset data lives in [presets.toml](presets.toml), embedded in
   the firmware image with `.incbin` and parsed at boot by a vendored tomlc99 into a static
   `PresetBank`; `make` (and `make presets-check`, without building) rejects a file the
   parser would fail
10. **Performance optimization**: Moved preset switching and flash writes from audio callback to main loop;
    flash writes are coalesced 3 s after the last change (`PedalStorage::ServiceSettingsSave()`)
11. **Modulo-based preset cycling** for cleaner wraparound logic
12. **Through the Looking Glass Preset** enabled by adopting a `max_delay_lines` value for each preset
13. **Equal-power makeup gain** on the wet path, so perceived loudness holds as the dry/wet
    balance changes (`refreshOutputLevels()`, `src/cloudseed.cpp:381-396`, applied by the render
    helpers at `:403-435`), derived from `reverb->GetAllParameters()` only
    when `DryOut`/`EarlyOut`/`MainOut` change (`state.outputLevelsDirty`), not per block
14. **1 kHz whine mitigation**: the reverb is still processed while bypassed
    (`src/cloudseed.cpp:480-488`) and every main-loop pass calls `keepCoreBusy()`
    (`src/cloudseed.cpp:502-508`, `:612`): a real FPU `sinf()` of a `volatile` phase plus `volatile`
    loads and stores, so the core never idles between callbacks
15. **FPU flush-to-zero enabled at boot** to eliminate denormal stalls (`src/cloudseed.cpp:511`)
16. **`BOOT_SRAM` app type** (`Makefile:6`): the app is loaded into SRAM from QSPI flash by
    the Daisy bootloader, leaving room for all ten presets
17. **Per-preset knob mapping** (`[preset.knob_map]` in presets.toml): every knob has a primary
    and a secondary target, the secondary bank selected by holding the preset footswitch. Targets
    are `"group.Parameter"` strings plus the pseudo-target `"reverse.delay"` (the reverse window
    length, formerly a SWITCH_3 overload of KNOB_4; its per-preset start value is
    `[preset.params.reverse] delay`). Knobs are **absolute but parked**
    ([src/knob_bank.h](src/knob_bank.h)): after power-up, a preset load, or a bank change a knob writes
    nothing until it is turned, then glides its target to the pot's position over 50 ms and
    tracks it 1:1; the stops land exactly on 0.0 / 1.0
18. **Footswitch gestures** (`processFootswitches()`, `src/cloudseed.cpp:306-331`): tapping
    FOOTSWITCH_2 cycles the preset on release; holding it selects the secondary knob and toggle
    bank, and its release cycles only if no secondary knob or toggle wrote during the hold.
    FOOTSWITCH_1 toggles bypass on release. Both are
    tracked by `FootswitchGestures` ([src/footswitch_gestures.h](src/footswitch_gestures.h)), which holds
    each switch from `Pressed()` until `FallingEdge()` so a bounce cannot drop it, and treats any
    overlap of the two as a chord whose releases do nothing
19. **Active preset configuration cached in `PedalState`** (`loadPreset()`,
    `src/cloudseed.cpp:168-198`): `knobMap`, `toggleMap`, `defaultDelayLines`, `maxDelayLines` and
    the blink timings are copied out of `gPresets` on load, so the audio callback and the blink
    state machine never touch the parsed bank. While `state.presetChangeInProgress` is set the
    callback passes audio through and makes no engine write, so a load cannot be read
    half-applied
20. **Per-preset user save / factory restore**: FOOTSWITCH_1 held 5 s stores the engine state
    (isReverse and the three toggle pseudo-values included), the reverse window, and the
    current toggle-derived delay-line count into the current preset's slot of a QSPI
    `UserPresets` store (offset 0x1000); FOOTSWITCH_1 + FOOTSWITCH_2 held 5 s clears the slot
    and reloads the presets.toml values. Both are confirmed by LED1 + LED2 blinking 3 × 80 ms.
    The store is stamped with a hash of the firmware image and discarded when it differs, so
    saved edits never outlive the firmware that wrote them (see "User preset save / factory
    restore")
21. **Per-preset toggle mapping** (`[preset.toggle_map]` in presets.toml): every switch has a
    primary and a secondary target, the secondary bank shared with the knobs (held preset
    footswitch). Targets are `"group.Parameter"` strings naming one of twelve on/off parameters,
    or one of the pseudo-targets `"delay_lines.max"`, `"reverse.enabled"`, `"reverse.direct_mix"`.
    Toggles are **parked** the same way knobs are ([src/toggle_bank.h](src/toggle_bank.h)): after
    power-up, a preset load, or a bank change a lever writes nothing until it is flipped, then
    writes its position (up = on) with no glide - every toggle target is on/off. Each preset also
    gets a required `default_delay_lines` next to `max_delay_lines`: the `"delay_lines.max"`
    toggle target switches the line count between them (off = default, on = max), replacing the
    old hard-coded 2-line default

### Version Information

Check git history for recent changes:
```bash
git log --oneline -10
```

Current branch: `restructure`. Run the command above for recent changes; do not restate
them here.

## Quick Reference Card

### Build & Flash
```bash
make clean         # Clean previous build
make libs          # Rebuild libdaisy, DaisySP, and libcloudseed (clean all)
make presets-check # Validate presets.toml without building (also run automatically by `make`)
make test          # Host unit tests (knob/toggle/footswitch state machines, preset parser)
make               # Build CloudSeed
make program-boot  # One time: flash the Daisy bootloader (BOOT_SRAM prerequisite)
make program-dfu   # Flash the app (reset, hold BOOT until rapid blink, then run)
```

### File Locations
- Control mapping: `[preset.knob_map]` / `[preset.toggle_map]` in `presets.toml`; dispatch at
  `src/cloudseed.cpp:351-352` (knobs) / `:359-360` (toggles), `applyKnobTarget()`
  `src/cloudseed.cpp:215-248`, `applyToggleTarget()` `src/cloudseed.cpp:252-267`
- Preset data: `presets.toml` (embedded via `src/presets_toml.s`, parsed by `src/preset_bank.cpp`)
- Preset application: `CloudSeed/ReverbController.h:47` (`LoadPreset`)
- Parameters: `CloudSeed/Parameter.h`; names table: `CloudSeed/ParameterNames.h`
- Hardware config: `Terrarium/terrarium.h`

### Key Concepts
- Buffer size: 48 samples
- Sample rate: 48kHz (typical)
- SDRAM pool: 48MB (first 512 KB reused as the boot-only TOML parse arena; peak 141,696 B)
- Delay lines: 5 (mono Terrarium), toggled per preset between `default_delay_lines` and
  `max_delay_lines` in presets.toml (default SWITCH_1, `[preset.toggle_map]`)
- Presets: 10, defined in presets.toml, `gPresets.count` at runtime (max `kMaxPresets` = 16)
- Persistent storage: preset index + bypass, QSPI flash, written 3 s after the last change,
  `SETTINGS_VERSION = 2` - preset order in presets.toml is frozen unless the version is bumped
- User presets: FS1 held 5 s saves the current sound into the current preset, FS1 + FS2 held
  5 s restores it to factory; stored in QSPI at offset 0x1000 and wiped when a different
  firmware image boots. presets.toml is never modified
- App type: `BOOT_SRAM` (app runs from SRAM, loaded by the Daisy bootloader)
- LED2: Continuous blink pattern indicates preset number; both LEDs blinking together at 5 Hz
  with no audio is `FatalErrorLoop()`: the embedded TOML failed to parse, the SDRAM pool is
  exhausted, or the audio block size is not `AUDIO_BUFFER_SIZE` (48). Both LEDs blinking 3
  times quickly is the save/restore confirmation
- Knob banks: 2 per preset (`knobN_a` primary, `knobN_b` while the preset footswitch is held);
  knobs are absolute but parked - nothing moves until a knob is turned after a preset or bank
  change, then the target glides to the knob's position over 50 ms (no step) and tracks it; the
  stops land exactly on 0.0 / 1.0
- Toggle banks: 2 per preset (`toggleN_a` primary, `toggleN_b` while the preset footswitch is
  held); toggles are parked the same way but with no glide - a flipped lever writes its position
  (up = on) immediately ([src/toggle_bank.h](src/toggle_bank.h))

### Quick Modifications
1. Control mapping → `[preset.knob_map]` / `[preset.toggle_map]` in `presets.toml` (verify with
   `./build/preset_check --print-knob-map presets.toml` /
   `./build/preset_check --print-toggle-map presets.toml`)
2. Knob feel → `KNOB_SMOOTHING_COEFF` (`src/cloudseed.cpp:48`), `kKnobMoveThreshold`,
   `kKnobApplyEpsilon`, `kKnobRailWindow`, `kKnobGlideBlocks` ([src/knob_bank.h](src/knob_bank.h))
3. Add/modify presets → `presets.toml` (`make` validates it; `make presets-check` validates only)
4. Blink patterns → `blinks` / `led_on_ms` / `led_off_ms` / `led_pause_ms` in `presets.toml`
5. Switch logic → reassign in `[preset.toggle_map]` (no C++ needed; see "1. Changing Control
   Mappings"), or in code: `src/cloudseed.cpp:252-267` (`applyToggleTarget()`),
   `src/cloudseed.cpp:368-376` (delay line count from `state.delayLinesMax`),
   `src/cloudseed.cpp:478-486` (`reverse.enabled` / `reverse.direct_mix` in the callback),
   `src/cloudseed.cpp:306-331` (footswitches),
   [src/footswitch_gestures.h](src/footswitch_gestures.h) (taps, FS2 hold, 5 s save / restore gestures)
6. LED2 blink behavior → `src/pedal_leds.cpp:67-113` (`ServicePresetBlink`); save/restore
   confirmation → `src/pedal_leds.cpp:116-139` (`CONFIRM_BLINKS`, `CONFIRM_BLINK_MS` at `:6-7`)
7. Knob map schema/validation → `parseKnobMap()` / `parseKnobTarget()` in
   [src/preset_bank.cpp](src/preset_bank.cpp)
8. Toggle map schema/validation → `parseToggleMap()` / `parseToggleTarget()` in
   [src/preset_bank.cpp](src/preset_bank.cpp); shared with knobs via `splitTarget()` /
   `resolveParamTarget()`
9. User preset store → `UserPresets` (`src/pedal_storage.h:30-48`), `PedalStorage::Init()`
   (`src/pedal_storage.cpp:33-46`), save/restore handling in the main loop (`src/cloudseed.cpp:580-594`)
