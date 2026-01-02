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
- LED_1 (pin 22) - Active indicator
- LED_2 (pin 23) - Available

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

Eight factory presets defined in [CloudSeed/ReverbController.h](CloudSeed/ReverbController.h) (lines 26-84):

1. Chorus
2. Dull Echos
3. Hyperplane
4. Medium Space
5. Noise in the Hallway
6. Rubi Ka Fields
7. Small Room
8. 90s Are Back

Preset cycling logic in cloudseed.cpp:168-177

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

**File**: [CloudSeed/ReverbController.h](CloudSeed/ReverbController.h)

1. Add preset definition in `factoryPresets` array (lines 26-84)
2. Update preset count in cloudseed.cpp if needed
3. Follow existing preset format

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

### 8. Using LED_2

```cpp
// In setup:
hw.led2.Set(brightness);  // 0.0-1.0
hw.led2.Update();

// In audio callback or main loop:
hw.led2.Set(some_modulation_value);
hw.led2.Update();
```

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

**CloudSeed**:
1. Input → `AudioCallback()` in cloudseed.cpp
2. Read controls → Update reverb parameters
3. Process: `reverb.Process(input, bufferSize)`
4. Mix dry/early/late signals
5. Output

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
6. Recent addition: Low-pass filter control on KNOB_6

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
- Presets: `CloudSeed/ReverbController.h`
- Parameters: `CloudSeed/Parameter.h`
- Hardware config: `Terrarium/terrarium.h`

### Key Concepts
- Buffer size: 48 samples
- Sample rate: 48kHz (typical)
- SDRAM pool: 48MB
- Delay lines: 5 (mono Terrarium)
- Presets: 8 factory presets

### Quick Modifications
1. Control mapping → cloudseed.cpp:142-162
2. Parameter ranges → Init() calls in cloudseed.cpp
3. Presets → ReverbController.h:26-84
4. Switch logic → cloudseed.cpp:179-204
5. LED behavior → cloudseed.cpp (LED_1/LED_2)
