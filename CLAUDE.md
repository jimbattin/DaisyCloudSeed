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
├── cloudseed.cpp          # Main application code (hardware, controls, audio callback)
├── knob_bank.h            # KnobBank absolute knob takeover (park, 50 ms glide, track; host-portable)
├── preset_footswitch.h    # PresetFootswitch FS2 tap/hold state machine (host-portable)
├── preset_bank.h          # PresetBank/PresetData types + knob-map types + ParsePresetBank()
├── preset_bank.cpp        # TOML -> PresetBank parser (host-portable, no libdaisy)
├── presets.toml           # THE preset data - all 10 presets, parsed at boot
├── presets_toml.s         # .incbin that embeds presets.toml into the firmware image
├── third_party/tomlc99/   # Vendored TOML parser (MIT, commit in README.txt)
├── tools/                 # preset_check.cpp (host-side presets.toml validator)
├── CLAUDE.md              # This file - agent-facing project documentation
├── README.md              # User-facing control table and build/flash instructions
├── PERFORMANCE.md         # Record of applied performance/correctness fixes (with file/line anchors)
├── license.txt            # License
├── pedal.png              # Pedal/control artwork
├── .vscode/               # Editor configuration
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
- Up to 5 delay lines (user-selectable via switches, capped per preset by `max_delay_lines`)
- 10 presets defined in [presets.toml](presets.toml), embedded in the image and parsed at boot
- Early and late reverberation stages
- Extensive modulation and diffusion
- 48MB SDRAM buffer allocation
- Preset and bypass state persisted to QSPI flash
- Equal-power makeup gain on the wet path
- FPU flush-to-zero enabled at boot (denormal stall elimination)

### Control Mapping

File: [cloudseed.cpp](cloudseed.cpp) + `[preset.knob_map]` in [presets.toml](presets.toml)

Knobs are **not** hard-wired. Each preset's `[preset.knob_map]` names a primary (`_a`) and a
secondary (`_b`) target per knob; the secondary bank is selected while **the preset footswitch
(FOOTSWITCH_2) is held**. The shipped default map in every preset reproduces the historical assignment:

```cpp
// Knob read: cloudseed.cpp:588-592; scan + dispatch: cloudseed.cpp:479-488;
// applyKnobTarget(): cloudseed.cpp:315-351
//                        primary (_a)                      secondary (_b)
KNOB_1: output.DryOut                        | input.PreDelay
KNOB_2: output.EarlyOut                      | input.HighPass   (+ HiPassEnabled on first touch)
KNOB_3: output.MainOut                       | input.LowPass    (+ LowPassEnabled on first touch)
KNOB_4: late_diffusion.LateDiffusionFeedback | late.LineModAmount
KNOB_5: early.TapDecay                       | late.LineModRate
KNOB_6: late.LineDecay                       | reverse.delay    (20-2000 ms reverse window)

SWITCH_1: Delay line count (off = 2 lines, on = the preset's max_delay_lines)
SWITCH_2: Bloom -> Parameter::isReverse, reverses multitap gain order
SWITCH_3: Reverse delay on/off (enables the reverse voice; SWITCH_4 selects its destination,
          the knob mapped to "reverse.delay" sets its window length; see CloudSeed/ReverseDelay.h)
SWITCH_4: Reverse routing (off = reverse into reverb wet path, on = direct output mix; only active with SWITCH_3 on)
FOOTSWITCH_1: Bypass toggle on RELEASE (persisted to flash 3 s later); independent of FOOTSWITCH_2
FOOTSWITCH_2 tap: Preset cycle on RELEASE
FOOTSWITCH_2 held: secondary knob bank; the release cycles the preset only if no secondary
          knob wrote a value during the hold (PresetFootswitch, preset_footswitch.h)
```

**Preset footswitch** ([preset_footswitch.h](preset_footswitch.h)): libdaisy's `Switch` is an
8-bit shift register clocked once per audio block: `Pressed()` is `state_ == 0xff` and clears
1 ms after a release, while `FallingEdge()` is `state_ == 0x80` and only fires 7 ms later
(`libdaisy/src/hid/switch.h:70-79`, `switch.cpp:44-47`). `PresetFootswitch` is a class whose
private `held` flag (read through `Held()`) is set on `Pressed()` and cleared only on
`FallingEdge()`, so a contact bounce mid-hold (which never produces `0x80`) cannot drop the
secondary bank, and the 6 ms tail before the edge stays in bank 1. The callback calls
`MarkEdited()` when a bank-1 `Scan()` returns at least one write (`cloudseed.cpp:492-493`),
i.e. a secondary knob moved past `kKnobMoveThreshold` and wrote; brushing a knob below the
threshold does not cancel the preset change, and entering bank 1 re-parks (zero writes), so
pressing FS2 is never itself an edit. `Update()` returns true on the release edge of a hold
with no `MarkEdited()` since the press and then clears both private flags; a falling edge
without a prior `Pressed()` is ignored. The bank is derived from `Held()` after `Update()` in
the same callback (`cloudseed.cpp:592`), so the block that ends a hold already scans in bank 0.
FOOTSWITCH_1 is a plain `FallingEdge()` bypass toggle; both footswitches are handled in
`processFootswitches()` (`cloudseed.cpp:452-469`), which reads each accessor once per callback
because edge flags are only valid for the update in which they occur.

**Knob take-over** ([knob_bank.h](knob_bank.h)): knobs are **absolute** - once a knob has taken
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
(`cloudseed.cpp:479-488`): it re-parks on a bank change (`bank != activeBank`), when
`state.knobResetPending` is set (by the main loop before `cyclePreset()`,
`cloudseed.cpp:710-716`), or on the first call; a re-parking call returns zero writes. It never
runs while `state.presetChangeInProgress` is set (the callback passes audio through and returns
first, `cloudseed.cpp:583-586`) because the re-park must not straddle a `state.knobMap` rewrite;
`knobResetPending` simply stays set until the load completes.
The glide start values come from `knobTargetValue()` (`cloudseed.cpp:356-376`), read for the
active bank every callback: knob values are stored verbatim in `parameters[]` by `SetParameter`,
so `GetAllParameters()` read-back is exact; the `reverse.delay` pseudo-target has no
`parameters[]` slot and is tracked in `state.reverseDelayNorm` instead. A disabled input filter
reports its open end instead of its stored cutoff (`HighPass` 0.0 while `HiPassEnabled` < 0.5,
`LowPass` 1.0 while `LowPassEnabled` < 0.5): the engine skips it, so that is what is heard, and
`applyKnobTarget()` enables it on the first write, so the glide opens from there rather than
stepping to the stored cutoff (about 2 kHz in Chorus and Dark Plate).

`main()` settles the knob one-poles for 200 ms between `hw.StartAdc()` and `hw.StartAudio()`
(`cloudseed.cpp:689-702`). `AnalogControl` starts at 0.0 and only converges while
`ProcessAnalogControls()` runs, which otherwise first happens inside the audio callback: without
the settle loop the first snapshot would capture ~5 % of each real knob position and the
settling ramp itself would be read as a deliberate turn at every power-up.

**ADC smoothing**: libdaisy's default `AnalogControl` slew computes to `coeff_ = 1.0` at this
callback rate (`libdaisy/src/hid/ctrl.cpp:16` with a 1 kHz update and the 0.002 s default),
i.e. no filtering. `main()` re-tunes every knob to `KNOB_SMOOTHING_COEFF` (0.05, ~20 ms) at
`cloudseed.cpp:669-670`. Nothing may call `hw.SetAudioBlockSize()` / `hw.SetAudioSampleRate()`
afterwards: both re-run `SetHidUpdateRates()` and overwrite the coefficient.

### Presets

All preset data lives in [presets.toml](presets.toml) at the repo root. The file is embedded
into the firmware image by `presets_toml.s` (`.incbin`, lands in `.rodata` → SRAM) and parsed
once at boot into the static `gPresets` bank (`cloudseed.cpp:90`). There is no filesystem and
no external storage: editing presets means editing the TOML and reflashing.

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
  `max_delay_lines`, a `[preset.knob_map]` table, and eight `[preset.params.*]` groups holding
  all 45 file-controlled parameters (see the parameter reference in the TOML header)
- `[preset.knob_map]` requires all twelve `knobN_a` / `knobN_b` keys. Each value is a quoted
  `"group.Parameter"` naming a parameter of that same group, or the pseudo-target
  `"reverse.delay"`. Parsed by `parseKnobMap()`/`parseKnobTarget()`
  ([preset_bank.cpp](preset_bank.cpp)) into `PresetData::knobMap[bank][knob]`
- `LineCount` and `isReverse` must NOT appear in the file: they are written at audio rate from
  SWITCH_1 and SWITCH_2. The parser rejects them
- Parsed by `ParsePresetBank()` ([preset_bank.cpp](preset_bank.cpp)) into `PresetBank`
  (`count` + `PresetData[16]`); `PresetData::params[]` is indexed by `(int)Parameter`
- Applied with `CloudSeed::ReverbController::LoadPreset()`
  (`CloudSeed/ReverbController.h:47`), which copies every slot except `LineCount`/`isReverse`
  and then re-applies all 47 through `SetParameter`
- Cycles using modulo operator: `(currentPreset + 1) % gPresets.count` (`cloudseed.cpp:266`)
- Preset index is persisted to QSPI flash, so **the order in presets.toml is frozen**;
  reordering or deleting entries requires bumping `SETTINGS_VERSION` (`cloudseed.cpp:52`)
- `loadPreset()` (`cloudseed.cpp:245-263`) is the only reader of `gPresets` after boot: besides
  calling `LoadPreset()`, it copies the preset's `knobMap`, `maxDelayLines` and blink timings
  into `PedalState` and sets `state.outputLevelsDirty`. The audio callback and
  `updateBlinkState()` read only those cached copies
- `max_delay_lines` is applied in the audio callback (`updateEngineControls()`) from
  `state.maxDelayLines` (`cloudseed.cpp:495-503`)
- LED2 blinks continuously to indicate active preset (N blinks = preset N)
- A parse failure is unrecoverable: `fatalErrorLoop()` (`cloudseed.cpp:171-180`, called at
  `:648`) blinks both LEDs at 5 Hz forever and never starts audio. The same loop reports an
  exhausted SDRAM pool and an unexpected audio block size. `make` validates presets.toml
  before embedding it, so a rejected file cannot be built into firmware in the first place

**Boot-time memory**: the parser allocates exclusively from a 512 KB bump arena carved from the
head of `custom_pool` (`cloudseed.cpp:212-225`), used between `hw.Init()` and
`new CloudSeed::ReverbController(...)`. Peak measured usage is 109,120 B on x86-64 (smaller on
32-bit ARM); the arena is abandoned - not freed - so the SDRAM pool starts at offset 0 for the
reverb. Permanent SDRAM cost of the TOML system: zero.

### Preset Persistence

The current preset and the bypass state are both automatically saved to and loaded from QSPI flash memory, so they persist across power cycles. Writes are coalesced: flash is written `SETTINGS_SAVE_DELAY_MS` (3000 ms) after the last change.

**Implementation** ([cloudseed.cpp](cloudseed.cpp)):

**Settings Structure** (`cloudseed.cpp:95-109`):
```cpp
constexpr int SETTINGS_VERSION = 2;                 // cloudseed.cpp:52
constexpr uint32_t SETTINGS_SAVE_DELAY_MS = 3000;  // cloudseed.cpp:56

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
1. **On startup**: `loadSettings()` (`cloudseed.cpp:274-284`) restores preset and bypass from
   flash; called from `main()` at `:681`
2. **On preset change / bypass toggle**: nothing is written yet. `cyclePreset()` updates
   `state.currentPreset` in the main loop; the audio callback flips `state.bypass` (`:457-460`)
3. **In main loop**: `serviceSettingsSave()` (`cloudseed.cpp:290-303`, called on every pass at
   `:722`) mirrors `state.currentPreset` / `state.bypass` into the RAM copy
   (`SavedSettings.GetSettings()`) and restarts a `SETTINGS_SAVE_DELAY_MS` timer whenever they
   differ. Once 3 s pass without another change it calls `SavedSettings.Save()`, outside the
   audio callback. A burst of preset/bypass changes therefore costs one QSPI sector erase, and
   `Save()` skips the erase entirely when flash already matches; a power-off within 3 s of a
   change loses that change (the pedal boots in the last saved state)

**Resilience Features**:
- Invalid preset indices automatically default to preset 0 (Chorus) (`cloudseed.cpp:280-281`);
  the corrected index is written back to flash 3 s after boot
- Version mismatch triggers `RestoreDefaults()` (`cloudseed.cpp:276-277`); the defaults are then
  read straight-line, with no reload
- First boot / version mismatch defaults to preset 0 and `bypass = true` (`cloudseed.cpp:673-678`)
- LED1 is re-synced to the restored bypass state after load (`cloudseed.cpp:685-686`)
- Non-blocking: flash writes happen in the main loop, not the audio callback
- Settings survive power cycles, firmware updates, and manual resets

**To add more persistent settings**: add the field to the `Settings` struct, extend its `operator!=`, increment `SETTINGS_VERSION`, and update `loadSettings()` (restore into `state`) and `serviceSettingsSave()` (mirror into the RAM copy and include it in the change test).

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
// cloudseed.cpp:186-206
static constexpr size_t alignUp8(size_t n) {
    return (n + 7u) & ~static_cast<size_t>(7u);
}

constexpr size_t CUSTOM_POOL_SIZE = 48u * 1024u * 1024u;  // 48MB
DSY_SDRAM_BSS __attribute__((aligned(32))) static char custom_pool[CUSTOM_POOL_SIZE];
DSY_SDRAM_BSS static float reverseDelayBuffer[REVERSE_BUFFER_SIZE];
static size_t pool_index = 0;

void* custom_pool_allocate(size_t size) {
    const size_t aligned = alignUp8(size);
    if (aligned > CUSTOM_POOL_SIZE - pool_index)
        fatalErrorLoop();  // callers placement-new into the result; 0x0 is ITCMRAM on the H750
    void* ptr = &custom_pool[pool_index];
    pool_index += aligned;
    return ptr;
}
```

This custom allocator manages SDRAM for delay lines.

Bump allocator, no free. The pool is aligned to the 32-byte M7 D-cache line and every block is
rounded up to 8 bytes. Exhaustion is a fatal boot error (`fatalErrorLoop()`, both LEDs at 5 Hz)
rather than a null return, because a write through 0x0 would land silently in ITCMRAM.
`custom_pool_allocate` keeps external linkage and this exact signature: the CloudSeed headers
declare it `extern`. Callers placement-new into it (e.g.
`CloudSeed/ModulatedDelay.h:41-42`), so destructors of pool-backed objects must not call
`delete` - see [PERFORMANCE.md](PERFORMANCE.md) §6.

The first 512 KB of this pool doubles as the boot-time TOML parse arena
(`toml_arena_alloc`, `cloudseed.cpp:219-225`). That arena is abandoned before
`new CloudSeed::ReverbController(...)` runs, so `pool_index` is still 0 when the reverb starts
allocating - the two uses never overlap in time.

### Core Classes

**ReverbController** ([CloudSeed/ReverbController.h](CloudSeed/ReverbController.h)):
- Main reverb controller
- Preset application: `LoadPreset(const float* values)` (`:47`) copies a parsed
  `PresetData::params[]` into `parameters[]`, skipping `LineCount`/`isReverse` (written at audio
  rate by SWITCH_1/SWITCH_2), then re-applies all 47 slots through `SetParameter`. The
  constructor no longer loads any preset - `main()` parses presets.toml and calls `LoadPreset()`
  before audio starts
- Single channel: `channelR` and all right-channel buffers are commented out
  (`CloudSeed/ReverbController.h:27`, `:37`, `:73`, `:174`, `:182`, `:199-201`)
- Fixed internal block size `static const int bufferSize = 48` (`CloudSeed/ReverbController.h:23`),
  backing fixed-size member arrays (`:28-30`)
- Public API: `LoadPreset(const float*)` `:47`, `SetParameter(Parameter, float)` `:168`,
  `ClearBuffers()` `:179`, `Process(float* input, float* output, int bufferSize)` `:185`
- Parameter scaling (the normalized 0.0-1.0 → real-unit mapping documented in presets.toml):
  `GetScaledParameter` `:86`

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
# Makefile:56-59 runs `clean all` in each, so this is a full rebuild of all three.
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

### Checking presets

```bash
# Validity check with the firmware's own parser, without building firmware.
# The same check runs automatically as part of `make`.
make presets-check
```

It builds `build/preset_check` from `tools/preset_check.cpp`, `preset_bank.cpp`, and the
vendored tomlc99 (compiled as C) using `HOSTCC`/`HOSTCXX` (default `gcc`/`g++`), then runs
`preset_check --validate presets.toml`. Unlike the stamp-gated check inside `make`, it re-runs
every time. `preset_check` requires exactly one mode: `--validate` (one-line summary; exit 1
with the parser's message on stderr if the file is rejected) or `--print-knob-map`; anything
else prints usage and exits 2.

`make` cannot produce firmware from a presets.toml the parser would reject: the embedded blob
(`$(BUILD_DIR)/presets_toml.o`) depends on `$(BUILD_DIR)/presets.valid`, whose recipe is
`preset_check --validate presets.toml` (`Makefile:39-68`). Validation covers TOML syntax,
missing/unknown/misplaced parameters, unknown preset- and root-level keys, out-of-range
scalars (a parameter value that is non-finite or outside 0..1, a `max_delay_lines` that is not a
whole number 1..5, `blinks`, the ms fields), and a document too large for the boot parse arena - the host tool allocates through a
replica of `TOML_ARENA_SIZE` (512 KB, 8-byte aligned, no reuse), so `presets.toml: 10 presets
valid, boot arena peak 109120 of 524288 bytes` is the same peak the pedal sees. Host pointers
are 64-bit, so the reported peak over-estimates the 32-bit target: a pass here implies a fit on
hardware. A near-miss should be fixed by raising `TOML_ARENA_SIZE` (`cloudseed.cpp:216`), not
by loosening the host check.

Preset values are free to change: `make presets-check` fails only on input the parser rejects.
There is no golden-value comparison; the original factory values are recoverable from git
history.

A file that somehow reaches the pedal broken is unrecoverable at runtime: `fatalErrorLoop()`
(`cloudseed.cpp:171-180`, called at `:648`) blinks both LEDs at 5 Hz forever and never starts
audio.

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
**CPU**: Cortex-M7 (`-mcpu=cortex-m7`, `CloudSeed/Makefile:78`)
**Optimization**: `-O3` (`Makefile:15`, `CloudSeed/Makefile:23`)
**FPU**: Hard float (`-mfpu=fpv5-d16 -mfloat-abi=hard`, `CloudSeed/Makefile:81-84`)
**Language**: C++14 (`-std=gnu++14`, `CloudSeed/Makefile:75`)
**Float flags**: `-ffast-math` on both the app (`Makefile:34`) and the library
(`CloudSeed/Makefile:118`); the library additionally uses `-fno-exceptions`,
`-finline-functions`, and `-fno-aggressive-loop-optimizations` (`CloudSeed/Makefile:116-120`)
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

`applyKnobTarget()` (`cloudseed.cpp:315-351`) turns `LowPassEnabled` on the first time that
knob is moved, so the filter is audible even in presets that ship with it off; the takeover
glide starts from the open filter (`knobTargetValue()`), so enabling it does not step the tone.
`HighPass` gets the same treatment via `HiPassEnabled`; no other gated parameter does - for the
shelves, the in-loop cutoff, and the diffusers, set the matching `*Enabled` value in
`[preset.params.*]`.

`applyKnobTarget()` is also where the output-level cache is invalidated: its `switch` sets
`state.outputLevelsDirty` for `DryOut`/`EarlyOut`/`MainOut`. Any new code path that writes one
of those three parameters outside `loadPreset()` must set that flag, or the makeup gain and the
dry-cancellation scale will stay at their old values.

**Adding a non-parameter target** (like `reverse.delay`) requires C++: a new
`KnobTargetKind` in [preset_bank.h](preset_bank.h), a branch in `parseKnobTarget()`
([preset_bank.cpp](preset_bank.cpp)), and a branch in `applyKnobTarget()`
(`cloudseed.cpp:315-351`).

### 2. Modifying Parameter Ranges

Knobs are read raw: `hw.knob[kKnobIndex[i]].Value()` (`cloudseed.cpp:589-590`) yields 0.0-1.0
and is handed straight to `SetParameter`, which applies the engine's own scaling
(`ReverbController::GetScaledParameter`). There is no per-knob min/max any more - the six
`::daisy::Parameter` wrappers were removed when knob targets became data.

To restrict a knob's travel, scale in `applyKnobTarget()` (`cloudseed.cpp:315-351`) before the
`SetParameter` call, e.g. `value = 0.5f + 0.5f * value;` for the upper half of the range. Note
that this affects every preset that maps a knob to that parameter.

The knob response constants: `KNOB_SMOOTHING_COEFF` (`cloudseed.cpp:48`, the ADC one-pole), and in
[knob_bank.h](knob_bank.h) `kKnobMoveThreshold` (how far a parked knob must move to take over),
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

# Input stage: pre-delay and the input filters feeding the whole reverb.
[preset.params.input]
InputMix       = 0.0
PreDelay       = 0.07
HiPassEnabled  = 0.0
HighPass       = 0.0
LowPassEnabled = 0.0
LowPass        = 0.29

# ... the remaining seven groups, every key required
```

Rules the parser enforces (a violation fails `make` before the blob is embedded; if one were
ever flashed it would stop boot and blink both LEDs):
- All eight `[preset.params.*]` groups must be present, each containing exactly its own keys -
  45 parameters total. Group membership is defined by `kGroups` in
  [preset_bank.cpp](preset_bank.cpp) and mirrored by the reference comment at the top of
  presets.toml
- `LineCount` and `isReverse` must not appear (SWITCH_1 and SWITCH_2 own them)
- `[preset.knob_map]` must be present with all twelve `knobN_a` / `knobN_b` keys. Each value
  is a quoted `"group.Parameter"` whose parameter really belongs to that group, or
  `"reverse.delay"`. Unknown knob keys, unknown groups/parameters, cross-group targets, and
  runtime parameters are all rejected
- `blinks` is 1..20; `max_delay_lines` is a whole number 1..5 (a fraction or `nan` fails with
  `preset N: max_delay_lines must be a whole number 1..5`); the ms fields are 0..60000 and
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
(`cloudseed.cpp:52`) in the same change so stale flash contents are discarded.

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

**File**: [cloudseed.cpp](cloudseed.cpp) (`cloudseed.cpp:58-64` for the named switch constants
`LINE_COUNT_SWITCH`, `BLOOM_SWITCH`, `REVERSE_ON_SWITCH`, `REVERSE_ROUTE_SWITCH`,
`BYPASS_FOOTSWITCH`, `PRESET_FOOTSWITCH`; `cloudseed.cpp:495-510` for SWITCH_1/SWITCH_2 in
`updateEngineControls()`; `cloudseed.cpp:595-603` for SWITCH_3/SWITCH_4 in the callback)

Current logic: SWITCH_1 selects the delay-line count (off = 2, on = the preset's
`max_delay_lines`); SWITCH_2 is Bloom; SWITCH_3 toggles the reverse voice; SWITCH_4 selects
reverse routing (off = into the reverb, on = direct mix). The reverse window length is no
longer a SWITCH_3 overload of KNOB_4 - it is whichever knob maps to `"reverse.delay"`.

```cpp
// Example: Make switches select exact count instead of additive
int lineCount = 2; // Default
if (hw.switches[Terrarium::SWITCH_3].Pressed()) lineCount = 5;
else if (hw.switches[Terrarium::SWITCH_2].Pressed()) lineCount = 4;
else if (hw.switches[Terrarium::SWITCH_1].Pressed()) lineCount = 3;
```

The code uses `.Pressed()` — an 'ON' toggle counts as pressed (`cloudseed.cpp:497`) — not
`.Read()`. Any replacement must still apply the per-preset max at `cloudseed.cpp:495-503`,
or CPU-intensive presets will crackle.

Switch 2 controls a "Bloom" effect which reverses the gain decay on multi-tap delays

### 6. Adjusting Audio Buffer Size

**File**: [cloudseed.cpp](cloudseed.cpp) (`cloudseed.cpp:689-702`)

```cpp
// Current: 48 samples per block
hw.StartAdc();
// ... 200 ms knob one-pole settle loop (see "Knob take-over" above) ...
hw.StartAudio(audioCallback);
```

Three sizes are coupled and must change together:
- `DaisyPetal::Init()` sets the hardware block size to 48 (`libdaisy/src/daisy_petal.cpp:90`);
  override it with `hw.SetAudioBlockSize(n)` before `StartAudio()`
- `AUDIO_BUFFER_SIZE` (`cloudseed.cpp:26`) sizes the file-scope `gInputBuffer`, `gWetBuffer`,
  `gReverseBuffer` and `gReverbInputBuffer` (`cloudseed.cpp:447-450`) and every callback loop;
  `main()` stops in `fatalErrorLoop()` if `hw.AudioBlockSize()` differs (`cloudseed.cpp:640-641`)
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

The system is defined in [cloudseed.cpp](cloudseed.cpp) (structs at `:67-81`;
`startBlinkSequence` :383-390; `updateBlinkState` :393-439). The pattern itself is
`state.blinkPattern`, cached by `loadPreset()`; there is no per-call lookup function. Both
functions only `Set()` LED2; the audio callback's `Led::Update()` pushes it to the pin.

**Key Components**:
- `BlinkPattern` struct: Defines blink timing parameters
- `BlinkState` struct: Maintains blink state machine
- `updateBlinkState()`: Main loop function that manages LED transitions
- `state.blinkPattern`: the active preset's pattern, refreshed by `loadPreset()`
- `startBlinkSequence()`: Initiates a new blink sequence

**Behavior**:
- Blinks continuously when pedal is active (not bypassed)
- Turns off completely when bypassed
- Blinks N times where N = preset number (1-10)
- 150ms on, 150ms off per blink
- 5 second pause between sequences
- Automatically restarts sequence after pause

**To modify LED2 behavior for custom purposes**, you would need to modify or replace the `updateBlinkState()` function in the main loop. The current implementation is tightly integrated with preset indication.

## Code Navigation Tips

### Key Files for Modification

**Most Common**:
- [presets.toml](presets.toml) - all preset data **and the knob map** (parsed at boot;
  validated by `make`)
- [cloudseed.cpp](cloudseed.cpp) - hardware wiring, audio callback, knob dispatch
- [knob_bank.h](knob_bank.h) - absolute, parked knob take-over state machine (park, 50 ms glide, track)
- [preset_footswitch.h](preset_footswitch.h) - FS2 tap (next preset) / hold (secondary bank) state machine

**Advanced**:
- [preset_bank.cpp](preset_bank.cpp) - TOML schema, group membership, and validation errors
- [CloudSeed/ReverbController.h](CloudSeed/ReverbController.h) - `LoadPreset`, parameter scaling
- [CloudSeed/Parameter.h](CloudSeed/Parameter.h) - All available reverb parameters
- [CloudSeed/ReverbChannel.h](CloudSeed/ReverbChannel.h) - Core reverb architecture

**Reference Only** (usually don't modify):
- [CloudSeed/DelayLine.h](CloudSeed/DelayLine.h) - Delay buffer implementation
- [CloudSeed/AudioLib/](CloudSeed/AudioLib/) - Audio utilities
- Submodules (libdaisy, DaisySP, Terrarium)

### Understanding Audio Flow

**CloudSeed Audio Callback** (`audioCallback()` at `cloudseed.cpp:572-606` - runs once per
48-sample block, i.e. at 1 kHz):
1. Process analog/digital controls and push both LEDs with `Led::Update()` (`:575-578`). Once
   audio runs this is the only caller of `Update()`: it is a read-modify-write that would race
   with the main loop
2. `processFootswitches()` (`:452-469`): FOOTSWITCH_1's `FallingEdge()` flips `state.bypass`
   and sets LED1 (the main loop persists it later). FOOTSWITCH_2's `Pressed()` and
   `FallingEdge()` go to one `gPresetFs.Update()` call, which returns true (set
   `triggerPresetChange`) on the release of a press during which no secondary knob wrote
3. Read `state.bypass` once. While `state.presetChangeInProgress` is set the main loop is
   rewriting `state.knobMap` and every engine parameter, so the callback copies the input to
   the output and returns (`:582-586`): no knob scan, no reverb, `reverseMix` does not advance
4. Read all six knobs with `hw.knob[kKnobIndex[i]].Value()` (`:588-590`) and call
   `updateEngineControls(gPresetFs.Held() ? 1 : 0, knobPositions)` (`:592`, body `:475-511`),
   which holds every reverb write:
   - read every knob's current target value for the active bank with `knobTargetValue()`, then
     one `gKnobs.Scan(bank, state.knobResetPending, …)` call (`:479-486`) re-parks on any bank
     or preset transition (zero writes that block) and otherwise returns the knobs to write:
     glide steps for a knob taking over, then the knob's own position
   - dispatch each returned write through `applyKnobTarget()` using `state.knobMap[bank][i]` —
     the cached copy, never `gPresets` (`:487-488`); any write while in bank 1 calls
     `gPresetFs.MarkEdited()`, cancelling that hold's preset change (`:492-493`)
   - select the delay line count from SWITCH_1 (`LINE_COUNT_SWITCH`; off = 2, on =
     `state.maxDelayLines`, cached by `loadPreset()`) and write `Parameter::LineCount` when it
     differs from `state.prevNumDelayLines` (`:495-503`). Both candidates are exact copies, so
     the `!=` test is exact; a preset with a different `max_delay_lines` is re-applied on the
     first callback after the change
   - apply Bloom (SWITCH_2, `BLOOM_SWITCH`) → `Parameter::isReverse` when it differs from
     `state.prevBloom` (`:505-510`)
5. `refreshOutputLevels()` (`:593`, body `:515-530`): `makeupGain` and `scaledDryOut` are
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
6. Sample SWITCH_3 (`REVERSE_ON_SWITCH`) into `reverseTarget` (0 or 1) and copy the left input
   channel into `gInputBuffer` (`:597-598`)
7. SWITCH_4 (`REVERSE_ROUTE_SWITCH`) picks the render helper (`:600-603`), each of which writes
   the output scaled by `makeupGain`:
   - Into-reverb, SWITCH_4 off, `renderReverseIntoReverb()` (`:537-554`): reverse the dry input
     into `gReverseBuffer`, scale it to the injected reverse, add that to the reverb input
     (`gReverbInputBuffer`), run `reverb->Process(gReverbInputBuffer, gWetBuffer, …)`, then
     `out = (wet − scaledDryOut * injectedReverse) * makeupGain`; subtracting
     `scaledDryOut * injectedReverse` cancels the reverb's dry pass-through of the injected
     reverse, so the forward dry stays clean and the reverse is heard only through the wet tail
   - Direct-mix, SWITCH_4 on, `renderReverseDirect()` (`:558-569`):
     `reverb->Process(gInputBuffer, gWetBuffer, …)`, record the reverb output into
     `reverseDelay` for backward playback, then
     `out = (wet + reversed * REVERSE_LEVEL * mix) * makeupGain`, the reverse audible on its own
   Both ramp the reverse via a smoothed mix toward `reverseTarget`, kept in a local during the
   sample loop and stored back to `state.reverseMix` afterwards, so SWITCH_3 toggles click-free
   (engine in `CloudSeed/ReverseDelay.h`)
8. If bypassed, overwrite the output with the input (`:604-605`)

When `state.bypass` is set, output is a straight copy of the input — but the reverb (and the
reverse mix) is still processed, deliberately, to suppress an audible 1 kHz whine (`:599`). When
`state.presetChangeInProgress` is set, the reverb is skipped entirely and the input is passed
through (`:583-586`).

**CloudSeed Main Loop** (`main()` while loop at `cloudseed.cpp:704-728` - free-running, no sleep):
1. **Handle preset changes** (`:709-720`): clear `triggerPresetChange`, set
   `presetChangeInProgress` and `state.knobResetPending` (so the knob positions are
   re-snapshotted before the new preset loads), then between two
   `std::atomic_signal_fence(std::memory_order_seq_cst)` compiler barriers run `cyclePreset()`
   and `startBlinkSequence(state.blinkPattern)`, then clear the flag to re-enable audio
   processing. `loadPreset()` is synchronous, so there is no delay: the gate reopens as soon as
   it returns. The four flags shared with the callback (`bypass`, `triggerPresetChange`,
   `presetChangeInProgress`, `knobResetPending`) are `volatile bool` (`:118-121`)
2. **Persist settings**: `serviceSettingsSave()` (`:722`) - the coalesced flash write described
   under Preset Persistence
3. **Update LED2 blink state**: `updateBlinkState()` (`:725`)
4. `keepCoreBusy()` (`:727`, defined at `:616-625`) — deliberate busy work that keeps the core
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
  - Flash memory writes
  - LED blink state machine
- **FPU flush-to-zero** is enabled once at the top of `main()` before `hw.Init()`
  (`cloudseed.cpp:628`) — see [PERFORMANCE.md](PERFORMANCE.md) §1

This separation prevents audio glitches during preset changes and flash writes.

## Debugging

### Serial Debug Output

```cpp
// In cloudseed.cpp, add:
#include "daisy_seed.h"
using namespace daisy;

// In setup:
hw.seed.StartLog(true);

// Anywhere:
hw.seed.PrintLine("Debug: value = %f", some_value);
```

### LED Indicators

```cpp
// Terrarium LEDs are daisy::Led members of PedalState (cloudseed.cpp:147-148, init :633-637).
// DaisyPetal has no led1/led2 members - it exposes SetRingLed/SetFootswitchLed/ClearLeds
// for the Daisy Petal board's own I2C LED driver, which Terrarium does not use.
state.led2.Set(parameter_value);  // 0.0-1.0
```

`Set()` is enough once audio runs: the audio callback calls `Led::Update()` for both LEDs every
block (`cloudseed.cpp:577-578`), and nothing else may call it after `hw.StartAudio()` (it is a
read-modify-write that races with the callback). Only before `StartAudio()` must you call
`state.led2.Update()` yourself.

Caveat: `updateBlinkState()` (`cloudseed.cpp:393-439`) drives LED2 on every main-loop pass and
will overwrite debug values unless that call is removed. LED1 is likewise re-set on every
bypass toggle (`cloudseed.cpp:459`).

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
- Check the knob smoothing coefficient (`KNOB_SMOOTHING_COEFF`, `cloudseed.cpp:48`) and the
  takeover constants in [knob_bank.h](knob_bank.h): a knob brushed by accident taking over its
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
  parameter-lookup fixes documented in [PERFORMANCE.md](PERFORMANCE.md), so real headroom is
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
- SRAM (`.text`+`.data`, `BOOT_SRAM` region): 187,236 B of 480KB (38.09%). Of that, the
  embedded `presets.toml` blob is 35,304 B (`build/presets_toml.o` - it now carries the
  per-preset `[preset.knob_map]` tables), tomlc99 is 14,371 B, and `preset_bank.o` is 5,836 B
- DTCMRAM: 20,476 B of 128KB (15.62%) — includes the 4,228 B `gPresets` bank (384 B of that is
  the knob maps, 24 B per preset slot); RAM_D2_DMA: 16,968 B of 32KB (51.78%)

### Optimization Tips

See [PERFORMANCE.md](PERFORMANCE.md) for the concrete list of performance/correctness
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
6. **Bypass state persisted to flash** alongside the preset (`SETTINGS_VERSION = 2`, `cloudseed.cpp:95-109`)
7. **Persistent preset storage** in QSPI flash memory with version control
8. **LED2 blink pattern system** for visual preset indication
9. **TOML-defined presets**: all preset data lives in [presets.toml](presets.toml), embedded in
   the firmware image with `.incbin` and parsed at boot by a vendored tomlc99 into a static
   `PresetBank`; `make` (and `make presets-check`, without building) rejects a file the
   parser would fail
10. **Performance optimization**: Moved preset switching and flash writes from audio callback to main loop;
    flash writes are coalesced 3 s after the last change (`serviceSettingsSave()`)
11. **Modulo-based preset cycling** for cleaner wraparound logic
12. **Through the Looking Glass Preset** enabled by adopting a `max_delay_lines` value for each preset
13. **Equal-power makeup gain** on the wet path, so perceived loudness holds as the dry/wet
    balance changes (`refreshOutputLevels()`, `cloudseed.cpp:515-530`, applied by the render
    helpers at `:537-569`), derived from `reverb->GetAllParameters()` only
    when `DryOut`/`EarlyOut`/`MainOut` change (`state.outputLevelsDirty`), not per block
14. **1 kHz whine mitigation**: the reverb is still processed while bypassed
    (`cloudseed.cpp:599-605`) and every main-loop pass calls `keepCoreBusy()`
    (`cloudseed.cpp:616-625`, `:727`): a real FPU `sinf()` of a `volatile` phase plus `volatile`
    loads and stores, so the core never idles between callbacks
15. **FPU flush-to-zero enabled at boot** to eliminate denormal stalls (`cloudseed.cpp:628`)
16. **`BOOT_SRAM` app type** (`Makefile:6`): the app is loaded into SRAM from QSPI flash by
    the Daisy bootloader, leaving room for all ten presets
17. **Per-preset knob mapping** (`[preset.knob_map]` in presets.toml): every knob has a primary
    and a secondary target, the secondary bank selected by holding the preset footswitch. Targets
    are `"group.Parameter"` strings plus the pseudo-target `"reverse.delay"` (the reverse window
    length, formerly a SWITCH_3 overload of KNOB_4). Knobs are **absolute but parked**
    ([knob_bank.h](knob_bank.h)): after power-up, a preset load, or a bank change a knob writes
    nothing until it is turned, then glides its target to the pot's position over 50 ms and
    tracks it 1:1; the stops land exactly on 0.0 / 1.0
18. **Preset footswitch tap/hold** (`processFootswitches()`, `cloudseed.cpp:452-469`): tapping
    FOOTSWITCH_2 cycles the preset on release; holding it selects the secondary knob bank, and its release cycles only
    if no secondary knob wrote during the hold. Tracked by `PresetFootswitch`
    ([preset_footswitch.h](preset_footswitch.h)), which holds the bank from `Pressed()` until
    `FallingEdge()` so a bounce cannot drop it. FOOTSWITCH_1 toggles bypass on release,
    independently of FOOTSWITCH_2
19. **Active preset configuration cached in `PedalState`** (`loadPreset()`,
    `cloudseed.cpp:245-263`): `knobMap`, `maxDelayLines` and the blink timings are copied out
    of `gPresets` on load, so the audio callback and the blink state machine never touch the
    parsed bank. While `state.presetChangeInProgress` is set the callback passes audio through
    and makes no engine write, so a load cannot be read half-applied

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
make               # Build CloudSeed
make program-boot  # One time: flash the Daisy bootloader (BOOT_SRAM prerequisite)
make program-dfu   # Flash the app (reset, hold BOOT until rapid blink, then run)
```

### File Locations
- Control mapping: `[preset.knob_map]` in `presets.toml`; dispatch at `cloudseed.cpp:487-488`
  and `applyKnobTarget()` `cloudseed.cpp:315-351`
- Preset data: `presets.toml` (embedded via `presets_toml.s`, parsed by `preset_bank.cpp`)
- Preset application: `CloudSeed/ReverbController.h:47` (`LoadPreset`)
- Parameters: `CloudSeed/Parameter.h`; names table: `CloudSeed/ParameterNames.h`
- Hardware config: `Terrarium/terrarium.h`

### Key Concepts
- Buffer size: 48 samples
- Sample rate: 48kHz (typical)
- SDRAM pool: 48MB (first 512 KB reused as the boot-only TOML parse arena; peak 109,120 B)
- Delay lines: 5 (mono Terrarium), capped per preset by `max_delay_lines` in presets.toml
- Presets: 10, defined in presets.toml, `gPresets.count` at runtime (max `kMaxPresets` = 16)
- Persistent storage: preset index + bypass, QSPI flash, written 3 s after the last change,
  `SETTINGS_VERSION = 2` - preset order in presets.toml is frozen unless the version is bumped
- App type: `BOOT_SRAM` (app runs from SRAM, loaded by the Daisy bootloader)
- LED2: Continuous blink pattern indicates preset number; both LEDs blinking together at 5 Hz
  with no audio is `fatalErrorLoop()`: the embedded TOML failed to parse, the SDRAM pool is
  exhausted, or the audio block size is not `AUDIO_BUFFER_SIZE` (48)
- Knob banks: 2 per preset (`knobN_a` primary, `knobN_b` while the preset footswitch is held);
  knobs are absolute but parked - nothing moves until a knob is turned after a preset or bank
  change, then the target glides to the knob's position over 50 ms (no step) and tracks it; the
  stops land exactly on 0.0 / 1.0

### Quick Modifications
1. Control mapping → `[preset.knob_map]` in `presets.toml` (verify with
   `./build/preset_check --print-knob-map presets.toml`)
2. Knob feel → `KNOB_SMOOTHING_COEFF` (`cloudseed.cpp:48`), `kKnobMoveThreshold`,
   `kKnobApplyEpsilon`, `kKnobRailWindow`, `kKnobGlideBlocks` ([knob_bank.h](knob_bank.h))
3. Add/modify presets → `presets.toml` (`make` validates it; `make presets-check` validates only)
4. Blink patterns → `blinks` / `led_on_ms` / `led_off_ms` / `led_pause_ms` in `presets.toml`
5. Switch logic → `cloudseed.cpp:58-64` (named switch constants), `cloudseed.cpp:595-603`
   (SWITCH_3 / SWITCH_4), `cloudseed.cpp:495-510` (delay line count, `max_delay_lines` clamp,
   Bloom), `cloudseed.cpp:452-469` (footswitches),
   [preset_footswitch.h](preset_footswitch.h) (FS2 tap/hold)
6. LED2 blink behavior → `cloudseed.cpp:393-439` (`updateBlinkState`)
7. Knob map schema/validation → `parseKnobMap()` / `parseKnobTarget()` in
   [preset_bank.cpp](preset_bank.cpp)
