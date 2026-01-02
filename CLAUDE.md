# DaisyCloudSeed Project Documentation

## Project Overview

This project implements two guitar effects DSPs for the Electrosmith Daisy Seed board mounted in a PedalPCB Terrarium guitar pedal enclosure:

1. **CloudSeed** - Advanced algorithmic reverb with 5 delay lines, 8 presets, and extensive control
2. **CloudyReverb** - Lightweight reverb based on Mutable Instruments Rings/Clouds algorithm

## Directory Structure

```
DaisyCloudSeed/
├── CloudSeed/              # Core reverb algorithm library (builds libcloudseed.a)
│   ├── AudioLib/          # Audio utilities (Biquad, filters, ShaRandom)
│   ├── Utils/             # SHA256 utilities
│   ├── build/             # Build artifacts
│   └── Makefile           # Library build configuration
│
├── petal/                  # Terrarium pedal implementations (MAIN WORKING AREA)
│   ├── CloudSeed/         # CloudSeed for Terrarium (mono, 5 delay lines)
│   │   ├── cloudseed.cpp  # Main application code
│   │   └── Makefile       # Build configuration
│   └── CloudyReverb/      # CloudyReverb for Terrarium
│       ├── cloudyreverb.cpp
│       └── Makefile
│
├── patch/                  # Daisy Patch implementations (stereo, reference)
│   ├── CloudSeed/
│   ├── CloudyReverb/
│   ├── Granular/
│   └── SamplePlayer/
│
├── libdaisy/              # Hardware abstraction layer (Git submodule)
├── DaisySP/               # DSP library (Git submodule)
├── eurorack/              # Mutable Instruments code (Git submodule)
├── Terrarium/             # Hardware definitions (Git submodule)
│
├── rebuild_libs.sh        # Build all libraries
├── rebuild_all.sh         # Build everything
└── .gitmodules           # Submodule definitions
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
- KNOB_1 through KNOB_6 (ADC channels 0,2,4,1,3,5)

**Switches** (4 toggles):
- SWITCH_1 through SWITCH_4 (GPIO pins 2,1,0,6)

**Footswitches** (2):
- FOOTSWITCH_1 (pin 4) - Bypass/Active
- FOOTSWITCH_2 (pin 5) - Preset cycling (CloudSeed only)

**LEDs** (2):
- LED_1 (pin 22) - Active indicator (on when not bypassed)
- LED_2 (pin 23) - Preset indicator (blinks N times for preset N, continuous with 5s pause)

**Audio**:
- Mono input/output (uses left channel only)

## CloudSeed Effect

### Architecture

CloudSeed is based on the open-source CloudSeed VST plugin by ValdemarOrn, modified for mono processing on embedded hardware.

**Key Features**:
- Up to 5 delay lines (user-selectable via switches)
- 8 factory presets
- Early and late reverberation stages
- Extensive modulation and diffusion
- 48MB SDRAM buffer allocation

### Control Mapping

File: [petal/CloudSeed/cloudseed.cpp](petal/CloudSeed/cloudseed.cpp)

```cpp
// Current mapping (lines 142-162):
KNOB_1: Dry Level (0.0-1.0)
KNOB_2: Early Reverberation Level (0.0-1.0)
KNOB_3: Late Reverberation Level (0.0-1.0)
KNOB_4: Late Reverberation Feedback (0.5-1.0)
KNOB_5: Early Reverberation Dampening/TapDecay (0.0-1.0)
KNOB_6: Late Reverberation Decay (0.0-1.0)

SWITCH_1-4: Delay line enable (additive, 1-5 total lines)
FOOTSWITCH_1: Bypass toggle
FOOTSWITCH_2: Preset cycle (8 presets)
```

### Presets

Eight factory presets are configured in an array-based system in [petal/CloudSeed/cloudseed.cpp](petal/CloudSeed/cloudseed.cpp) (lines 55-96):

1. Chorus (1 blink)
2. Dull Echos (2 blinks)
3. Hyperplane (3 blinks)
4. Medium Space (4 blinks)
5. Noise in the Hallway (5 blinks)
6. Rubi Ka Fields (6 blinks)
7. Small Room (7 blinks)
8. 90s Are Back (8 blinks)

**Preset System Architecture**:
- Presets defined in `PRESETS[]` array with function pointers and LED blink patterns
- Cycles using modulo operator: `(currentPreset + 1) % NUM_PRESETS`
- Uses designated initializers for clarity
- Current preset persists in QSPI flash memory
- LED2 blinks continuously to indicate active preset (N blinks = preset N)

Preset definitions use initialization functions from [CloudSeed/ReverbController.h](CloudSeed/ReverbController.h) (lines 26-84)

### Preset Persistence

The current preset is automatically saved to and loaded from QSPI flash memory, ensuring it persists across power cycles.

**Implementation** ([petal/CloudSeed/cloudseed.cpp](petal/CloudSeed/cloudseed.cpp)):

**Settings Structure** (lines 100-113):
```cpp
struct Settings {
    int version;         // SETTINGS_VERSION for compatibility checking
    int currentPreset;   // Currently selected preset (0-7)
    bool operator!=(const Settings& a) const;  // Required by PersistentStorage
};
```

**Storage Management**:
- Uses Daisy's `PersistentStorage<Settings>` class with QSPI flash
- Version control ensures compatibility when Settings struct changes
- Automatic defaults restoration if version mismatch detected
- Settings validated on load (invalid presets default to 0)

**Save/Load Workflow**:
1. **On startup**: `load_settings()` restores preset from flash (line 96-119)
2. **On preset change**: `save_settings()` triggers deferred write (line 121-130)
3. **In main loop**: Actual flash write happens outside audio callback (line 296-299)

**Resilience Features**:
- Invalid preset indices automatically default to preset 0 (Chorus)
- Version mismatch triggers clean restore to defaults
- Non-blocking: Flash writes happen in main loop, not audio callback
- Settings survive power cycles, firmware updates, and manual resets

**To add more persistent settings**: Add fields to `Settings` struct, increment `SETTINGS_VERSION`, and update `load_settings()` and `save_settings()` functions.

### Parameters

84+ reverb parameters enumerated in [CloudSeed/Parameter.h](CloudSeed/Parameter.h):

**Input Stage**: InputMix, PreDelay, HighPass, LowPass

**Early Reverb**: TapCount, TapLength, TapGain, TapDecay, DiffusionEnabled, DiffusionStages, DiffusionDelay, DiffusionFeedback

**Late Reverb**: LineCount, LineDelay, LineDecay, LateDiffusionEnabled, etc.

**Modulation**: Enabled, ModAmount, ModRate

**Output**: DryOut, PredelayOut, EarlyOut, MainOut

### Memory Architecture

CloudSeed requires massive delay buffers:

```cpp
// cloudseed.cpp:16-30
#define CUSTOM_POOL_SIZE (48*1024*1024)  // 48MB
DSY_SDRAM_BSS char custom_pool[CUSTOM_POOL_SIZE];

void* custom_pool_allocate(size_t size) {
    static size_t allocated = 0;
    void* ptr = &custom_pool[allocated];
    allocated += size;
    return ptr;
}
```

This custom allocator manages SDRAM for delay lines.

### Core Classes

**ReverbController** ([CloudSeed/ReverbController.h](CloudSeed/ReverbController.h)):
- Main reverb controller
- Preset management
- Dual-channel architecture (modified to mono for Terrarium)

**ReverbChannel** ([CloudSeed/ReverbChannel.h](CloudSeed/ReverbChannel.h)):
- `TotalLineCount = 5` for mono implementation
- Contains delay lines, diffusers, modulation

**DelayLine** ([CloudSeed/DelayLine.h](CloudSeed/DelayLine.h)):
- Core delay buffer implementation (5019 lines)
- Uses custom SDRAM allocator

**AllpassDiffuser, MultitapDiffuser**: Diffusion stages
**ModulatedAllpass, ModulatedDelay**: Modulated processing

## CloudyReverb Effect

### Architecture

Based on Mutable Instruments Rings/Clouds reverb algorithm using Griesinger topology.

**Key Features**:
- Lightweight processing
- 4 AP diffusers + loop of 2x(2AP+1Delay)
- Simple 5-knob control scheme
- No preset system
- Lower memory requirements

### Control Mapping

File: [petal/CloudyReverb/cloudyreverb.cpp](petal/CloudyReverb/cloudyreverb.cpp)

```cpp
// Current mapping (lines 63-97):
KNOB_1: Dry/Wet Mix (0.0-1.0)
KNOB_2: Input Level (0.0-1.0)
KNOB_3: Reverb Time/Feedback (0.0-1.0)
KNOB_4: Diffusion (0.0-1.0)
KNOB_5: Low Pass Filter (0.0-1.0)
KNOB_6: Unused

FOOTSWITCH_1: Bypass toggle
```

### Core Class

**FxEngine** from eurorack (Mutable Instruments):
- Lives in `eurorack/rings/dsp/fx/reverb.h`
- Methods: `set_amount()`, `set_input_gain()`, `set_time()`, `set_diffusion()`, `set_lp()`
- High-quality, battle-tested algorithm

## Build System

### Building Libraries

```bash
# Build all libraries (libdaisy, DaisySP, libcloudseed)
./rebuild_libs.sh

# Or build individually:
cd CloudSeed && make
cd libdaisy && make
cd DaisySP && make
```

### Building Effects

```bash
# CloudSeed
cd petal/CloudSeed
make

# CloudyReverb
cd petal/CloudyReverb
make
```

### Build Outputs

Located in `petal/{CloudSeed,CloudyReverb}/build/`:
- **{target}.bin** - Binary for DFU flashing via USB
- **{target}.elf** - ELF executable with debug symbols
- **{target}.hex** - Intel HEX format
- **{target}.map** - Linker map file

### Flashing to Hardware

```bash
# Put Daisy Seed in bootloader mode (hold BOOT button, press RESET)
make program-dfu
```

### Compiler Configuration

**Platform**: ARM GCC (`arm-none-eabi-gcc`)
**CPU**: Cortex-M7 (`-mcpu=cortex-m7`)
**Optimization**: `-O3`
**FPU**: Hard float (`-mfpu=fpv5-d16 -mfloat-abi=hard`)
**Language**: C++14 (`-std=gnu++14`)

## Common Modifications

### 1. Changing Control Mappings

**File**: [petal/CloudSeed/cloudseed.cpp](petal/CloudSeed/cloudseed.cpp) or [petal/CloudyReverb/cloudyreverb.cpp](petal/CloudyReverb/cloudyreverb.cpp)

**Example**: Swap KNOB_1 and KNOB_2 in CloudSeed

```cpp
// Find the parameter initialization (lines 142-147):
dryOut.Init(hw.knob[KNOB_1], 0.0f, 1.0f, Parameter::LINEAR);
predelayOut.Init(hw.knob[KNOB_2], 0.0f, 1.0f, Parameter::LINEAR);

// Change to:
dryOut.Init(hw.knob[KNOB_2], 0.0f, 1.0f, Parameter::LINEAR);
predelayOut.Init(hw.knob[KNOB_1], 0.0f, 1.0f, Parameter::LINEAR);
```

**Example**: Add low-pass filter control to CloudSeed

```cpp
// 1. Add parameter object (around line 140):
daisy::Parameter lpFilter;

// 2. Initialize in setup (around line 160):
lpFilter.Init(hw.knob[KNOB_6], 0.0f, 1.0f, Parameter::LINEAR);

// 3. Use in audio callback (around line 195):
float lp_value = lpFilter.Process();
reverb.SetParameter(Parameter::LowPassEnabled, 1.0f);
reverb.SetParameter(Parameter::LowPassFrequency, lp_value);
```

### 2. Modifying Parameter Ranges

CloudSeed uses `daisy::Parameter::Init()` with min/max values:

```cpp
// Current feedback range: 0.5-1.0
lateFeedback.Init(hw.knob[KNOB_4], 0.5f, 1.0f, Parameter::LINEAR);

// To allow full range 0.0-1.0:
lateFeedback.Init(hw.knob[KNOB_4], 0.0f, 1.0f, Parameter::LINEAR);
```

### 3. Adding New Presets

**File**: [petal/CloudSeed/cloudseed.cpp](petal/CloudSeed/cloudseed.cpp)

The preset system uses an array-based configuration (lines 55-96). To add a new preset:

1. Add a new entry to the `PRESETS[]` array using designated initializers:

```cpp
{
    .initFunction = &CloudSeed::ReverbController::initFactoryYourNewPreset,
    .blinkPattern = {.numBlinks = 9, .onDurationMs = 150, .offDurationMs = 150, .pauseAfterMs = 5000},
    .name = "Your New Preset"
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

**File**: [petal/CloudSeed/cloudseed.cpp](petal/CloudSeed/cloudseed.cpp) (lines 179-204)

Current logic: Switches add delay lines (1 + number of switches on)

```cpp
// Example: Make switches select exact count instead of additive
int lineCount = 1; // Default
if (hw.switches[SWITCH_4].Read()) lineCount = 5;
else if (hw.switches[SWITCH_3].Read()) lineCount = 4;
else if (hw.switches[SWITCH_2].Read()) lineCount = 3;
else if (hw.switches[SWITCH_1].Read()) lineCount = 2;
```

### 6. Adjusting Audio Buffer Size

**File**: [petal/CloudSeed/cloudseed.cpp](petal/CloudSeed/cloudseed.cpp) (line 234)

```cpp
// Current: 48 samples
hw.StartAdc();
hw.StartAudio(AudioCallback);

// Buffer size is set in libdaisy hardware configuration
// Smaller buffers = lower latency, higher CPU load
// Larger buffers = higher latency, lower CPU load
```

### 7. Adding CV Control

Terrarium has knob inputs that can accept CV (0-3.3V or 0-5V with voltage divider):

```cpp
// Read CV directly (no Parameter smoothing):
float cv_value = hw.knob[KNOB_1].Process();
// cv_value is 0.0-1.0 regardless of input voltage
```

### 8. LED2 Preset Indicator System

**Current Implementation**: LED2 uses a state machine to blink the preset number continuously (runs in main loop).

The system is defined in [petal/CloudSeed/cloudseed.cpp](petal/CloudSeed/cloudseed.cpp) (lines 148-229):

**Key Components**:
- `BlinkPattern` struct: Defines blink timing parameters
- `BlinkState` struct: Maintains blink state machine
- `updateBlinkState()`: Main loop function that manages LED transitions
- `getPresetBlinkPattern()`: Retrieves pattern for current preset
- `startBlinkSequence()`: Initiates a new blink sequence

**Behavior**:
- Blinks continuously when pedal is active (not bypassed)
- Turns off completely when bypassed
- Blinks N times where N = preset number (1-8)
- 150ms on, 150ms off per blink
- 5 second pause between sequences
- Automatically restarts sequence after pause

**To modify LED2 behavior for custom purposes**, you would need to modify or replace the `updateBlinkState()` function in the main loop. The current implementation is tightly integrated with preset indication.

## Code Navigation Tips

### Key Files for Modification

**Most Common**:
- [petal/CloudSeed/cloudseed.cpp](petal/CloudSeed/cloudseed.cpp) - CloudSeed control mapping and logic
- [petal/CloudyReverb/cloudyreverb.cpp](petal/CloudyReverb/cloudyreverb.cpp) - CloudyReverb control mapping

**Advanced**:
- [CloudSeed/ReverbController.h](CloudSeed/ReverbController.h) - Presets and high-level reverb control
- [CloudSeed/Parameter.h](CloudSeed/Parameter.h) - All available reverb parameters
- [CloudSeed/ReverbChannel.h](CloudSeed/ReverbChannel.h) - Core reverb architecture

**Reference Only** (usually don't modify):
- [CloudSeed/DelayLine.h](CloudSeed/DelayLine.h) - Delay buffer implementation
- [CloudSeed/AudioLib/](CloudSeed/AudioLib/) - Audio utilities
- Submodules (libdaisy, DaisySP, eurorack, Terrarium)

### Understanding Audio Flow

**CloudSeed Audio Callback** (`AudioCallback()` - runs at audio rate, ~48kHz):
1. Process analog/digital controls (knobs, switches, footswitches)
2. Update LED states
3. Read and smooth all parameter values
4. Update reverb parameters (only when values change)
5. Handle delay line count switching
6. Check for preset change request → set trigger flag
7. Check for bypass toggle
8. Process audio: `reverb.Process(input, bufferSize)`
9. Mix and output dry/wet signals

**CloudSeed Main Loop** (`main()` while loop - runs at ~100Hz):
1. **Handle preset changes**: When triggered by footswitch
   - Call `cyclePreset()` to load new preset
   - Call `save_settings()` to queue flash write
   - Start LED2 blink sequence for new preset
2. **Handle flash writes**: Write settings to QSPI when queued
3. **Update LED2 blink state**: Manage LED transitions for preset indication
4. Delay 10ms

**Performance Architecture**:
- **Audio callback**: Time-critical, optimized for low latency
  - Only parameter smoothing and audio processing
  - Sets trigger flags for heavy operations
  - No flash writes or preset loading
- **Main loop**: Non-critical background tasks
  - Preset switching (includes buffer clearing)
  - Flash memory writes
  - LED blink state machine

This separation prevents audio glitches during preset changes and flash writes.

**CloudyReverb**:
1. Input → `AudioCallback()` in cloudyreverb.cpp
2. Read controls → Update reverb settings
3. Process: `clouds_reverb.Process()`
4. Mix wet/dry
5. Output

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
// Use LED_2 for debugging:
hw.led2.Set(parameter_value);  // Visual feedback
hw.led2.Update();
```

### Common Issues

**Build Errors**:
- Ensure submodules are initialized: `git submodule update --init --recursive`
- Rebuild libraries: `./rebuild_libs.sh`
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
- Runs at ~70-80% CPU (estimated)

**CloudyReverb**: Lightweight
- Optimized Mutable Instruments algorithm
- Runs at ~30-40% CPU (estimated)

### Memory Usage

**CloudSeed**:
- 48MB SDRAM for delay buffers
- ~100KB SRAM for processing

**CloudyReverb**:
- Minimal SDRAM usage
- ~50KB SRAM for processing

### Optimization Tips

1. **Use hard float**: Already enabled (`-mfloat-abi=hard`)
2. **Optimize compilation**: Already using `-O3`
3. **Minimize parameter updates**: Use smoothing (Parameter class)
4. **Avoid dynamic allocation**: Use pre-allocated buffers
5. **Profile with map file**: Check `build/*.map` for code size

## Further Resources

### Documentation
- Daisy Wiki: https://github.com/electro-smith/DaisyWiki/wiki
- libdaisy API: https://electro-smith.github.io/libDaisy/
- DaisySP API: https://electro-smith.github.io/DaisySP/

### Source Projects
- CloudSeed VST: https://github.com/ValdemarOrn/CloudSeed
- Mutable Instruments: https://github.com/pichenettes/eurorack
- GuitarML fork: https://github.com/GuitarML/DaisyCloudSeed

### Hardware
- Daisy Seed: https://www.electro-smith.com/daisy/daisy
- PedalPCB Terrarium: https://www.pedalpcb.com/product/pcb351/

## Project-Specific Notes

### Mono vs Stereo

This fork differs from the original CloudSeed:
- **Original**: Stereo (2 channels, 2 delay lines each)
- **Terrarium**: Mono (1 channel, 5 delay lines)

The trade-off: More delay lines in mono = richer reverb tail.

### GuitarML Modifications

Key changes in this fork:
1. Adapted for Terrarium hardware (mono, 6 knobs, 4 switches)
2. Increased delay line count from 2 to 5
3. Added preset cycling via footswitch
4. Simplified control scheme for guitar pedal use
5. Added delay line switching via toggle switches
6. Low-pass filter control on KNOB_6
7. **Persistent preset storage** in QSPI flash memory with version control
8. **LED2 blink pattern system** for visual preset indication
9. **Array-based preset configuration** with function pointers and designated initializers
10. **Performance optimization**: Moved preset switching and flash writes from audio callback to main loop
11. **Modulo-based preset cycling** for cleaner wraparound logic

### Version Information

Check git history for recent changes:
```bash
git log --oneline -10
```

Current branch: master
Recent commits show lowpass filter control addition.

## Quick Reference Card

### Build & Flash
```bash
./rebuild_libs.sh          # Build libraries
cd petal/CloudSeed && make # Build CloudSeed
make program-dfu           # Flash to Daisy
```

### File Locations
- Control mapping: `petal/CloudSeed/cloudseed.cpp`
- Preset array configuration: `petal/CloudSeed/cloudseed.cpp` (lines 55-96)
- Preset initialization functions: `CloudSeed/ReverbController.h`
- Parameters: `CloudSeed/Parameter.h`
- Hardware config: `Terrarium/terrarium.h`

### Key Concepts
- Buffer size: 48 samples
- Sample rate: 48kHz (typical)
- SDRAM pool: 48MB
- Delay lines: 5 (mono Terrarium)
- Presets: 8 factory presets (array-based, auto-counted)
- Persistent storage: QSPI flash with version control
- LED2: Continuous blink pattern indicates preset number

### Quick Modifications
1. Control mapping → cloudseed.cpp:242-247
2. Parameter ranges → Init() calls in cloudseed.cpp
3. Add/modify presets → cloudseed.cpp:55-96 (PRESETS array)
4. Blink patterns → cloudseed.cpp:55-96 (blinkPattern fields)
5. Switch logic → cloudseed.cpp (delay line control)
6. LED2 blink behavior → cloudseed.cpp:180-229 (updateBlinkState)
