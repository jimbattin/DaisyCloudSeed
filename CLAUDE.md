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
│   ├── pedal_storage.h/.cpp   # QSPI Settings + UserPresets (PedalStorage) + uploaded bank I/O, firmware+bank hash
│   ├── stored_bank.h          # Uploaded-bank QSPI layout + ValidStoredBankText() boot check (host-portable)
│   ├── sdram_pool.h/.cpp      # custom_pool_allocate() SDRAM bump pool + dedicated TOML parse arena (boot + USB upload)
│   ├── preset_protocol.h/.cpp # USB-MIDI SysEx preset upload protocol v1 (host-portable; docs/USB_MIDI.md)
│   ├── usb_midi_link.h/.cpp   # UsbMidiLink: USB-MIDI receive/reply over daisy::MidiUsbTransport
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
│   ├── engine_alloc_test.cpp  # CloudSeed engine: SetParameter()/Process() never allocate
│   ├── preset_protocol_test.cpp # SysEx assembler, USB-MIDI packer, protocol state machine, end-to-end via the parser
│   ├── stored_bank_test.cpp   # ValidStoredBankText(): which stored bank the pedal boots
│   └── fixtures/two_presets.toml  # Parser fixture (Chorus + Through the Looking Glass)
├── docs/
│   ├── HARDWARE_TESTS.md      # On-pedal validation checklist per revision (USB-MIDI link, audio regressions)
│   ├── PERFORMANCE.md         # Record of applied performance/correctness fixes (with file/line anchors)
│   ├── USB_MIDI.md            # USB-MIDI preset upload protocol, for host (browser) authors
│   └── pedal.png              # Pedal/control artwork
├── presets.toml           # Built-in preset bank (10 presets); an uploaded bank in QSPI can override it
├── third_party/tomlc99/   # Vendored TOML parser (MIT, commit in README.txt)
├── tools/                 # preset_check.cpp (host-side presets.toml validator),
│                          # usb_preset_host.py (Linux USB-MIDI test host for docs/HARDWARE_TESTS.md)
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
- Presets can be uploaded, read back, and reverted over USB-MIDI SysEx from a browser
  (docs/USB_MIDI.md); the pedal enumerates as a class-compliant USB-MIDI device at every boot
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
// Knob read: src/cloudseed.cpp:468-470; scan + dispatch: src/cloudseed.cpp:357-366;
// applyKnobTarget(): src/cloudseed.cpp:229-262
//                        primary (_a)                      secondary (_b)
KNOB_1: output.DryOut                        | input.PreDelay
KNOB_2: output.EarlyOut                      | input.HighPass   (+ HiPassEnabled on first touch)
KNOB_3: output.MainOut                       | input.LowPass    (+ LowPassEnabled on first touch)
KNOB_4: late_diffusion.LateDiffusionFeedback | late.LineModAmount
KNOB_5: early.TapDecay                       | late.LineModRate
KNOB_6: late.LineDecay                       | reverse.delay    (20-2000 ms reverse window)

// Toggle read: src/cloudseed.cpp:471-473; scan + dispatch: src/cloudseed.cpp:370-374;
// applyToggleTarget(): src/cloudseed.cpp:266-281. Primary (_a) = secondary (_b) in every shipped preset.
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
an 8-bit shift register that `Debounce()` shifts at most once per ms (`switch.cpp:34`), i.e. once
per 1 ms audio block: `Pressed()` is `state_ == 0xff` and clears
1 ms after a release, while `FallingEdge()` is `state_ == 0x80` and only fires 7 ms later
(`libdaisy/src/hid/switch.h:68-74`, `switch.cpp:39-41`). `FootswitchGestures` tracks both
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
(`src/cloudseed.cpp:379-380`), i.e. a secondary knob moved past `kKnobMoveThreshold` and wrote, or a
secondary toggle was flipped; brushing a knob below the threshold does not cancel the preset
change, and entering bank 1 re-parks both (zero writes), so pressing FS2 is never itself an edit.
The bank is derived from `PresetHeld()` after `Update()` in the same callback (`src/cloudseed.cpp:475`),
so the block that ends a hold already scans in bank 0. `processFootswitches()`
(`src/cloudseed.cpp:320-345`) reads each accessor once per callback,
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
`CloudSeed/ReverbChannel.h:317-328`), so a step would click. After the glide the knob writes its
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
(`src/cloudseed.cpp:357-366`): it re-parks on a bank change (`bank != activeBank`), when
`state.controlResetPending` is set (by `loadPresetGated()` before the load,
`src/cloudseed.cpp:216-226`), or on the first call; a re-parking call returns zero writes. It never
runs while `state.presetChangeInProgress` is set (the callback passes audio through and returns
first, `src/cloudseed.cpp:463-466`) because the re-park must not straddle a `state.knobMap` rewrite;
`controlResetPending` simply stays set until the load completes.
The glide start values come from `knobTargetValue()` (`src/cloudseed.cpp:286-307`), read for the
active bank every callback: knob values are stored verbatim in `parameters[]` by `SetParameter`,
so `GetAllParameters()` read-back is exact; the `reverse.delay` pseudo-target has no
`parameters[]` slot and is tracked in `state.reverseDelayNorm` instead (set by `loadPreset()`
from the preset's `[preset.params.reverse] delay`, then by the knob). A disabled input filter
reports its open end instead of its stored cutoff (`HighPass` 0.0 while `HiPassEnabled` < 0.5,
`LowPass` 1.0 while `LowPassEnabled` < 0.5): the engine skips it, so that is what is heard, and
`applyKnobTarget()` enables it on the first write, so the glide opens from there rather than
stepping to the stored cutoff (about 2 kHz in Chorus and Dark Plate).

`main()` settles the knob one-poles for 200 ms between `hw.StartAdc()` and `hw.StartAudio()`
(`src/cloudseed.cpp:633-649`). `AnalogControl` starts at 0.0 and only converges while
`ProcessAnalogControls()` runs, which otherwise first happens inside the audio callback: without
the settle loop the first snapshot would capture ~5 % of each real knob position and the
settling ramp itself would be read as a deliberate turn at every power-up.

**ADC smoothing**: libdaisy's default `AnalogControl` slew computes to `coeff_ = 1.0` at this
callback rate (`libdaisy/src/hid/ctrl.cpp:16` with a 1 kHz update and the 0.002 s default),
i.e. no filtering. `main()` re-tunes every knob to `KNOB_SMOOTHING_COEFF` (0.05, ~20 ms) at
`src/cloudseed.cpp:617-618`. Nothing may call `hw.SetAudioBlockSize()` / `hw.SetAudioSampleRate()`
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
(`state.controlResetPending`) and `bank` (`src/cloudseed.cpp:370-374`), then dispatches each write
through `applyToggleTarget()` (`src/cloudseed.cpp:266-281`): the three pseudo targets
(`delay_lines.max`, `reverse.enabled`, `reverse.direct_mix`) set a `PedalState` bool read
elsewhere in the callback, and `ToggleTarget_Param` calls `reverb->SetParameter()` directly (no
`outputLevelsDirty`: no toggle parameter feeds the makeup gain).

A toggle that targets `input.HiPassEnabled` or `input.LowPassEnabled` **owns** that filter's
enable: `loadPreset()` computes `state.hiPassOwnedByToggle` / `state.lowPassOwnedByToggle` from
`state.toggleMap` (`src/cloudseed.cpp:171-178`, `:205-206`), and while true `applyKnobTarget()` no
longer auto-enables the filter on the knob's first touch and `knobTargetValue()` glides the knob
from the stored cutoff instead of the open end (`src/cloudseed.cpp:243-252`, `:294-302`) - the toggle
is the only thing that switches the filter on or off.

### Presets

All preset data lives in [presets.toml](presets.toml) at the repo root and is the **built-in**
preset bank: it is embedded into the firmware image by `src/presets_toml.s` (`.incbin`, lands in
`.rodata` → SRAM) and is never modified on the pedal. A bank uploaded over USB-MIDI
(docs/USB_MIDI.md) is stored in QSPI instead and, if it is present and parses, overrides
presets.toml at boot until it is reverted or a different firmware image is flashed - see
"USB-MIDI preset upload" below. Whichever bank is active is parsed once at boot into the static
`gPresets` bank (`src/cloudseed.cpp:59`); there is still no filesystem, and the active bank's
text is never modified in place. Changing the built-in bank means editing presets.toml and
reflashing; a sound saved on the pedal (FS1 held 5 s) is stored separately in QSPI and overrides
its preset's engine values, within whichever bank is active, until it is restored (see "User
preset save / factory restore" below).

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
  (`kToggleParams`: `isReverse`, the seven filter/diffusion/shelf/cutoff enables, the two
  diffusion stage counts, `LateStageTap`, `Interpolation`) or one of the three pseudo-targets
  `"delay_lines.max"`, `"reverse.enabled"`, `"reverse.direct_mix"`. Parsed by
  `parseToggleMap()`/`parseToggleTarget()` ([src/preset_bank.cpp](src/preset_bank.cpp)) into
  `PresetData::toggleMap[bank][toggle]`; `splitTarget()` / `resolveParamTarget()` / `groupIs()`
  are shared with the knob-map parser
- `LineCount` alone must NOT appear in the file: it is driven live by the `"delay_lines.max"`
  toggle target, which selects `default_delay_lines` (off) or `max_delay_lines` (on). The parser
  rejects it
- `default_delay_lines` and `max_delay_lines` are each a whole number 1..`TotalLineCount` (5,
  `CloudSeed/DelayLineCount.h`; checked by `readLineCount()`,
  [src/preset_bank.cpp](src/preset_bank.cpp)), and `default_delay_lines` must not exceed
  `max_delay_lines` - the toggle's off state must never exceed a preset's CPU cap
- Parsed by `ParsePresetBank()` ([src/preset_bank.cpp](src/preset_bank.cpp)) into `PresetBank`
  (`count` + `PresetData[16]`); `PresetData::params[]` is indexed by `(int)Parameter`
- Applied with `CloudSeed::ReverbController::LoadPreset()`
  (`CloudSeed/ReverbController.h:47`), which copies every slot except `LineCount`
  and then re-applies all 47 through `SetParameter`
- Cycles using modulo operator: `(currentPreset + 1) % gPresets.count` (`src/cloudseed.cpp:676`)
- Preset index is persisted to QSPI flash, so **the order in presets.toml is frozen**;
  reordering or deleting entries requires bumping `SETTINGS_VERSION` (`src/pedal_storage.cpp:5`)
- `loadPreset()` (`src/cloudseed.cpp:182-212`) is the only reader of `gPresets` after boot. It loads
  the preset's saved user edit if its `UserPreset` slot is valid, else the factory values:
  `LoadPreset()` with the chosen `params`, `applyReverseWindow()` with the chosen reverse window,
  and `state.delayLinesMax` / `state.reverseEnabled` / `state.reverseDirectMix` from the chosen
  toggle pseudo-values. It then copies the preset's `knobMap`, `toggleMap`, `defaultDelayLines`,
  `maxDelayLines` and blink timings (never user-edited) into `PedalState`, derives
  `hiPassOwnedByToggle` / `lowPassOwnedByToggle` via `toggleMapTargets()`
  (`src/cloudseed.cpp:171-178`), and sets `state.outputLevelsDirty`. The audio callback and
  `ServicePresetBlink()` (handed `state.blinkPattern` by the main loop) read only those cached copies
- The delay-line count is applied in the audio callback (`updateEngineControls()`) as
  `state.delayLinesMax ? state.maxDelayLines : state.defaultDelayLines` (`src/cloudseed.cpp:385-390`)
- LED2 blinks continuously to indicate active preset (N blinks = preset N)
- A parse failure is unrecoverable: `FatalErrorLoop()` (`src/pedal_leds.cpp:45-54`, called at
  `src/cloudseed.cpp:595`) blinks both LEDs at 5 Hz forever and never starts audio. The same loop reports an
  exhausted SDRAM pool and an unexpected audio block size. `make` validates presets.toml
  before embedding it, so a rejected file cannot be built into firmware in the first place

**Boot-time memory**: preset text - the embedded presets.toml, or an uploaded bank read from
QSPI - is parsed from a dedicated 512 KB `DSY_SDRAM_BSS` arena that is not part of `custom_pool`
(`toml_arena`, `src/sdram_pool.cpp:49-59`; parsing itself is `ParsePresetText()`,
`src/sdram_pool.cpp:63-69`, a wrapper that resets the arena around the host-portable
`ParsePresetBankText()`, `src/preset_bank.cpp:870`). The same arena and parser run again at runtime to validate a USB
upload before anything is written to flash (see "USB-MIDI preset upload" below). Every parse
starts the arena at offset 0 and resets it on return, so boot and an upload validation never
overlap, and the reverb's `custom_pool` is unaffected either way. Peak measured usage at boot is
143,168 B on x86-64 (smaller on 32-bit ARM). Permanent SDRAM cost of the TOML parse arena:
512 KB, whether or not a USB upload ever happens.

### Preset Persistence

The current preset and the bypass state are both automatically saved to and loaded from QSPI flash memory, so they persist across power cycles. Writes are coalesced: flash is written `SETTINGS_SAVE_DELAY_MS` (3000 ms) after the last change.

**Implementation** ([src/pedal_storage.h](src/pedal_storage.h),
[src/pedal_storage.cpp](src/pedal_storage.cpp)): `PedalStorage` owns both
`PersistentStorage` instances. It is constructed in `src/cloudseed.cpp` as
`gStorage(hw.seed.qspi)` (`:121`), right after `hw`, because `PersistentStorage` needs the
board's `QSPIHandle&` at construction.

**Settings Structure** (`src/pedal_storage.h:15-29`):
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
1. **On startup**: `main()` (`src/cloudseed.cpp:622-625`) calls `gStorage.Init(active.hash)`
   (`src/pedal_storage.cpp:41-56`), then takes `RestoredPreset(gPresets.count)` /
   `RestoredBypass()` into `state` and calls `loadPreset()`
2. **On preset change / bypass toggle**: nothing is written yet. `loadPresetGated()` updates
   `state.currentPreset` in the main loop; the audio callback flips `state.bypass`
   (`src/cloudseed.cpp:328-331`)
3. **In main loop**: `gStorage.ServiceSettingsSave(state.currentPreset, state.bypass)`
   (`src/pedal_storage.cpp:112-125`, called on every pass at `src/cloudseed.cpp:683`) mirrors
   both values into the RAM copy and restarts a `SETTINGS_SAVE_DELAY_MS` timer whenever they
   differ. Once 3 s pass without another change it saves the `Settings` store, outside the
   audio callback. A burst of preset/bypass changes therefore costs one QSPI sector erase, and
   `Save()` skips the erase entirely when flash already matches; a power-off within 3 s of a
   change loses that change (the pedal boots in the last saved state)

**Resilience Features**:
- Invalid preset indices automatically default to preset 0 (Chorus)
  (`PedalStorage::RestoredPreset()`, `src/pedal_storage.cpp:58-61`);
  the corrected index is written back to flash 3 s after boot
- Version mismatch triggers `RestoreDefaults()` (`src/pedal_storage.cpp:54-55`); the defaults are then
  read straight-line, with no reload
- First boot / version mismatch defaults to preset 0 and `bypass = true` (`src/pedal_storage.cpp:42`)
- LED1 is re-synced to the restored bypass state after load (`src/cloudseed.cpp:629-630`)
- Non-blocking: flash writes happen in the main loop, not the audio callback
- Settings survive power cycles, firmware updates, and manual resets

**To add more persistent settings**: add the field to the `Settings` struct (`src/pedal_storage.h`), extend its `operator!=`, increment `SETTINGS_VERSION`, add a `PedalStorage` accessor that `main()` restores into `state` (like `RestoredBypass()`), and extend `ServiceSettingsSave()` (a new parameter, mirrored into the RAM copy and included in the change test).

### User preset save / factory restore

A sound can be saved into the current preset on the pedal and later reverted to its
presets.toml version. Saved edits survive power cycles but not a firmware change.

**Storage** (`src/pedal_storage.cpp:11-13`, `src/pedal_storage.h:10-11`, `:31-54`, `:74-97`):
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
struct UserPresets { uint32_t identity; UserPreset presets[kMaxPresets]; }; // 3332 B
```

`identity` (renamed from `firmwareHash`) is `(firmwareImageHash() ^ presetBankHash) * 16777619u`
(`PedalStorage::Init()`, below): the firmware image hash mixed with the FNV-1a hash of whichever
preset bank text is active, so an upload, a revert, or a reflash all discard these edits, not
just a reflash.

- Every field is 4 bytes wide, so there is no padding and `operator!=` is a `memcmp`. A
  `static_assert` keeps `sizeof(UserPresets)` + the 4 B `PersistentStorage` state word within
  one sector; it breaks if `kMaxPresets` goes above 19
- A slot is used only when `valid == USER_PRESET_VALID` (1); erased flash reads 0xFFFFFFFF.
  `valid` is the last field, and QSPI pages are programmed in ascending order, so a write cut
  off by power loss can leave a slot invalid (factory) but never half-written
- `params` is the full `GetAllParameters()` snapshot. `LineCount` comes along but is ignored,
  because `LoadPreset()` skips it. `isReverse` is preset data now and is saved for real. The
  knob map, toggle map, `default_delay_lines`/`max_delay_lines`, and blink timing are not saved

**Firmware + bank identity** (`firmwareImageHash()`, `src/pedal_storage.cpp:24-31`): FNV-1a over
the words between the linker symbols `_stext` and `_etext`, which bound `.text` + `.rodata`
(`libdaisy/core/STM32H750IB_sram.lds:36,47`) and include the embedded presets.toml.
`PedalStorage::Init(presetBankHash)` (`src/pedal_storage.cpp:41-56`, called from `main()` at
`src/cloudseed.cpp:622` as `gStorage.Init(active.hash)`, before the first `loadPreset()`) mixes
that image hash with the FNV-1a hash of whichever bank text is active
(`(imageHash() ^ presetBankHash) * 16777619u`) into `UserPresets::identity`, and calls
`RestoreDefaults()` (one sector erase) when the stored `identity` differs. Any code change, any
presets.toml change, a USB upload, or a revert therefore discards every saved preset - all four
change one of the two hashes that make up `identity`. A byte-identical reflash of the same bank
keeps them, because nothing distinguishes it from a reboot. `Settings` (preset index, bypass)
are unaffected by either hash.

**Save** (FS1 held 5 s): `processFootswitches()` sets `gSaveRequested`. On the next callback
with the preset-change gate open, the callback copies `GetAllParameters()`, `state.reverseDelayNorm`
and the three toggle pseudo-values into `gSaveSnapshot`, marks it valid, and publishes it with
`state.saveSnapshotReady` (`src/cloudseed.cpp:480-490`). The main loop (`:654-661`) copies the
snapshot into `gStorage.UserSlot(state.currentPreset)`, clears the flag, calls
`gStorage.SaveUserPresets()` (a
blocking erase + write in the main loop; audio keeps running from SRAM), and starts the
confirmation blink.

**Restore** (FS1 + FS2 held 5 s): the callback sets `state.triggerPresetRestore`. The main
loop (`src/cloudseed.cpp:662-668`) clears the slot to `UserPreset{}` (invalid), reloads the preset through
`loadPresetGated()`, which parks the knobs and toggles and drops any unsaved tweaks, calls
`SaveUserPresets()`, and starts the confirmation blink. The save is skipped when the slot was already empty.

Both handlers run before the preset-change block in the main loop, so a snapshot is always
stored into the preset that was loaded when it was taken.

**Confirmation**: `StartConfirmBlink()` / `ServiceConfirmBlink()` (`src/pedal_leds.cpp:116-139`) drive
LED1 and LED2 together for `CONFIRM_BLINKS` (3) on/off cycles of `CONFIRM_BLINK_MS` (80 ms),
even when bypassed. While the blink runs, `ServicePresetBlink()` is skipped. When it ends, LED1 is
re-set to the bypass state and LED2 goes back to the preset pattern. This is distinct from
`FatalErrorLoop()`, which blinks both LEDs at 5 Hz forever.

### USB-MIDI preset upload

The Seed's micro-USB port enumerates as a class-compliant USB-MIDI device at every boot
(`gMidiLink.Init()`, `src/cloudseed.cpp:647`, immediately before `hw.StartAudio()`); no driver
and no button press are needed. A Web MIDI host (typically a browser page) can upload a complete
`presets.toml` text over SysEx, read the active bank's text back, or revert to the bank built
into the firmware. Protocol v1 (`F0 7D 43 53 <cmd> ... F7`; INFO/BEGIN/DATA/COMMIT/REVERT/READ/
ABORT) is defined normatively in [src/preset_protocol.h](src/preset_protocol.h) and documented
for host authors in [docs/USB_MIDI.md](docs/USB_MIDI.md).

**Link** ([src/usb_midi_link.h](src/usb_midi_link.h), [src/usb_midi_link.cpp](src/usb_midi_link.cpp)):
`UsbMidiLink` drives `daisy::MidiUsbTransport` directly for receive - libdaisy's `MidiHandler`
truncates SysEx at 128 B (`libdaisy/src/hid/MidiEvent.h:2`) - and its USB-interrupt callback does
nothing but `SysExAssembler::Feed()` (`src/preset_protocol.h`), because the USB OTG FS interrupt
shares priority 0 with the audio DMA (`libdaisy/src/usbd/usbd_conf.c:102-107`,
`libdaisy/src/sys/dma.c:17-50`). Replies bypass `MidiUsbTransport::Tx()`, which splits SysEx and
sends the closing F7 as a packet of its own (`libdaisy/src/hid/usb_midi.cpp:294-318`): they are
packed into USB-MIDI 1.0 event packets by `PackSysExUsbMidi()` and sent with
`UsbHandle::TransmitInternal()`, the same CDC path.

**Main loop** (`serviceUsbPresetLink()`, `src/cloudseed.cpp:528-570`, called every pass at `:681`,
before `gStorage.ServiceSettingsSave()`): answers one received SysEx frame through
`PresetProtocol::Handle()`, performs the resulting `WriteStoredBank()` (COMMIT) or
`EraseStoredBank()` (REVERT) on `gStorage`, sends the reply, and - on a successful COMMIT/REVERT,
or a COMMIT that fails after already erasing the bank the pedal is currently running - flushes
the pending settings save and reboots with `NVIC_SystemReset()` (the bootloader reloads the app,
which boots the new bank). It also `Tick()`s the 5 s session timeout and keeps
`state.uploadActive` (a `volatile bool`, `src/cloudseed.cpp:73`) in step with the session: while a
session is open the audio callback passes the input through and skips the reverb
(`state.presetChangeInProgress || state.uploadActive`, `src/cloudseed.cpp:463`); when the session
ends, `reverb->ClearBuffers()` runs and `state.controlResetPending` is set before `uploadActive`
is cleared, so knobs and toggles moved during the session re-park instead of jumping the engine.

**Validation and storage**: an upload is checked with the firmware's own parser
(`ParsePresetText()` into a dedicated `PresetBank gUploadCheck`, `src/cloudseed.cpp:138-143`) -
the same rules `make presets-check` enforces - before anything reaches flash. QSPI layout
(`src/stored_bank.h:15-26`): a `StoredBankHeader` (`magic` "CSB1", `length`, `textHash`,
`firmwareHash`) at `STORED_BANK_HEADER_OFFSET` (0x10000, its own 4 KB sector so `REVERT` erases
only it) and the text at `STORED_BANK_TEXT_OFFSET` (0x11000) up to
`PresetProtocol::kMaxTextBytes` (96 KiB). `PedalStorage::WriteStoredBank()`
(`src/pedal_storage.cpp:83-96`) erases both, writes and verifies the text, then writes and
verifies the header last, so a write cut off by power loss leaves no valid stored bank.
`PedalStorage::StoredBankText()` (`src/pedal_storage.cpp:67-73`) hands the memory-mapped header
and text to the host-portable `ValidStoredBankText()` (`src/stored_bank.h:33-43`), which accepts
the bank only if the magic matches, its `firmwareHash` matches the running image (the same
discard-on-reflash rule saved user presets follow), the length is 1..`kMaxTextBytes` (checked
before the text is hashed, so a corrupt header never reads past the region), and the text hash
matches.

**Boot** (`src/cloudseed.cpp:584-598`): the stored bank is used if `StoredBankText()` returns one
and it parses; otherwise the embedded presets.toml (`EmbeddedPresetText()`,
`src/sdram_pool.cpp:71-74`) is used, and a parse failure there is the unrecoverable
`FatalErrorLoop()` case. Either way `gStorage.Init(active.hash)` mixes the active bank's FNV-1a
hash into the user-preset `identity` (see "Firmware + bank identity" above), so an upload, a
revert, or a reflash all wipe saved user presets.

**Tests** (`make test`): [tests/preset_protocol_test.cpp](tests/preset_protocol_test.cpp) covers
the SysEx assembler, the USB-MIDI packer, and the upload/read/revert state machine - framing,
sequencing, timeouts and error replies - plus an end-to-end upload of the parser fixture through
USB-MIDI packets and the real parser; [tests/stored_bank_test.cpp](tests/stored_bank_test.cpp)
covers `ValidStoredBankText()`; `ParsePresetBankText()` is covered in `preset_bank_test`. None
of them involve libdaisy.

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
`DSY_SDRAM_BSS` array in `src/cloudseed.cpp:132`, outside the pool.

Bump allocator, no free. The pool is aligned to the 32-byte M7 D-cache line and every block is
rounded up to 8 bytes. Exhaustion is a fatal boot error (`FatalErrorLoop()`, both LEDs at 5 Hz)
rather than a null return, because a write through 0x0 would land silently in ITCMRAM.
`custom_pool_allocate` keeps external linkage and this exact signature: the CloudSeed headers
declare it `extern`. Callers placement-new into it (e.g.
`CloudSeed/ModulatedDelay.h:41-42`), so destructors of pool-backed objects must not call
`delete` - see [docs/PERFORMANCE.md](docs/PERFORMANCE.md) §6.

The TOML parse arena is not part of this pool: it is a separate 512 KB `DSY_SDRAM_BSS`
buffer (`toml_arena`, `src/sdram_pool.cpp:49-59`), used by `ParsePresetText()`
(`src/sdram_pool.cpp:63-69`) both at boot and to validate a USB upload before it reaches
flash (see "USB-MIDI preset upload" above). `EmbeddedPresetText()` (`:71-74`) hands out the
embedded presets.toml.

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
- Builds `TotalLineCount` (5) delay lines for the mono implementation
  ([CloudSeed/DelayLineCount.h](CloudSeed/DelayLineCount.h)); `SetParameter(LineCount)` clamps the
  active count to 1..`TotalLineCount` (`CloudSeed/ReverbChannel.h:211-221`)
- Contains delay lines, diffusers, modulation

**DelayLine** ([CloudSeed/DelayLine.h](CloudSeed/DelayLine.h)):
- Core delay buffer implementation (255 lines)
- The delay buffer itself is SDRAM-pool-backed (placement-new into `custom_pool_allocate`,
  `CloudSeed/ModulatedDelay.h:41-42`)
- `tempBuffer`, `mixedBuffer`, and `filterOutputBuffer` are real heap allocations
  (`CloudSeed/DelayLine.h:52-54`, freed at `:74-76`)

**AllpassDiffuser, MultitapDiffuser**: Diffusion stages
**ModulatedAllpass, ModulatedDelay**: Modulated processing

## Build System

### Checkout and prerequisites

`libdaisy/`, `DaisySP/` and `Terrarium/` are git submodules (`.gitmodules`); the build reads
all three (`Makefile:16-24`, `:42`), so a checkout without them fails at the first `include`.

```bash
git clone --recurse-submodules https://github.com/jimbattin/DaisyCloudSeed.git
# or, in an existing clone / after a pull that moves a submodule:
git submodule update --init --recursive
```

The superproject pins exact commits: libdaisy `v8.1.0`, DaisySP `V1.0.0`, Terrarium `main` at
`cd6c80d`. The `branch =` values in `.gitmodules` are those tag names, not branches, so never use
`git submodule update --remote`: it would move the submodules off the pinned commits.
`--recursive` is required: libdaisy v7+ moved CMSIS, the STM32H7 HAL and the USB device library
into nested submodules (`libdaisy/.gitmodules`), and neither libdaisy nor the app compiles
without them. It also fetches `libdaisy/tests/googletest` and `DaisySP/DaisySP-LGPL`, which the
firmware does not need (DaisySP's `make` builds the LGPL library only if it is present,
`DaisySP/Makefile:227-230`). A full recursive checkout of libdaisy is about 330 MB of working
tree plus 480 MB of git objects; `--shallow-submodules` on the clone reduces the git part.

Tools: the Daisy Toolchain (`arm-none-eabi-gcc`, `make`, `dfu-util`; this tree builds with Arm GNU
Toolchain 13.3.Rel1) and a host `gcc`/`g++` (`HOSTCC`/`HOSTCXX`, `Makefile:50-51`) - plain `make`
needs the host compilers too, because it builds `preset_check` to validate presets.toml.
Then `make libs` once (and after every submodule update), then `make`.

### Building Libraries

```bash
# Rebuild all libraries (libcloudseed, DaisySP, libdaisy).
# Makefile:113-116 runs `clean all` in each, so this is a full rebuild of all three.
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

`make` also runs `$(MAKE) -C CloudSeed` on every invocation (`Makefile:32-39`): the sub-make
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
`preset_check --validate presets.toml` (`Makefile:53-70`). `preset_check` calls
`ParsePresetBankText()` with the file's full length, exactly as the pedal does at boot, so an
empty file or an embedded NUL byte is rejected too. Validation covers non-ASCII bytes
(any byte >= 0x80, comments included, rejected with its line and column before tomlc99 sees the
text: `checkAscii()`, `src/preset_bank.cpp:811-837`), TOML syntax,
missing/unknown/misplaced parameters, unknown preset- and root-level keys, out-of-range
scalars (a parameter value that is non-finite or outside 0..1; `max_delay_lines` /
`default_delay_lines` that are not whole numbers 1..5 or where the default exceeds the max;
`blinks`, the ms fields), an unresolvable `[preset.knob_map]` / `[preset.toggle_map]` target
(unknown group/parameter, wrong group, a runtime parameter, or - for toggles - a parameter not
in `kToggleParams`), and a document too large for the boot parse arena - the host tool allocates
through a replica of `TOML_ARENA_SIZE` (512 KB, 8-byte aligned, no reuse), so `presets.toml: 10
presets valid, boot arena peak 143168 of 524288 bytes` is the same peak the pedal sees. Host
pointers are 64-bit, so the reported peak over-estimates the 32-bit target: a pass here implies
a fit on hardware. A near-miss should be fixed by raising `TOML_ARENA_SIZE` (`src/sdram_pool.cpp:49`),
not by loosening the host check.

Preset values are free to change: `make presets-check` fails only on input the parser rejects.
There is no golden-value comparison; the original factory values are recoverable from git
history.

A file that somehow reaches the pedal broken is unrecoverable at runtime: `FatalErrorLoop()`
(`src/pedal_leds.cpp:45-54`, called at `src/cloudseed.cpp:595`) blinks both LEDs at 5 Hz forever and never starts
audio.

### Host unit tests

```bash
make test
```

Builds seven host executables into `build/` with `HOSTCC`/`HOSTCXX` (`Makefile:80-121`) and runs them;
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
  must each be rejected with a specific message (non-ASCII bytes in a value and in a comment
  among them), a check of the line and column reported for a non-ASCII byte, and two
  mutations that must be accepted. `ParsePresetBankText()` is checked to parse exactly
  `length` bytes of text with no NUL terminator, to leave its source untouched, to release
  every allocation on success and on failure, and to reject empty text, embedded NUL bytes,
  and a failed scratch allocation
- `engine_alloc_test` - the whole CloudSeed library compiled for the host, with counting
  replacements for `operator new` and `custom_pool_allocate`: after boot, every parameter is
  swept 0 → 1 → 0.5 through `SetParameter()` with a `Process()` block after each write, and
  must cause zero heap and zero pool allocations (see "Performance Architecture")
- `preset_protocol_test tests/fixtures/two_presets.toml` - `SysExAssembler` framing (split
  frames, real-time bytes, overflow, aborted, empty and restarted frames), `PackSysExUsbMidi()`
  CIN/padding, and the `PresetProtocol` state machine: INFO fields, BEGIN length checks and
  restart, chunked upload + COMMIT including a full 98,304-byte upload and an overrun at that
  size, duplicate/gapped/wrapped sequence numbers, length/hash mismatches (hash bits 28-31
  included), `ParseError` and `FlashError` replies, READ chunking, the 5 s session timeout,
  ABORT, REVERT, and foreign/unknown SysEx. Every reply is checked to be one well-formed
  7-bit SysEx message. An end-to-end case uploads the fixture through USB-MIDI packets and the
  real parser, and checks that a rejected bank's COMMIT reply carries the parser's own message
- `stored_bank_test` - `ValidStoredBankText()`: erased flash, wrong magic, a different firmware
  image, corrupted text or hash, and the 1..`kMaxTextBytes` length bounds

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

Both targets come from `libdaisy/core/Makefile:348-352` and require `dfu-util`. `program-boot`
flashes `BOOT_BIN`, the bootloader shipped with libdaisy (`dsy_bootloader_v6_4-intdfu-2000ms.bin`
at v8.1.0, `libdaisy/core/Makefile:220`); a pedal still running an older bootloader keeps working
but misses its fixes (v6.3+ carries libdaisy's QSPI write-protect fix), so re-run it after a
libdaisy bump that ships a new one. A `BOOT_SRAM` build cannot be flashed with `make program`
(openocd) - libdaisy errors out on that path (`libdaisy/core/Makefile:340-341`). Same procedure
as `README.md:72-83`.

### Compiler Configuration

**Platform**: ARM GCC (`arm-none-eabi-gcc`)
**CPU**: Cortex-M7 (`-mcpu=cortex-m7`, `CloudSeed/Makefile:70`)
**Optimization**: `-O3` (`Makefile:20`, `CloudSeed/Makefile:20`)
**FPU**: Hard float (`-mfpu=fpv5-d16 -mfloat-abi=hard`, `CloudSeed/Makefile:73-76`)
**Language**: C++14 (`-std=gnu++14`, `CloudSeed/Makefile:67`)
**Float flags**: `-ffast-math` on both the app (`Makefile:44`) and the library
(`CloudSeed/Makefile:106`); the library additionally uses `-fno-exceptions`,
`-finline-functions`, and `-fno-aggressive-loop-optimizations` (`CloudSeed/Makefile:104-108`)
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

`applyKnobTarget()` (`src/cloudseed.cpp:229-262`) turns `LowPassEnabled` on the first time that
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
(`src/cloudseed.cpp:229-262`).

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
(`src/cloudseed.cpp:266-281`).

### 2. Modifying Parameter Ranges

Knobs are read raw: `hw.knob[kKnobIndex[i]].Value()` (`src/cloudseed.cpp:468-470`) yields 0.0-1.0
and is handed straight to `SetParameter`, which applies the engine's own scaling
(`ReverbController::GetScaledParameter`). There is no per-knob min/max any more - the six
`::daisy::Parameter` wrappers were removed when knob targets became data.

To restrict a knob's travel, scale in `applyKnobTarget()` (`src/cloudseed.cpp:229-262`) before the
`SetParameter` call, e.g. `value = 0.5f + 0.5f * value;` for the upper half of the range, **and**
apply the inverse for that parameter in `knobTargetValue()` (`src/cloudseed.cpp:286-307`), e.g.
`(v - 0.5f) / 0.5f` clamped to 0..1. The takeover glide runs in knob space from
`knobTargetValue()` to the pot, so without the inverse its first step writes `0.5 + 0.5 * stored`
instead of `stored` - a jump. A stored value outside the knob's range still jumps to the nearest
end of that range on the first write. Both changes affect every preset that maps a knob to that
parameter.

The knob response constants: `KNOB_SMOOTHING_COEFF` (`src/cloudseed.cpp:50`, the ADC one-pole), and in
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
- presets.toml must be plain ASCII, comments included (`line L, column C: non-ASCII byte 0xNN`):
  tomlc99 passes plain `char`s to `isdigit()`, which is undefined for bytes >= 0x80, so
  `ParsePresetBank()` rejects them before parsing (`src/preset_bank.cpp:811-837`, called at `:848`)
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
- `blinks` is 1..20; `default_delay_lines` and `max_delay_lines` are each a whole number
  1..`TotalLineCount` (5; a fraction or `nan` fails with `preset N: <key> must be a whole number 1..5`), and
  `default_delay_lines` must not exceed `max_delay_lines`
  (`preset N: default_delay_lines exceeds max_delay_lines`); the ms fields are optional whole
  numbers 0..60000, written `300` or `300.0` (defaults 150/150/5000 when absent; a fraction, a
  string or an out-of-range value fails with `preset N: <key> must be a whole number 0..60000`);
  `name` must be non-empty and at most 31 bytes (`kMaxPresetNameLen` - 1; longer fails with
  `preset N: name longer than 31 bytes`); at most `kMaxPresets` (16) presets
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

**File**: [CloudSeed/DelayLineCount.h](CloudSeed/DelayLineCount.h)

```cpp
constexpr int TotalLineCount = 5;  // CloudSeed/DelayLineCount.h:14
```

`TotalLineCount` is the single definition of how many `DelayLine`s `ReverbChannel` builds, and
everything that depends on it follows automatically:
- `ReverbChannel::SetParameter(LineCount)` clamps the active count to 1..`TotalLineCount`
  (`CloudSeed/ReverbChannel.h:211-221`), so no caller can make `Process` loop past the lines that
  exist (`:401`, `:404`). The lower bound matters too: `ReverbController::LoadPreset()` re-applies
  the stored `LineCount`, which is 0 at boot until the audio callback sets the real count
- `readLineCount()` (`src/preset_bank.cpp:195-210`) validates `default_delay_lines` /
  `max_delay_lines` against it, so `make` rejects a preset asking for more lines than exist,
  naming the preset and key: `preset N: max_delay_lines must be a whole number 1..<TotalLineCount>`.
  The `preset_check` and `preset_bank_test` rules list the header as a prerequisite, so they are
  rebuilt when it changes

After lowering it, `make` fails until every preset's `default_delay_lines` / `max_delay_lines`
fits (nine ship `max_delay_lines = 5.0`). Also update the `1..5` wording in presets.toml's
header, this file and README.md, and the expected `1..5` messages and 5.0 values in
`tests/preset_bank_test.cpp` / `tests/fixtures/two_presets.toml`. Raising it costs SDRAM pool
memory and CPU per line ("Through the Looking Glass" already crackles above 4).

### 5. Modifying Switch Behavior

**File**: [presets.toml](presets.toml) for reassigning a switch (see "1. Changing Control
Mappings" above); [src/cloudseed.cpp](src/cloudseed.cpp) for the mechanism itself
(`src/cloudseed.cpp:115-116` for `kToggleIndex`, the presets.toml-order -> `hw.switches[]` map;
`src/cloudseed.cpp:352-391` for `updateEngineControls()`, which scans and dispatches both knobs and
toggles; `src/cloudseed.cpp:266-281` for `applyToggleTarget()`; `src/cloudseed.cpp:493-501` for
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
(`src/cloudseed.cpp:266-281`); see "1. Changing Control Mappings" above.

`hw.switches[...].Pressed()` — an 'ON' toggle counts as pressed
(`src/cloudseed.cpp:471-473`) — is read for every toggle, not `.Read()`.

### 6. Adjusting Audio Buffer Size

**File**: [src/cloudseed.cpp](src/cloudseed.cpp) (`src/cloudseed.cpp:633-649`)

```cpp
// Current: 48 samples per block
hw.StartAdc();
// ... 200 ms knob one-pole settle loop (see "Knob take-over" above) ...
hw.StartAudio(audioCallback);
```

Three sizes are coupled and must change together:
- `DaisyPetal::Init()` sets the hardware block size to 48 (`libdaisy/src/daisy_petal.cpp:90`);
  override it with `hw.SetAudioBlockSize(n)` immediately after `hw.Init()`
  (`src/cloudseed.cpp:574`). It must precede the block-size guard (`:581-582`) and the knob
  `SetCoeff()` loop (`:617-618`): it re-runs `SetHidUpdateRates()`, which re-initialises every
  knob's one-pole (see "ADC smoothing")
- `AUDIO_BUFFER_SIZE` (`src/cloudseed.cpp:31`) sizes the file-scope `gInputBuffer`, `gWetBuffer`,
  `gReverseBuffer` and `gReverbInputBuffer` (`src/cloudseed.cpp:315-318`) and every callback loop;
  `main()` stops in `FatalErrorLoop()` if `hw.AudioBlockSize()` differs (`src/cloudseed.cpp:581-582`)
- `ReverbController::bufferSize` (`CloudSeed/ReverbController.h:23`) sizes the controller's
  fixed member arrays

Raising the hardware block size alone stops boot at that guard; raising it with
`AUDIO_BUFFER_SIZE` but not `bufferSize` overruns the controller's arrays.
Smaller blocks = lower latency, higher CPU load; larger blocks = higher latency, lower CPU load.

These are counted in callbacks and assume the 1 ms block (48 samples at 48 kHz); rescale them
with the block period:
- `kKnobGlideBlocks` (`src/knob_bank.h:26`): 50 blocks = the 50 ms takeover glide
- `kLongHoldBlocks` (`src/footswitch_gestures.h:24`): 5000 blocks = the 5 s save / restore holds
- `KNOB_SMOOTHING_COEFF` (`src/cloudseed.cpp:50`): a per-callback one-pole coefficient, ~20 ms
  at 1 kHz
- libdaisy's `Switch::Debounce()` shifts at most once per ms (`libdaisy/src/hid/switch.cpp:34`):
  with longer blocks it shifts once per block, so the 8-shift press latch and 7-shift release
  edge stretch with the block

Everything timed with `System::GetNow()` / `System::Delay()` (the blink patterns, the
confirmation blink, the 3 s settings save, the 200 ms knob settle loop) is independent of the
block size.

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

**CloudSeed Audio Callback** (`audioCallback()` at `src/cloudseed.cpp:452-504` - runs once per
48-sample block, i.e. at 1 kHz):
1. Process analog/digital controls and push both LEDs with `UpdateLeds()` (`:455-457`, two `Led::Update()` calls). Once
   audio runs this is the only caller of `Update()`: it is a read-modify-write that would race
   with the main loop
2. `processFootswitches()` (`:320-345`): one `gFootswitches.Update()` call with both switches'
   `Pressed()` / `FallingEdge()` returns `FootswitchEvents`. `toggleBypass` flips
   `state.bypass` and sets LED1 (the main loop persists it later), `cyclePreset` sets
   `triggerPresetChange`, `savePreset` sets `gSaveRequested`, `restorePreset` sets
   `triggerPresetRestore`
3. Read `state.bypass` once. While `state.presetChangeInProgress` **or** `state.uploadActive`
   is set - the main loop is rewriting `state.knobMap` / `state.toggleMap` and every engine
   parameter, or a USB upload session owns the reverb (`serviceUsbPresetLink()`, below) - the
   callback copies the input to the output and returns (`:463-466`): no knob/toggle scan, no
   reverb, `reverseMix` does not advance
4. Read all six knobs with `hw.knob[kKnobIndex[i]].Value()` (`:468-470`) and all four toggles
   with `hw.switches[kToggleIndex[i]].Pressed()` (`:471-473`) and call
   `updateEngineControls(gFootswitches.PresetHeld() ? 1 : 0, knobPositions, togglePositions)`
   (`:475-476`, body `:352-391`), which holds every reverb write:
   - read every knob's current target value for the active bank with `knobTargetValue()`, then
     one `gKnobs.Scan(bank, reset, …)` call (`:357-364`) re-parks on any bank or preset
     transition (zero writes that block) and otherwise returns the knobs to write: glide steps
     for a knob taking over, then the knob's own position; `reset` is
     `state.controlResetPending`, read once and shared with the toggle scan (`:361`)
   - dispatch each returned write through `applyKnobTarget()` using `state.knobMap[bank][i]` —
     the cached copy, never `gPresets` (`:365-366`)
   - one `gToggles.Scan(bank, reset, togglePositions, …)` call (`:370-371`) re-parks the same
     way (`toggle_bank.h`), then returns the levers to write: a flipped lever writes its
     position (up = on) immediately, no glide; dispatch each through `applyToggleTarget()`
     using `state.toggleMap[bank][i]` (`:373-374`)
   - any knob or toggle write while in bank 1 calls `gFootswitches.MarkEdited()`, cancelling
     that hold's preset change (`:379-380`)
   - select the delay line count as `state.delayLinesMax ? state.maxDelayLines :
     state.defaultDelayLines` (the `"delay_lines.max"` toggle target, applied above if it was
     just flipped) and write `Parameter::LineCount` when it differs from
     `state.prevNumDelayLines` (`:382-390`). Both candidates are exact copies, so the `!=` test
     is exact; a preset with different default/max counts is re-applied on the first callback
     after the change
   Then, if `gSaveRequested` is set and the previous snapshot has been consumed, copy
   `GetAllParameters()`, `state.reverseDelayNorm` and the three toggle pseudo-values into
   `gSaveSnapshot` and publish it with `state.saveSnapshotReady` (`:480-490`). This runs only
   past the preset-change gate, so a half-loaded preset is never captured
5. `refreshOutputLevels()` (`:491`, body `:395-410`): `makeupGain` and `scaledDryOut` are
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
   1), and the left input channel is copied into `gInputBuffer` (`:495-496`)
7. `state.reverseDirectMix` (the `"reverse.direct_mix"` toggle target) picks the render helper
   (`:498-501`), each of which writes the output scaled by `makeupGain`:
   - Into-reverb, `reverseDirectMix` off, `renderReverseIntoReverb()` (`:417-434`): reverse the
     dry input into `gReverseBuffer`, scale it to the injected reverse, add that to the reverb
     input (`gReverbInputBuffer`), run `reverb->Process(gReverbInputBuffer, gWetBuffer, …)`,
     then `out = (wet − scaledDryOut * injectedReverse) * makeupGain`; subtracting
     `scaledDryOut * injectedReverse` cancels the reverb's dry pass-through of the injected
     reverse, so the forward dry stays clean and the reverse is heard only through the wet tail
   - Direct-mix, `reverseDirectMix` on, `renderReverseDirect()` (`:438-449`):
     `reverb->Process(gInputBuffer, gWetBuffer, …)`, record the reverb output into
     `reverseDelay` for backward playback, then
     `out = (wet + reversed * REVERSE_LEVEL * mix) * makeupGain`, the reverse audible on its own
   Both ramp the reverse via a smoothed mix toward `reverseTarget`, kept in a local during the
   sample loop and stored back to `state.reverseMix` afterwards, so the `"reverse.enabled"`
   toggle switches click-free (engine in `CloudSeed/ReverseDelay.h`)
8. If bypassed, overwrite the output with the input (`src/cloudseed.cpp:502-503`)

When `state.bypass` is set, output is a straight copy of the input — but the reverb (and the
reverse mix) is still processed, deliberately, to suppress an audible 1 kHz whine (the
`reverb->Process()` call inside whichever render helper runs, `src/cloudseed.cpp:417-449`). When
`state.presetChangeInProgress` or `state.uploadActive` is set, the reverb is skipped entirely and
the input is passed through (`src/cloudseed.cpp:463-466`).

**CloudSeed Main Loop** (`main()` while loop at `src/cloudseed.cpp:651-690` - free-running, no sleep):
1. **Save / restore** (`:654-668`): a published `gSaveSnapshot` is copied into the current
   preset's `UserPreset` slot (`gStorage.UserSlot()`) and written with `gStorage.SaveUserPresets()`; a restore clears the
   slot, reloads the preset with `loadPresetGated()`, and saves. Both start the confirmation
   blink. See "User preset save / factory restore"
2. **Handle preset changes** (`:674-678`): clear `triggerPresetChange`, then
   `loadPresetGated(next)` (`:216-226`) sets `presetChangeInProgress` and
   `state.controlResetPending` (so the knob and toggle positions are re-snapshotted before the
   new preset loads), and between two `std::atomic_signal_fence(std::memory_order_seq_cst)`
   compiler barriers updates `state.currentPreset` and runs `loadPreset()`, then clears the flag
   to re-enable audio processing; `StartPresetBlink(state.blinkPattern)` follows.
   `loadPreset()` is synchronous, so there is no delay: the gate reopens as soon as it returns.
   The seven flags shared with the callback (`bypass`, `triggerPresetChange`,
   `triggerPresetRestore`, `saveSnapshotReady`, `presetChangeInProgress`, `uploadActive`,
   `controlResetPending`) are `volatile bool` (`:68-74`)
3. **USB preset upload/read/revert**: `serviceUsbPresetLink()` (`:681`, body `:528-570`) answers
   one received SysEx frame per pass (`UsbMidiLink` / `PresetProtocol`, see "USB-MIDI preset
   upload" above), performs the blocking QSPI write on COMMIT or the erase on REVERT, and keeps
   `state.uploadActive` in step with the session so the callback's passthrough gate opens and
   closes with it. A successful COMMIT or REVERT - and a COMMIT that fails after already
   erasing the bank the pedal is running - reboots via `NVIC_SystemReset()`
4. **Persist settings**: `gStorage.ServiceSettingsSave()` (`:683`) - the coalesced flash write described
   under Preset Persistence
5. **Update LEDs**: `ServiceConfirmBlink()`, else `ServicePresetBlink()` (`:686-687`)
6. `keepCoreBusy()` (`:689`, defined at `:517-523`) — deliberate busy work that keeps the core
   out of idle between callbacks and reduces an audible 1 kHz whine: it advances a `volatile`
   phase by 0.001 (wrapped at `TWO_PI`) and stores `sinf(phase)` to a `volatile` sink, so every
   pass performs a real FPU `sinf()` plus real loads and stores (a constant argument would be
   folded away at compile time). There is no loop delay; the loop spins

**Performance Architecture**:
- **Audio callback**: Time-critical, optimized for low latency
  - Control scanning (knobs, toggles, footswitch gestures), engine parameter writes, and audio
    processing
  - Sets trigger flags for heavy operations
  - No flash writes or preset loading
  - **Allocation-free**: every knob or toggle write goes through `ReverbChannel::SetParameter`
    inside the audio interrupt, so no parameter update may touch the heap or the SDRAM pool;
    `make test` enforces this (`tests/engine_alloc_test.cpp`). The seed-derived values every
    stage draws its delays, gains and modulation from live in fixed-size
    `AudioLib::SeedSeries<N>` members (`CloudSeed/AudioLib/ShaRandom.h:15-57`): SHA-256 runs only
    when a seed actually changes (`TapSeed`, `DiffusionSeed`, `DelaySeed`, `PostDiffusionSeed`),
    a `CrossSeed` write only re-blends the two cached series, and `UpdateLines()` and
    `MultitapDiffuser::Update()` read the cached values into fixed arrays. The per-write cost
    of the default knob targets (KNOB_5 `TapDecay`, KNOB_6 `LineDecay`, secondary KNOB_4/5
    `LineModAmount`/`LineModRate`) is therefore bounded arithmetic - see
    [docs/PERFORMANCE.md](docs/PERFORMANCE.md) §9
  - The USB-MIDI receive path shares this: the ISR (priority 0, same as the audio DMA) does
    nothing but `SysExAssembler::Feed()`; everything else about an upload runs in the main loop
- **Main loop**: Non-critical background tasks
  - Preset switching (includes buffer clearing)
  - Flash memory writes (settings, user preset save/restore)
  - USB preset link service (`serviceUsbPresetLink()`): services one SysEx frame per pass, and
    performs the COMMIT/REVERT QSPI writes (blocking, up to ~3 s) while the callback's
    passthrough gate is held closed
  - LED blink state machine and save/restore confirmation blink
- **FPU flush-to-zero** is enabled once at the top of `main()` before `hw.Init()`
  (`src/cloudseed.cpp:573`) — see [docs/PERFORMANCE.md](docs/PERFORMANCE.md) §1

This separation prevents audio glitches during preset changes, flash writes, and USB uploads.


## Debugging

### Serial Debug Output

```cpp
// In src/cloudseed.cpp, add:
#include "daisy_seed.h"
using namespace daisy;

// In setup (true would block boot until a USB serial host opens the port):
hw.seed.StartLog(false);

// Anywhere:
hw.seed.PrintLine("Debug: value = %f", some_value);
```

`StartLog()` puts the same micro-USB port into USB CDC (serial) mode, and the USB-MIDI preset
link (`gMidiLink.Init()` in `main()`) needs it in MIDI mode. Remove the `gMidiLink.Init()`
call while logging; USB preset upload is unavailable in such a build.

### LED Indicators

```cpp
// Terrarium LEDs are daisy::Led file statics gLed1/gLed2 in src/pedal_leds.cpp (:18-19,
// init LedsInit() :25-31). DaisyPetal has no led1/led2 members - it exposes
// SetRingLed/SetFootswitchLed/ClearLeds for the Daisy Petal board's own I2C LED driver,
// which Terrarium does not use. For a debug value, add a setter next to SetBypassLed():
gLed2.Set(parameter_value);  // 0.0-1.0
```

`Set()` is enough once audio runs: the audio callback calls `Led::Update()` for both LEDs every
block (`src/cloudseed.cpp:457`), and nothing else may call it after `hw.StartAudio()` (it is a
read-modify-write that races with the callback). Only before `StartAudio()` must you call
`UpdateLeds()` yourself.

Caveat: `ServicePresetBlink()` (`src/pedal_leds.cpp:67-113`) runs on every main-loop pass and sets
LED2 at each blink transition (and turns it off while bypassed), so it overwrites debug values
unless that call is removed. LED1 is likewise re-set on every
bypass toggle (`src/cloudseed.cpp:330`), and both LEDs are driven by the save/restore confirmation
blink (`ServiceConfirmBlink()`, `src/pedal_leds.cpp:124-139`).

### Common Issues

**Build Errors**:
- Ensure submodules are initialized: `git submodule update --init --recursive`
- Rebuild libraries: `make libs` (runs `clean all` in CloudSeed, DaisySP, and libdaisy)
- Clean build: `make clean && make`
- Expected warnings on a clean `make`, none from `src/` or `CloudSeed/`: `array subscript has
  type 'char'` from the vendored `third_party/tomlc99/toml.c` (`isdigit()` on a `char`;
  unreachable, because `ParsePresetBank()` rejects every byte >= 0x80 before tomlc99 runs),
  `FP registers might be clobbered despite 'interrupt' attribute`
  from libdaisy's `Default_Handler` (`libdaisy/core/startup_stm32h750xx.c:1560`, an infinite loop
  that never returns), newlib's `_close`/`_read`/... `is not implemented and will always fail`,
  and `LOAD segment with RWX permissions` at link time

**Audio Issues**:
- Check buffer size (48 samples typical)
- Verify SDRAM allocation for CloudSeed
- Check parameter ranges (0.0-1.0 typical)

**Control Issues**:
- Verify ADC channel mapping in Terrarium
- Check the knob smoothing coefficient (`KNOB_SMOOTHING_COEFF`, `src/cloudseed.cpp:50`) and the
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
- SDRAM: 53,138,176 B of 64MB (79.18%), entirely static `DSY_SDRAM_BSS`: `custom_pool`
  50,331,648 B + `reverseDelayBuffer` 768,000 B (192,000 floats = 4 s @ 48 kHz, sized so the
  2000 ms max reverse window clears `ReverseDelay`'s `size / 2` clamp) +
  `CloudSeed::FastSin::data` 131,072 B + `AudioLib::ValueTables` tables 1,280,032 B (see the
  `.sdram_bss` section in `build/cloudseed.map`) + the 512 KB `toml_arena` (not part of
  `custom_pool`; `src/sdram_pool.cpp`, used at boot and to validate USB uploads) + the 96 KiB
  `gUploadText` upload receive buffer + the ~4.8 KB `gUploadCheck` validation `PresetBank`
  (`src/cloudseed.cpp`)
- The runtime heap is not in this report: it grows from `end` in RAM_D2
  (`libdaisy/core/STM32H750IB_sram.lds:239-250`), which is where `DelayLine`'s `tempBuffer`,
  `mixedBuffer`, and `filterOutputBuffer` (`CloudSeed/DelayLine.h:52-54`) land
- SRAM (`.text`+`.data`, `BOOT_SRAM` region): 233,208 B of 480KB (47.45%). Of that, the
  embedded `presets.toml` blob is 49,116 B (`build/presets_toml.o` - it carries the
  per-preset `[preset.knob_map]`, `[preset.toggle_map]`, `[preset.params.reverse]` and
  `[preset.params.delay_lines]` tables), tomlc99 is 14,371 B, and `preset_bank.o` is 8,285 B
- DTCMRAM: 45,420 B of 128KB (34.65%) — up from 30,284 B; the added 15,136 B is libdaisy's USB
  device stack, pulled in by `UsbMidiLink`: the four 2 KB `UserRxBufferFS`/`UserTxBufferFS`/
  `UserRxBufferHS`/`UserTxBufferHS` ring buffers, `midi_usb_handle` (2,112 B),
  `hpcd_USB_OTG_FS` (1,292 B) and the two 732 B `hUsbDeviceFS`/`hUsbDeviceHS` descriptors, plus
  `gMidiLink` (616 B) and `gPresetProtocol` (312 B). Also includes the 4,804 B `gPresets` bank
  (40 B of that per preset slot is the knob + toggle maps: 24 B knobs, 16 B toggles), the
  6,720 B `gStorage` (6,676 B of it is the user-preset `PersistentStorage`, which keeps a
  defaults copy and a live copy of the 3,332 B `UserPresets`), and the 208 B `gSaveSnapshot`
- RAM_D2_DMA: 17,956 B of 32KB (54.80%) — up from 16,968 B, the `MidiUsbTransport` rx/tx buffers
- QSPI: `Settings` in sector 0 (offset 0), `UserPresets` in sector 1 (offset 0x1000), and the
  uploaded-bank `StoredBankHeader` (offset 0x10000) + text (offset 0x11000), all inside the
  256 KB below the bootloader's program area at 0x90040000
- Boot time: about +10 ms for `MidiUsbTransport::Impl::Init()` (`System::Delay(10)`,
  `libdaisy/src/hid/usb_midi.cpp:125`); the new SDRAM buffers cost nothing at boot, since
  `.sdram_bss` is `NOLOAD` and never zeroed

### Optimization Tips

See [docs/PERFORMANCE.md](docs/PERFORMANCE.md) for the concrete list of performance/correctness
fixes applied to the CloudSeed DSP (FPU denormal handling, parameter storage, hot-loop
modulo/precision fixes, placement-new/delete destructor safety, build flags, the redundant
bypass copy, allocation-free parameter updates, and engine state defined before its first
read), each with the exact file/line and pattern it addresses.

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

This fork differs from the original CloudSeed and its predecessors (`CloudSeed/DelayLineCount.h`):
- **Original CloudSeed plugin**: 8 (or 12) delay lines, stereo
- **DaisyCloudSeed (Daisy Patch)**: 2 delay lines, stereo
- **GuitarML Terrarium fork**: 4 delay lines, mono
- **This fork**: 5 delay lines, mono (`constexpr int TotalLineCount = 5;`)

The trade-off: More delay lines in mono = richer reverb tail.

### GuitarML Modifications

Key changes in this fork:
1. Adapted for Terrarium hardware (mono, 6 knobs, 4 switches)
2. Increased delay line count from 2 to 5
3. Added preset cycling via footswitch
4. Simplified control scheme for guitar pedal use
5. Added delay line switching via toggle switches
6. **Bypass state persisted to flash** alongside the preset (`SETTINGS_VERSION = 2`, `src/pedal_storage.h:15-29`)
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
    balance changes (`refreshOutputLevels()`, `src/cloudseed.cpp:395-410`, applied by the render
    helpers at `:417-449`), derived from `reverb->GetAllParameters()` only
    when `DryOut`/`EarlyOut`/`MainOut` change (`state.outputLevelsDirty`), not per block
14. **1 kHz whine mitigation**: the reverb is still processed while bypassed
    (`src/cloudseed.cpp:493-503`) and every main-loop pass calls `keepCoreBusy()`
    (`src/cloudseed.cpp:517-523`, `:689`): a real FPU `sinf()` of a `volatile` phase plus `volatile`
    loads and stores, so the core never idles between callbacks
15. **FPU flush-to-zero enabled at boot** to eliminate denormal stalls (`src/cloudseed.cpp:573`)
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
18. **Footswitch gestures** (`processFootswitches()`, `src/cloudseed.cpp:320-345`): tapping
    FOOTSWITCH_2 cycles the preset on release; holding it selects the secondary knob and toggle
    bank, and its release cycles only if no secondary knob or toggle wrote during the hold.
    FOOTSWITCH_1 toggles bypass on release. Both are
    tracked by `FootswitchGestures` ([src/footswitch_gestures.h](src/footswitch_gestures.h)), which holds
    each switch from `Pressed()` until `FallingEdge()` so a bounce cannot drop it, and treats any
    overlap of the two as a chord whose releases do nothing
19. **Active preset configuration cached in `PedalState`** (`loadPreset()`,
    `src/cloudseed.cpp:182-212`): `knobMap`, `toggleMap`, `defaultDelayLines`, `maxDelayLines` and
    the blink timings are copied out of `gPresets` on load, so the audio callback and the blink
    state machine never touch the parsed bank. While `state.presetChangeInProgress` is set the
    callback passes audio through and makes no engine write, so a load cannot be read
    half-applied
20. **Per-preset user save / factory restore**: FOOTSWITCH_1 held 5 s stores the engine state
    (isReverse and the three toggle pseudo-values included; `delay_lines.max` is stored as its
    on/off state, the line counts always come from presets.toml) and the reverse window into the
    current preset's slot of a QSPI
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
22. **USB-MIDI preset upload**: the Seed's micro-USB port is a class-compliant USB-MIDI device
    at every boot (`gMidiLink.Init()`, `src/cloudseed.cpp:647`); a host can upload a complete
    `presets.toml` text, read the active bank back, or revert to the built-in bank over SysEx
    protocol v1 ([src/preset_protocol.h](src/preset_protocol.h),
    [docs/USB_MIDI.md](docs/USB_MIDI.md)). An upload is validated with the firmware's own parser
    before anything reaches flash, is stored in QSPI (`PedalStorage::WriteStoredBank()`), and a
    successful commit or revert reboots the pedal. Uploading, reverting, or reflashing all wipe
    saved user presets, because all three change the bank-hash half of the user-preset
    `identity` (see "Firmware + bank identity" under "User preset save / factory restore")

### Version Information

Check git history for recent changes:
```bash
git log --oneline -10
```

Run `git branch --show-current` for the current branch and the command above for recent
changes; do not restate them here.

## Quick Reference Card

### Build & Flash
```bash
make clean         # Clean previous build
make libs          # Rebuild libdaisy, DaisySP, and libcloudseed (clean all)
make presets-check # Validate presets.toml without building (also run automatically by `make`)
make test          # Host unit tests (knob/toggle/footswitch state machines, preset parser, engine allocations)
make               # Build CloudSeed
make program-boot  # One time: flash the Daisy bootloader (BOOT_SRAM prerequisite)
make program-dfu   # Flash the app (reset, hold BOOT until rapid blink, then run)
```

### File Locations
- Control mapping: `[preset.knob_map]` / `[preset.toggle_map]` in `presets.toml`; dispatch at
  `src/cloudseed.cpp:365-366` (knobs) / `:373-374` (toggles), `applyKnobTarget()`
  `src/cloudseed.cpp:229-262`, `applyToggleTarget()` `src/cloudseed.cpp:266-281`
- Preset data: `presets.toml` (embedded via `src/presets_toml.s`, parsed by `src/preset_bank.cpp`)
- Preset application: `CloudSeed/ReverbController.h:47` (`LoadPreset`)
- Parameters: `CloudSeed/Parameter.h`; names table: `CloudSeed/ParameterNames.h`
- Hardware config: `Terrarium/terrarium.h`
- USB-MIDI preset upload: [docs/USB_MIDI.md](docs/USB_MIDI.md) (protocol, for host authors);
  normative definition [src/preset_protocol.h](src/preset_protocol.h); link layer
  [src/usb_midi_link.h](src/usb_midi_link.h)
- Hardware validation checklist: [docs/HARDWARE_TESTS.md](docs/HARDWARE_TESTS.md), driven by
  `tools/usb_preset_host.py`

### Key Concepts
- Buffer size: 48 samples
- Sample rate: 48kHz (typical)
- SDRAM pool: 48MB for the reverb (`custom_pool`), plus a separate 512 KB TOML parse arena used
  at boot and to validate USB uploads (peak measured 143,168 B)
- Delay lines: 5 (mono Terrarium), toggled per preset between `default_delay_lines` and
  `max_delay_lines` in presets.toml (default SWITCH_1, `[preset.toggle_map]`)
- Presets: 10, defined in presets.toml, `gPresets.count` at runtime (max `kMaxPresets` = 16)
- Persistent storage: preset index + bypass, QSPI flash, written 3 s after the last change,
  `SETTINGS_VERSION = 2` - preset order in presets.toml is frozen unless the version is bumped
- User presets: FS1 held 5 s saves the current sound into the current preset, FS1 + FS2 held
  5 s restores it to the active bank's values; stored in QSPI at offset 0x1000 and wiped
  whenever the active bank's text changes (a different firmware image, a USB upload, or a
  revert). presets.toml itself is never modified
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
- USB-MIDI preset upload: the pedal is a class-compliant USB-MIDI device at every boot; a host
  can upload, read back, or revert the active preset bank over SysEx (docs/USB_MIDI.md). An
  uploaded bank lives in QSPI and overrides presets.toml until it is reverted or a different
  firmware image is flashed; audio passes through dry for the duration of a session

### Quick Modifications
1. Control mapping → `[preset.knob_map]` / `[preset.toggle_map]` in `presets.toml` (verify with
   `./build/preset_check --print-knob-map presets.toml` /
   `./build/preset_check --print-toggle-map presets.toml`)
2. Knob feel → `KNOB_SMOOTHING_COEFF` (`src/cloudseed.cpp:50`), `kKnobMoveThreshold`,
   `kKnobApplyEpsilon`, `kKnobRailWindow`, `kKnobGlideBlocks` ([src/knob_bank.h](src/knob_bank.h))
3. Add/modify presets → `presets.toml` (`make` validates it; `make presets-check` validates only)
4. Blink patterns → `blinks` / `led_on_ms` / `led_off_ms` / `led_pause_ms` in `presets.toml`
5. Switch logic → reassign in `[preset.toggle_map]` (no C++ needed; see "1. Changing Control
   Mappings"), or in code: `src/cloudseed.cpp:266-281` (`applyToggleTarget()`),
   `src/cloudseed.cpp:382-390` (delay line count from `state.delayLinesMax`),
   `src/cloudseed.cpp:493-501` (`reverse.enabled` / `reverse.direct_mix` in the callback),
   `src/cloudseed.cpp:320-345` (footswitches),
   [src/footswitch_gestures.h](src/footswitch_gestures.h) (taps, FS2 hold, 5 s save / restore gestures)
6. LED2 blink behavior → `src/pedal_leds.cpp:67-113` (`ServicePresetBlink`); save/restore
   confirmation → `src/pedal_leds.cpp:116-139` (`CONFIRM_BLINKS`, `CONFIRM_BLINK_MS` at `:6-7`)
7. Knob map schema/validation → `parseKnobMap()` / `parseKnobTarget()` in
   [src/preset_bank.cpp](src/preset_bank.cpp)
8. Toggle map schema/validation → `parseToggleMap()` / `parseToggleTarget()` in
   [src/preset_bank.cpp](src/preset_bank.cpp); shared with knobs via `splitTarget()` /
   `resolveParamTarget()`
9. User preset store → `UserPresets` (`src/pedal_storage.h:31-50`), `PedalStorage::Init()`
   (`src/pedal_storage.cpp:41-56`), save/restore handling in the main loop (`src/cloudseed.cpp:654-668`)
