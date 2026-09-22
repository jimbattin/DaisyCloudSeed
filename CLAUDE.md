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
├── cloudseed.cpp          # Main application code (the only app source file)
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
- 10 factory presets (per-preset delay-line cap via `PresetConfig::max_delay_lines`)
- Early and late reverberation stages
- Extensive modulation and diffusion
- 48MB SDRAM buffer allocation
- Preset and bypass state persisted to QSPI flash
- Equal-power makeup gain on the wet path
- FPU flush-to-zero enabled at boot (denormal stall elimination)

### Control Mapping

File: [cloudseed.cpp](cloudseed.cpp)

```cpp
// Knob Parameter::Init() calls: cloudseed.cpp:550-555
// Parameter writes:            cloudseed.cpp:423-460
KNOB_1: Dry level               -> Parameter::DryOut                (0.0-1.0)
KNOB_2: Early reverb level      -> Parameter::EarlyOut              (0.0-1.0)
KNOB_3: Late reverb level       -> Parameter::MainOut               (0.0-1.0)
KNOB_4: Late diffusion feedback -> Parameter::LateDiffusionFeedback (0.0-1.0)
KNOB_5: Early reverb dampening  -> Parameter::TapDecay              (0.0-1.0)
KNOB_6: Late reverb decay       -> Parameter::LineDecay             (0.0-1.0)

SWITCH_1: Delay line count (off = 2 lines, on = the preset's max_delay_lines)
SWITCH_2: Unused (not referenced)
SWITCH_3: Reverse delay on/off (mixes a reversed copy of the reverb output; see CloudSeed/ReverseDelay.h)
SWITCH_4: Bloom -> Parameter::isReverse, reverses multitap gain order
FOOTSWITCH_1: Bypass toggle (persisted to flash)
FOOTSWITCH_2: Preset cycle (10 presets)
```

### Presets

Ten factory presets are configured in an array-based system in [cloudseed.cpp](cloudseed.cpp) (`cloudseed.cpp:79-141`):

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
(`max_delay_lines = 4.0f`, `cloudseed.cpp:133`) - it is CPU-intensive and crackles above that.

**Preset System Architecture**:
- Presets defined in `PRESETS[]` array with function pointers and LED blink patterns
- Cycles using modulo operator: `(currentPreset + 1) % NUM_PRESETS`
- Uses designated initializers for clarity
- Current preset and bypass state persist in QSPI flash memory
- `PresetConfig` carries a per-preset `max_delay_lines` cap applied in the audio callback
  (`cloudseed.cpp:473-475`)
- LED2 blinks continuously to indicate active preset (N blinks = preset N)

Preset definitions use initialization functions from [CloudSeed/ReverbController.h](CloudSeed/ReverbController.h) (`initFactoryChorus` at `CloudSeed/ReverbController.h:53` through `initFactoryDarkPlate` at `CloudSeed/ReverbController.h:579`)

### Preset Persistence

The current preset and the bypass state are both automatically saved to and loaded from QSPI flash memory, so they persist across power cycles.

**Implementation** ([cloudseed.cpp](cloudseed.cpp)):

**Settings Structure** (`cloudseed.cpp:146-160`):
```cpp
#define SETTINGS_VERSION 2   // cloudseed.cpp:40

struct Settings {
    int  version;        // SETTINGS_VERSION for compatibility checking
    int  currentPreset;  // Currently selected preset (0-9)
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
1. **On startup**: `loadSettings()` (`cloudseed.cpp:257-284`) restores preset and bypass from
   flash; called from `main()` at `:591`
2. **On preset change**: `saveSettings()` (`cloudseed.cpp:286-295`) updates the local copy and
   sets `state.triggerSettingsSave`
3. **On bypass toggle**: the audio callback sets `state.triggerBypassSave` (`:410`); the main
   loop turns that into a `saveSettings()` call (`:625-628`)
4. **In main loop**: the actual flash write (`SavedSettings.Save()`) happens outside the audio
   callback (`:631-634`)

**Resilience Features**:
- Invalid preset indices automatically default to preset 0 (Chorus) (`cloudseed.cpp:274-276`)
- Version mismatch triggers `RestoreDefaults()` and a reload (`cloudseed.cpp:263-268`)
- First boot / version mismatch defaults to preset 0 and `bypass = true` (`cloudseed.cpp:583-587`)
- LED1 is re-synced to the restored bypass state after load (`cloudseed.cpp:595-596`)
- Non-blocking: flash writes happen in the main loop, not the audio callback
- Settings survive power cycles, firmware updates, and manual resets

**To add more persistent settings**: add fields to the `Settings` struct, extend its `operator!=`, increment `SETTINGS_VERSION`, and update the `loadSettings()` and `saveSettings()` functions.

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
// cloudseed.cpp:211-226
#define CUSTOM_POOL_SIZE (48*1024*1024)  // 48MB
DSY_SDRAM_BSS char custom_pool[CUSTOM_POOL_SIZE];
size_t pool_index = 0;
int allocation_count = 0;

void* custom_pool_allocate(size_t size) {
    if (pool_index + size >= CUSTOM_POOL_SIZE) {
        return 0;  // Pool exhausted - the caller will fault rather than silently corrupt
    }
    void* ptr = &custom_pool[pool_index];
    pool_index += size;
    allocation_count++;
    return ptr;
}
```

This custom allocator manages SDRAM for delay lines.

Bump allocator, no free. Usage introspection: `get_pool_usage()` / `get_pool_remaining()`
(`cloudseed.cpp:229-235`). Callers placement-new into it (e.g.
`CloudSeed/ModulatedDelay.h:41-42`), so destructors of pool-backed objects must not call
`delete` - see [PERFORMANCE.md](PERFORMANCE.md) §6.

### Core Classes

**ReverbController** ([CloudSeed/ReverbController.h](CloudSeed/ReverbController.h)):
- Main reverb controller
- Preset management
- Single channel: `channelR` and all right-channel buffers are commented out
  (`CloudSeed/ReverbController.h:27`, `:37`, `:647`, `:748`, `:756`, `:773-775`)
- Fixed internal block size `static const int bufferSize = 48` (`CloudSeed/ReverbController.h:23`),
  backing fixed-size member arrays (`:28-30`)
- Public API: `SetParameter(Parameter, float)` `:742`, `ClearBuffers()` `:753`,
  `Process(float* input, float* output, int bufferSize)` `:759`

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
# Makefile:32-35 runs `clean all` in each, so this is a full rebuild of all three.
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
**Float flags**: `-ffast-math` on both the app (`Makefile:30`) and the library
(`CloudSeed/Makefile:118`); the library additionally uses `-fno-exceptions`,
`-finline-functions`, and `-fno-aggressive-loop-optimizations` (`CloudSeed/Makefile:116-120`)
**App type**: `BOOT_SRAM` (`Makefile:6`)

## Common Modifications

### 1. Changing Control Mappings

**File**: [cloudseed.cpp](cloudseed.cpp)

**Example**: Swap KNOB_1 and KNOB_2 in CloudSeed

```cpp
// Knob Parameter::Init() calls: cloudseed.cpp:550-555
state.dryOut.Init(hw.knob[Terrarium::KNOB_1], 0.0f, 1.0f, ::daisy::Parameter::LINEAR);
state.earlyOut.Init(hw.knob[Terrarium::KNOB_2], 0.0f, 1.0f, ::daisy::Parameter::LINEAR);

// Change to:
state.dryOut.Init(hw.knob[Terrarium::KNOB_2], 0.0f, 1.0f, ::daisy::Parameter::LINEAR);
state.earlyOut.Init(hw.knob[Terrarium::KNOB_1], 0.0f, 1.0f, ::daisy::Parameter::LINEAR);
```

Parameters are members of `PedalState` (`cloudseed.cpp:163-171`), not globals, and both the
`Terrarium::` and `::daisy::` qualifications are required as used in the file.

**Example**: Add low-pass filter control to CloudSeed

```cpp
// 1. Add the parameter and its change-detection field to PedalState (cloudseed.cpp:163-178):
::daisy::Parameter lpFilter;
float prevLpFilter;

// 2. Initialize it next to the other knobs in main() (cloudseed.cpp:550-555):
state.lpFilter.Init(hw.knob[Terrarium::KNOB_6], 0.0f, 1.0f, ::daisy::Parameter::LINEAR);

// 3. Use it in audioCallback(), following the hasChanged() guard pattern used by every
//    other parameter (cloudseed.cpp:432-460):
const float lpValue = state.lpFilter.Process();
if (hasChanged(state.prevLpFilter, lpValue)) {
    reverb->SetParameter(::Parameter::LowPassEnabled, 1.0f);
    reverb->SetParameter(::Parameter::LowPass, lpValue);
    state.prevLpFilter = lpValue;
}
```

`reverb` is a pointer (`cloudseed.cpp:198`), so parameter writes use `reverb->`. The filter
parameters are `LowPass`/`HighPass` (cutoff values scaled into `SetCutoffHz`,
`CloudSeed/ReverbChannel.h:163-168`) plus the `LowPassEnabled`/`HiPassEnabled` switches
(`CloudSeed/Parameter.h:11-12`, `:75-76`).

### 2. Modifying Parameter Ranges

CloudSeed uses `::daisy::Parameter::Init()` with min/max values. Every knob currently uses the
full `0.0f-1.0f` range (`cloudseed.cpp:550-555`):

```cpp
// Current KNOB_4 (drives Parameter::LateDiffusionFeedback), cloudseed.cpp:553:
state.diffusion.Init(hw.knob[Terrarium::KNOB_4], 0.0f, 1.0f, ::daisy::Parameter::LINEAR);

// To restrict it to the upper half of the range:
state.diffusion.Init(hw.knob[Terrarium::KNOB_4], 0.5f, 1.0f, ::daisy::Parameter::LINEAR);
```

### 3. Adding New Presets

**File**: [cloudseed.cpp](cloudseed.cpp)

The preset system uses an array-based configuration (`cloudseed.cpp:79-141`). To add a new preset:

1. Add a new entry to the `PRESETS[]` array using designated initializers:

```cpp
{
    .initFunction = &CloudSeed::ReverbController::initFactoryYourNewPreset,
    .blinkPattern = {.numBlinks = 11, .onDurationMs = 150, .offDurationMs = 150, .pauseAfterMs = 5000},
    .name = "Your New Preset",
    .max_delay_lines = 5.0f
}
```

2. Define the initialization function in [CloudSeed/ReverbController.h](CloudSeed/ReverbController.h)

**No other code changes needed** - `NUM_PRESETS` is computed automatically from the array size, and the modulo cycling logic adapts automatically.

**Blink Pattern Customization**:
Each preset can have a unique blink pattern. Modify the `.blinkPattern` fields:
- `.numBlinks`: How many times to blink
- `.onDurationMs`: LED on duration (milliseconds)
- `.offDurationMs`: LED off duration between blinks (milliseconds)
- `.pauseAfterMs`: Pause before repeating the sequence (milliseconds)

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

**File**: [cloudseed.cpp](cloudseed.cpp) (`cloudseed.cpp:462-487`)

Current logic: Switches 1-3 add delay lines (2 + number of switches on)

```cpp
// Example: Make switches select exact count instead of additive
int lineCount = 2; // Default
if (hw.switches[Terrarium::SWITCH_3].Pressed()) lineCount = 5;
else if (hw.switches[Terrarium::SWITCH_2].Pressed()) lineCount = 4;
else if (hw.switches[Terrarium::SWITCH_1].Pressed()) lineCount = 3;
```

The code uses `.Pressed()` — an 'ON' toggle counts as pressed (`cloudseed.cpp:467`) — not
`.Read()`. Any replacement must still apply the per-preset clamp at `cloudseed.cpp:473-475`,
or CPU-intensive presets will crackle.

Switch 4 controls a "Bloom" effect which reverses the gain decay on multi-tap delays

### 6. Adjusting Audio Buffer Size

**File**: [cloudseed.cpp](cloudseed.cpp) (`cloudseed.cpp:599-600`)

```cpp
// Current: 48 samples per block
hw.StartAdc();
hw.StartAudio(audioCallback);
```

Three sizes are coupled and must change together:
- `DaisyPetal::Init()` sets the hardware block size to 48 (`libdaisy/src/daisy_petal.cpp:90`);
  override it with `hw.SetAudioBlockSize(n)` before `StartAudio()`
- `AUDIO_BUFFER_SIZE` (`cloudseed.cpp:24`) sizes the static in/out buffers the callback writes
- `ReverbController::bufferSize` (`CloudSeed/ReverbController.h:23`) sizes the controller's
  fixed member arrays

Raising the hardware block size without raising the other two overruns those buffers.
Smaller blocks = lower latency, higher CPU load; larger blocks = higher latency, lower CPU load.

### 7. Adding CV Control

Terrarium has knob inputs that can accept CV (0-3.3V or 0-5V with voltage divider):

```cpp
// Read CV directly (no Parameter smoothing):
float cv_value = hw.knob[Terrarium::KNOB_1].Process();
// cv_value is 0.0-1.0 regardless of input voltage
```

### 8. LED2 Preset Indicator System

**Current Implementation**: LED2 uses a state machine to blink the preset number continuously (runs in main loop).

The system is defined in [cloudseed.cpp](cloudseed.cpp) (structs at `:53-67`;
`getPresetBlinkPattern` :313-319; `startBlinkSequence` :322-330; `updateBlinkState` :333-382):

**Key Components**:
- `BlinkPattern` struct: Defines blink timing parameters
- `BlinkState` struct: Maintains blink state machine
- `updateBlinkState()`: Main loop function that manages LED transitions
- `getPresetBlinkPattern()`: Retrieves pattern for current preset
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
- [cloudseed.cpp](cloudseed.cpp) - CloudSeed control mapping and logic

**Advanced**:
- [CloudSeed/ReverbController.h](CloudSeed/ReverbController.h) - Presets and high-level reverb control
- [CloudSeed/Parameter.h](CloudSeed/Parameter.h) - All available reverb parameters
- [CloudSeed/ReverbChannel.h](CloudSeed/ReverbChannel.h) - Core reverb architecture

**Reference Only** (usually don't modify):
- [CloudSeed/DelayLine.h](CloudSeed/DelayLine.h) - Delay buffer implementation
- [CloudSeed/AudioLib/](CloudSeed/AudioLib/) - Audio utilities
- Submodules (libdaisy, DaisySP, Terrarium)

### Understanding Audio Flow

**CloudSeed Audio Callback** (`audioCallback()` at `cloudseed.cpp:389-530` - runs at audio rate, ~48kHz):
1. Process analog/digital controls and update both LEDs (`:397-400`)
2. Footswitch edges: FOOTSWITCH_1 flips `state.bypass`, updates LED1, and sets
   `triggerBypassSave`; FOOTSWITCH_2 sets `triggerPresetChange` (`:407-416`)
3. `Process()` all six knob parameters, then write each to the reverb only when
   `hasChanged()` reports a difference larger than `FLOAT_EPSILON` (`:423-460`)
4. Select the delay line count from SWITCH_1 (off = 2, on = the preset's `max_delay_lines`)
   before writing `Parameter::LineCount`, then sample SWITCH_3 into `state.reverseDelayOn`
   (`:466-478`)
5. Apply Bloom → `Parameter::isReverse` when SWITCH_4 changes (`:480-484`)
6. Copy the left input channel into the static input buffer (`:496-498`)
7. Process audio: `reverb->Process(audioInputBuffer, audioOutputBuffer, AUDIO_BUFFER_SIZE)` (`:504`)
8. Apply makeup gain and write the output (`:506-524`). The dry/wet mix itself happens inside
   the reverb via `DryOut`/`EarlyOut`/`MainOut`; the callback only scales the result by
   `makeupGain = OUTPUT_VOLUME_BOOST * (1 + sinf(wetBalance * π/2) * MAKEUP_GAIN_STRENGTH)`,
   where `wetBalance = (earlyOut + mainOut) / (dryOut + earlyOut + mainOut + FLOAT_EPSILON)`
9. Run the reverse-delay stage on `audioOutputBuffer` and mix its reversed output into the
   active (non-bypass) signal, ramped in/out by a smoothed `state.reverseMix` toward
   `state.reverseDelayOn` so SWITCH_3 toggles click-free (`cloudseed.cpp` output loop;
   engine in `CloudSeed/ReverseDelay.h`)

When `state.bypass` is set, output is a straight `out[0][i] = in[0][i]` copy — but the reverb is
still processed, deliberately, to suppress an audible 1 kHz whine (`:502-503`). When
`state.presetChangeInProgress` is set, the reverb is skipped entirely and the input is passed
through (`:525-529`).

**CloudSeed Main Loop** (`main()` while loop at `cloudseed.cpp:602-643` - free-running, no sleep):
1. **Handle preset changes**: set `presetChangeInProgress`, then `cyclePreset()`,
   `saveSettings()`, `startBlinkSequence(getPresetBlinkPattern(...))`, `System::Delay(10)`,
   then clear the flag to re-enable audio processing (`:604-621`)
2. **Handle bypass persistence**: `triggerBypassSave` → `saveSettings()` (`:625-628`)
3. **Handle flash writes**: `triggerSettingsSave` → `SavedSettings.Save()` (`:631-634`)
4. **Update LED2 blink state**: `updateBlinkState()` (`:637`)
5. `dummy_trig_value = sinf(0.12345f)` — deliberate busy work written to a `volatile` global
   that keeps the STM32 out of a low-power state and reduces an audible 1 kHz whine
   (`:639-642`). There is no loop delay; the loop spins

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
  (`cloudseed.cpp:537`) — see [PERFORMANCE.md](PERFORMANCE.md) §1

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
// Terrarium LEDs are daisy::Led members of PedalState (cloudseed.cpp:191-192, init :576-580).
// DaisyPetal has no led1/led2 members - it exposes SetRingLed/SetFootswitchLed/ClearLeds
// for the Daisy Petal board's own I2C LED driver, which Terrarium does not use.
state.led2.Set(parameter_value);  // 0.0-1.0
state.led2.Update();
```

Caveat: `updateBlinkState()` (`cloudseed.cpp:333-382`) drives LED2 on every main-loop pass and
will overwrite debug values unless that call is removed. LED1 is likewise re-set on every
bypass toggle (`cloudseed.cpp:409`).

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
- Check Parameter smoothing settings
- Test with direct reads: `hw.knob[x].Process()`

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
- SDRAM: 51,742,752 B of 64MB (77.10%), entirely static `DSY_SDRAM_BSS`: `custom_pool`
  50,331,648 B + `CloudSeed::FastSin::data` 131,072 B + `AudioLib::ValueTables` tables
  1,280,032 B (see the `.sdram_bss` section in `build/cloudseed.map`)
- The runtime heap is not in this report: it grows from `end` in RAM_D2
  (`libdaisy/core/STM32H750IB_sram.lds:244-251`), which is where `DelayLine`'s `tempBuffer`,
  `mixedBuffer`, and `filterOutputBuffer` (`CloudSeed/DelayLine.h:44-46`) land
- SRAM (`.text`+`.data`, `BOOT_SRAM` region): 140,152 B of 480KB (28.51%)
- DTCMRAM: 15,276 B of 128KB (11.65%); RAM_D2_DMA: 16,968 B of 32KB (51.78%)

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
6. **Bypass state persisted to flash** alongside the preset (`SETTINGS_VERSION 2`, `cloudseed.cpp:146-160`)
7. **Persistent preset storage** in QSPI flash memory with version control
8. **LED2 blink pattern system** for visual preset indication
9. **Array-based preset configuration** with function pointers and designated initializers
10. **Performance optimization**: Moved preset switching and flash writes from audio callback to main loop
11. **Modulo-based preset cycling** for cleaner wraparound logic
12. **Through the Looking Glass Preset** enabled by adopting a `max_delay_lines` value for each preset
13. **Equal-power makeup gain** on the wet path, so perceived loudness holds as the dry/wet
    balance changes (`cloudseed.cpp:506-524`)
14. **1 kHz whine mitigation**: the reverb is still processed while bypassed
    (`cloudseed.cpp:502-503`) and the main loop performs a `volatile` `sinf()` write
    (`cloudseed.cpp:639-642`) to keep the STM32 out of a low-power state
15. **FPU flush-to-zero enabled at boot** to eliminate denormal stalls (`cloudseed.cpp:537`)
16. **`BOOT_SRAM` app type** (`Makefile:6`): the app is loaded into SRAM from QSPI flash by
    the Daisy bootloader, leaving room for all ten presets

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
make               # Build CloudSeed
make program-boot  # One time: flash the Daisy bootloader (BOOT_SRAM prerequisite)
make program-dfu   # Flash the app (reset, hold BOOT until rapid blink, then run)
```

### File Locations
- Control mapping: `cloudseed.cpp:550-555` (knob `Init()`), `cloudseed.cpp:423-460` (parameter writes)
- Preset array configuration: `cloudseed.cpp:79-141` (`PRESETS[]`)
- Preset initialization functions: `CloudSeed/ReverbController.h` (`initFactoryChorus` :53 through `initFactoryDarkPlate` :579)
- Parameters: `CloudSeed/Parameter.h`
- Hardware config: `Terrarium/terrarium.h`

### Key Concepts
- Buffer size: 48 samples
- Sample rate: 48kHz (typical)
- SDRAM pool: 48MB
- Delay lines: 5 (mono Terrarium), capped per preset by `PresetConfig::max_delay_lines`
- Presets: 10 factory presets (array-based, auto-counted)
- Persistent storage: preset + bypass, QSPI flash, `SETTINGS_VERSION 2`
- App type: `BOOT_SRAM` (app runs from SRAM, loaded by the Daisy bootloader)
- LED2: Continuous blink pattern indicates preset number

### Quick Modifications
1. Control mapping → `cloudseed.cpp:550-555` (knob `Init()` calls)
2. Parameter ranges → `Init()` calls at `cloudseed.cpp:550-555`
3. Add/modify presets → `cloudseed.cpp:79-141` (`PRESETS[]`)
4. Blink patterns → `cloudseed.cpp:79-141` (`.blinkPattern` fields)
5. Switch logic → `cloudseed.cpp:462-487` (delay line count, `max_delay_lines` clamp, Bloom)
6. LED2 blink behavior → `cloudseed.cpp:333-382` (`updateBlinkState`)
