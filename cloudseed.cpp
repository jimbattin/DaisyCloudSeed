// Modified version of DaisyCloudSeed by Keith Bloemer.
// Intended for the Terrarrium guitar pedal hardware.
// Code has been modified for mono processing (originally stereo) to 
// allow up to 5 delay lines on the Daisy Seed.

#include "daisy_petal.h"
#include "daisysp.h"
#include "terrarium.h"
#include "cmsis_gcc.h"
#include <cmath>
#include <array>
#include <stdio.h>
#include <string.h>

#include "CloudSeed/Default.h"
#include "CloudSeed/ReverbController.h"
#include "CloudSeed/FastSin.h"
#include "CloudSeed/AudioLib/ValueTables.h"
#include "CloudSeed/AudioLib/MathDefs.h"
#include "CloudSeed/ReverseDelay.h"
#include "preset_bank.h"
#include "knob_bank.h"
#include "footswitch_combo.h"

using namespace daisy;
using namespace daisysp;
using namespace terrarium;  // This is important for mapping the correct controls to the Daisy Seed on Terrarium PCB

// Constants
constexpr size_t AUDIO_BUFFER_SIZE = 48;
constexpr float OUTPUT_VOLUME_BOOST = 1.2f;
constexpr float MAKEUP_GAIN_STRENGTH = 0.8f;  // Max additional gain when fully wet (0.0-1.0)
constexpr float FLOAT_EPSILON = 1e-6f;

// Reverse delay stage (records the reverb output and plays it back backwards)
constexpr int   REVERSE_BUFFER_SIZE = 192000;  // 4s @ 48kHz record buffer (SDRAM);
                                               // >= 2x the 2000ms max window so the
                                               // ReverseDelay size/2 clamp never engages
constexpr float REVERSE_GRAIN_NORM  = 0.4771f; // boot default window (~500 ms through
                                               // Response3Oct); the knob mapped to
                                               // "reverse.delay" retunes it
constexpr float REVERSE_TIME_MIN_MS = 20.0f;   // "reverse.delay" knob fully CCW
constexpr float REVERSE_TIME_MAX_MS = 2000.0f; // "reverse.delay" knob fully CW
constexpr float REVERSE_LEVEL       = 0.7f;    // output scale of the reversed signal
constexpr float REVERSE_MIX_SMOOTHING = 0.002f;// per-sample one-pole toward target 0/1

// One-pole coefficient applied to every knob at boot. libdaisy's default slew
// computes to 1.0 at this callback rate (hid/ctrl.cpp:16 with a 1 kHz update and
// the 0.002 s default), i.e. no filtering at all; 0.05 is ~20 ms.
constexpr float KNOB_SMOOTHING_COEFF = 0.05f;


// Volatile global variable used to prevent optimization
volatile float dummy_trig_value = 0.0f;


#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923  // π/2 for equal-power curves
#endif

// Increment this when changing the settings struct so the software will know
// to reset to defaults if this ever changes.
#define SETTINGS_VERSION 2

// Switch to control Bloom (reverse tap decay)
static const int BLOOM_SWITCH = Terrarium::SWITCH_2;

// LED blink pattern configuration (defined early for use in preset config)
struct BlinkPattern {
    int numBlinks;           // Number of times to blink
    uint32_t onDurationMs;   // How long LED stays on per blink
    uint32_t offDurationMs;  // How long LED stays off between blinks
    uint32_t pauseAfterMs;   // Pause after all blinks complete
};

// Blink state machine
struct BlinkState {
    bool active;
    int currentBlink;
    bool ledOn;
    uint32_t lastTransitionTime;
    BlinkPattern pattern;
};

// Presets are defined in presets.toml, embedded into the firmware image by
// presets_toml.s and parsed once at boot into gPresets.
extern "C" {
    extern const char     presets_toml[];
    extern const uint32_t presets_toml_len;  // includes the terminating NUL
}

static PresetBank gPresets;
static KnobBank gKnobs;
static FootswitchCombo gCombo;

// Persistent Settings
struct Settings {
    int version;        // Version of the settings struct
    int currentPreset;  // Currently selected preset (0-9)
    bool bypass;         // Persisted bypass state (true = pedal was bypassed at last save)

    // Overloading the != operator
    // This is necessary as this operator is used in the PersistentStorage source code
    bool operator!=(const Settings& a) const {
        return !(
            a.version == version &&
            a.currentPreset == currentPreset &&
            a.bypass == bypass
        );
    }
};

// Global state structure
struct PedalState {
    // Previous parameter values (for change detection)
    int   prevReverseGrainSamples;  // last window length written to reverseDelay
    float prevNumDelayLines;
    bool prevReverseTaps;

    // State
    bool bypass;
    int currentPreset;
    bool triggerPresetChange;    // Set true in audio callback when preset switch pressed
    bool triggerBypassSave;      // Set true in audio callback when bypass is toggled
    bool triggerSettingsSave;    // Set true when settings need to be saved
    bool triggerPresetBlink;     // Set true when we should blink LED to show preset
    bool presetChangeInProgress; // Set true while preset is being changed (prevents audio processing)
    bool reverseTaps;
    bool  reverseDelayOn;  // SWITCH_3 state, sampled each callback
    float reverseMix;      // smoothed 0..1 crossfade for the reverse voice
    float samplesPerMs;    // sampleRate / 1000, precomputed for the reverse-time knob

    // Active preset configuration, copied by loadPreset(). The audio callback and
    // the blink state machine read these, never gPresets.
    KnobTarget   knobMap[kKnobBanks][kKnobCount];
    float        maxDelayLines;
    BlinkPattern blinkPattern;

    // Derived from DryOut/EarlyOut/MainOut; recomputed only when those change.
    bool  outputLevelsDirty;
    float makeupGain;
    float scaledDryOut;

    int   activeKnobBank;   // bank the live KnobBank snapshot was taken for
    float reverseDelayNorm; // normalized reverse-window value; the current value of
                            // the "reverse.delay" target, which has no params[] slot
    bool knobResetPending; // main loop changed the preset; re-snapshot knob positions
    Led led1;
    Led led2;
};

// knob1..knob6 in presets.toml order -> hw.knob[] indices (Terrarium ADC order)
static const int kKnobIndex[kKnobCount] = {
    Terrarium::KNOB_1, Terrarium::KNOB_2, Terrarium::KNOB_3,
    Terrarium::KNOB_4, Terrarium::KNOB_5, Terrarium::KNOB_6};

// Declare a local daisy_petal for hardware access
DaisyPetal hw;
PedalState state;
CloudSeed::ReverbController* reverb = nullptr;
CloudSeed::ReverseDelay reverseDelay;

// Persistent Storage Declaration. Using type Settings and passed the device's qspi handle
PersistentStorage<Settings> SavedSettings(hw.seed.qspi);

BlinkState led2BlinkState = {false, 0, false, 0, {0, 0, 0, 0}};

/*
 * Memory pool for delay lines
 */

// This is used in the modified CloudSeed code for allocating
// delay line memory to SDRAM (64MB available on Daisy)
#define CUSTOM_POOL_SIZE (48*1024*1024)
DSY_SDRAM_BSS char custom_pool[CUSTOM_POOL_SIZE];
DSY_SDRAM_BSS float reverseDelayBuffer[REVERSE_BUFFER_SIZE];
size_t pool_index = 0;
int allocation_count = 0;
void* custom_pool_allocate(size_t size) {
    if (pool_index + size >= CUSTOM_POOL_SIZE) {
        // Memory pool exhausted - this should never happen during normal operation
        // If this occurs, it indicates a serious memory allocation issue
        // Returning NULL will likely cause a crash, but it's better than silent corruption
        return 0;
    }
    void* ptr = &custom_pool[pool_index];
    pool_index += size;
    allocation_count++;
    return ptr;
}

// Helper to get memory pool usage statistics
size_t get_pool_usage() {
    return pool_index;
}

size_t get_pool_remaining() {
    return CUSTOM_POOL_SIZE - pool_index;
}

/*
 * Boot-only TOML parse arena
 */

// Carved from the head of custom_pool. Nothing else has allocated from the pool
// yet (the reverb is constructed afterwards), so the whole region is handed back
// simply by abandoning it. The heap is deliberately avoided: libnosys' _sbrk
// grows unchecked from end = 0x30008000 into the 256 KB RAM_D2 region, and
// custom_pool_allocate() does not align its returns.
constexpr size_t TOML_ARENA_SIZE = 512 * 1024;
static size_t toml_arena_index = 0;

static void* toml_arena_alloc(size_t size) {
    const size_t aligned = (size + 7u) & ~static_cast<size_t>(7u);
    if (toml_arena_index + aligned > TOML_ARENA_SIZE) return nullptr;
    void* ptr = &custom_pool[toml_arena_index];
    toml_arena_index += aligned;
    return ptr;
}

static void toml_arena_free(void*) {}

static bool loadPresetBank(char* err, int errLen) {
    toml_arena_index = 0;
    // toml_parse() mutates its input, so parse a scratch copy, never the .rodata blob.
    char* scratch = static_cast<char*>(toml_arena_alloc(presets_toml_len));
    if (!scratch) { snprintf(err, errLen, "arena too small"); return false; }
    memcpy(scratch, presets_toml, presets_toml_len);
    const bool ok = ParsePresetBank(scratch, gPresets, err, errLen,
                                    toml_arena_alloc, toml_arena_free);
    toml_arena_index = 0;  // release: custom_pool is untouched from here on
    return ok;
}

// Unrecoverable: no presets means no reverb configuration. Blink both LEDs at 5 Hz
// forever and never start audio, so the failure is unmistakable on the pedal.
static void presetErrorLoop() {
    while (true) {
        state.led1.Set(1.0f); state.led2.Set(1.0f);
        state.led1.Update(); state.led2.Update();
        System::Delay(100);
        state.led1.Set(0.0f); state.led2.Set(0.0f);
        state.led1.Update(); state.led2.Update();
        System::Delay(100);
    }
}

/*
 * Presets
 */

void loadPreset(int presetIndex) {
    // Validate preset index
    if (presetIndex < 0 || presetIndex >= gPresets.count) {
        presetIndex = 0;  // Default to first preset if invalid
    }

    const PresetData& p = gPresets.presets[presetIndex];

    reverb->ClearBuffers();
    reverb->LoadPreset(p.params);

    // Apply the preset's non-parameter configuration to pedal state. This is the
    // only place gPresets is read after boot; the audio callback uses the copies.
    memcpy(state.knobMap, p.knobMap, sizeof state.knobMap);
    state.maxDelayLines = p.maxDelayLines;
    state.blinkPattern  = BlinkPattern{p.blinks, p.onDurationMs, p.offDurationMs,
                                       p.pauseAfterMs};
    state.outputLevelsDirty = true;
}

/*
 * Persistent settings
 */

void loadSettings() {
    // Reference to local copy of settings stored in flash
    Settings &localSettings = SavedSettings.GetSettings();

    int savedVersion = localSettings.version;

    if (savedVersion != SETTINGS_VERSION) {
        // Something has changed. Load defaults!
        SavedSettings.RestoreDefaults();
        loadSettings();
        return;
    }

    // Load and validate the preset
    state.currentPreset = localSettings.currentPreset;

    // Validate preset range and default to 0 if invalid
    if (state.currentPreset < 0 || state.currentPreset >= gPresets.count) {
        state.currentPreset = 0;
    }

    // Load bypass state (no range validation needed: bool has no invalid values
    // once the SETTINGS_VERSION check above guarantees a freshly-defaulted struct
    // on any layout mismatch)
    state.bypass = localSettings.bypass;

    loadPreset(state.currentPreset);
}

void saveSettings() {
    // Reference to local copy of settings stored in flash
    Settings &localSettings = SavedSettings.GetSettings();

    localSettings.version = SETTINGS_VERSION;
    localSettings.currentPreset = state.currentPreset;
    localSettings.bypass = state.bypass;

    state.triggerSettingsSave = true;
}

void cyclePreset() {
    state.currentPreset = (state.currentPreset + 1) % gPresets.count;
    loadPreset(state.currentPreset);
}


// Helper function to check if float values differ significantly
inline bool hasChanged(float prev, float current) {
    return (prev < current - FLOAT_EPSILON) || (prev > current + FLOAT_EPSILON);
}

// Antilog reverse-window mapping via the same ValueTables pattern the engine uses for
// Parameter::LineDelay (CloudSeed/ReverbController.h:114): min + table * span.
// Response3Oct is (8^x - 1) / 7 normalized, so 0.0 -> 20ms, 0.5 -> ~537ms, 1.0 -> 2000ms.
static float reverseWindowMs(float norm) {
    return REVERSE_TIME_MIN_MS
         + AudioLib::ValueTables::Get(norm, AudioLib::ValueTables::Response3Oct)
           * (REVERSE_TIME_MAX_MS - REVERSE_TIME_MIN_MS);
}

// Writes one knob value to its mapped destination.
static void applyKnobTarget(const KnobTarget& target, float value) {
    if (target.kind == KnobTarget_ReverseDelay) {
        const float reverseMs    = reverseWindowMs(value);
        const int   grainSamples = (int)(reverseMs * state.samplesPerMs);
        if (grainSamples != state.prevReverseGrainSamples) {
            reverseDelay.SetGrainSamples(grainSamples);
            state.prevReverseGrainSamples = grainSamples;
        }
        state.reverseDelayNorm = value;
        return;
    }

    const ::Parameter param = (::Parameter)target.paramIndex;
    reverb->SetParameter(param, value);

    switch (param) {
    // The two input filters are switch-gated and off in most presets, so a knob
    // mapped to one of them would otherwise be silent. Enabling on first touch
    // leaves an untouched preset exactly as authored.
    case ::Parameter::HighPass:
        if (reverb->GetAllParameters()[(int)::Parameter::HiPassEnabled] < 0.5f)
            reverb->SetParameter(::Parameter::HiPassEnabled, 1.0f);
        break;
    case ::Parameter::LowPass:
        if (reverb->GetAllParameters()[(int)::Parameter::LowPassEnabled] < 0.5f)
            reverb->SetParameter(::Parameter::LowPassEnabled, 1.0f);
        break;
    // These three are the only inputs to makeupGain / scaledDryOut.
    case ::Parameter::DryOut:
    case ::Parameter::EarlyOut:
    case ::Parameter::MainOut:
        state.outputLevelsDirty = true;
        break;
    default:
        break;
    }
}

// Current value of whatever a knob is mapped to, in knob (0..1) space. SetParameter
// stores knob values verbatim in parameters[] (CloudSeed/ReverbController.h:170), so
// this is directly comparable to a raw knob position.
static float knobTargetValue(const KnobTarget& target) {
    if (target.kind == KnobTarget_ReverseDelay)
        return state.reverseDelayNorm;
    return reverb->GetAllParameters()[target.paramIndex];
}

/*
 * LED blink
 */

// Start a blink sequence
void startBlinkSequence(BlinkPattern pattern) {
    led2BlinkState.active = true;
    led2BlinkState.currentBlink = 0;
    led2BlinkState.ledOn = false;
    led2BlinkState.lastTransitionTime = System::GetNow();
    led2BlinkState.pattern = pattern;
    state.led2.Set(0.0f);
    state.led2.Update();
}

// Update blink state machine (call this in main loop)
void updateBlinkState() {
    // If bypassed, turn off LED2 and deactivate blinking
    if (state.bypass) {
        if (led2BlinkState.active) {
            led2BlinkState.active = false;
            state.led2.Set(0.0f);
            state.led2.Update();
        }
        return;
    }

    // If not active and not bypassed, restart the blink sequence
    if (!led2BlinkState.active) {
        startBlinkSequence(state.blinkPattern);
        return;
    }

    uint32_t now = System::GetNow();
    uint32_t elapsed = now - led2BlinkState.lastTransitionTime;

    if (led2BlinkState.ledOn) {
        // LED is currently on, check if it's time to turn it off
        if (elapsed >= led2BlinkState.pattern.onDurationMs) {
            state.led2.Set(0.0f);
            state.led2.Update();
            led2BlinkState.ledOn = false;
            led2BlinkState.lastTransitionTime = now;
            led2BlinkState.currentBlink++;
        }
    } else {
        // LED is currently off
        if (led2BlinkState.currentBlink >= led2BlinkState.pattern.numBlinks) {
            // All blinks complete, check if pause is done
            if (elapsed >= led2BlinkState.pattern.pauseAfterMs) {
                // Restart the sequence instead of stopping
                led2BlinkState.currentBlink = 0;
                led2BlinkState.ledOn = false;
                led2BlinkState.lastTransitionTime = now;
            }
        } else {
            // More blinks to go, check if it's time to turn LED on again
            if (elapsed >= led2BlinkState.pattern.offDurationMs) {
                state.led2.Set(1.0f);
                state.led2.Update();
                led2BlinkState.ledOn = true;
                led2BlinkState.lastTransitionTime = now;
            }
        }
    }
}

/*
 * Main audio callback
 */

// This runs at a fixed rate, to prepare audio samples
static void audioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size) {

    // Audio buffers
    static float audioInputBuffer[AUDIO_BUFFER_SIZE];
    static float audioOutputBuffer[AUDIO_BUFFER_SIZE];
    static float reverseOutputBuffer[AUDIO_BUFFER_SIZE];
    static float reverbInputBuffer[AUDIO_BUFFER_SIZE];

    hw.ProcessAnalogControls();
    hw.ProcessDigitalControls();
    state.led1.Update();
    state.led2.Update();

    //
    // Process footswitches
    //

    // Holding both footswitches is the secondary-knob gesture, not a bypass toggle
    // and not a preset change. Both actions fire on the release edge, and the edges
    // belonging to a combo press are eaten by FootswitchCombo. All four accessors
    // are read exactly once per callback: libdaisy's edge flags are only valid for
    // the update in which they occur (hid/switch.h:62-66).
    const FootswitchCombo::Actions fsActions = gCombo.Update(
        hw.switches[Terrarium::FOOTSWITCH_1].Pressed(),
        hw.switches[Terrarium::FOOTSWITCH_2].Pressed(),
        hw.switches[Terrarium::FOOTSWITCH_1].FallingEdge(),
        hw.switches[Terrarium::FOOTSWITCH_2].FallingEdge());

    // (De-)Activate bypass and toggle LED when the left footswitch is released
    if (fsActions.fs1Release) {
        state.bypass = !state.bypass;
        state.led1.Set(state.bypass ? 0.0f : 1.0f);
        state.triggerBypassSave = true;
    }

    // Cycle presets (actual preset change happens in main loop)
    if (fsActions.fs2Release) {
        state.triggerPresetChange = true;
    }

    //
    // Process knobs and toggle switches
    //

    const bool reverseTaps = hw.switches[BLOOM_SWITCH].Pressed();

    // SWITCH_3: reverse voice on/off. The reverse window length is a knob_map
    // target ("reverse.delay"), not a SWITCH_3 overload of KNOB_4.
    state.reverseDelayOn = hw.switches[Terrarium::SWITCH_3].Pressed();

    float knobValues[kKnobCount];
    for (int i = 0; i < kKnobCount; i++)
        knobValues[i] = hw.knob[kKnobIndex[i]].Value();

    // Holding both footswitches selects bank 1; the gesture stays engaged until both
    // are released, so lifting one foot first cannot drop the bank mid-turn.
    const int bank = gCombo.active ? 1 : 0;

    // Every reverb write is skipped while the main loop is loading a preset: it is
    // rewriting state.knobMap and all 47 engine parameters at the same time. The
    // re-snapshot below is gated for the same reason (it must not straddle a knobMap
    // rewrite); knobResetPending simply stays set until the load completes.
    if (!state.presetChangeInProgress) {
        // Any bank or preset transition re-snapshots knob positions, so movement made
        // in one bank is never replayed as a delta into the other bank's target.
        if (bank != state.activeKnobBank || state.knobResetPending || !gKnobs.primed) {
            gKnobs.Reset(knobValues);
            state.activeKnobBank   = bank;
            state.knobResetPending = false;
        }

        // Range-scaled relative takeover: a knob's movement since its last accepted
        // update is added to the current value of its target, scaled by the range left
        // in the direction of travel, so it responds immediately from wherever it sits,
        // never jumps the parameter to the pot's absolute position, and lands exactly on
        // 0.0 / 1.0 when turned to a stop.
        for (int i = 0; i < kKnobCount; i++) {
            const KnobTarget& target = state.knobMap[bank][i];
            float             value;
            if (gKnobs.Update(i, knobValues[i], knobTargetValue(target), value))
                applyKnobTarget(target, value);
        }

        // SWITCH_1: off = 2 delay lines, on = the preset's max
        const float numDelayLines = hw.switches[Terrarium::SWITCH_1].Pressed()
            ? state.maxDelayLines
            : 2.0f;

        if (hasChanged(state.prevNumDelayLines, numDelayLines)) {
            reverb->SetParameter(::Parameter::LineCount, numDelayLines);
            state.prevNumDelayLines = numDelayLines;
        }

        // Process Bloom/Reverse tap switch
        if (state.prevReverseTaps != reverseTaps) {
            reverb->SetParameter(::Parameter::isReverse, reverseTaps ? 1.0f : 0.0f);
            state.prevReverseTaps = reverseTaps;
        }
    }

    // SWITCH_4: reverse destination (off = into reverb wet path; on = direct output mix)
    const bool reverseIntoReverb = !hw.switches[Terrarium::SWITCH_4].Pressed();



    //
    // Process audio
    //

    // Copy input to buffers
    for (size_t i = 0; i < size; i++) {
        audioInputBuffer[i] = in[0][i]; // left channel
    }

    // Apply effect or bypass
    // IMPORTANT: Skip reverb processing if preset change is in progress to avoid race condition
    // We want to compute our reverb output even when bypassed to minimize 1khz whine
    if (!state.presetChangeInProgress) {
        const float reverseTarget = state.reverseDelayOn ? 1.0f : 0.0f;

        // Makeup gain and the dry-cancellation scale depend only on the three output
        // levels, so they are derived when those change, not once per block.
        if (state.outputLevelsDirty) {
            const float* levels        = reverb->GetAllParameters();
            const float  dryOutValue   = levels[(int)::Parameter::DryOut];
            const float  earlyOutValue = levels[(int)::Parameter::EarlyOut];
            const float  mainOutValue  = levels[(int)::Parameter::MainOut];
            const float  totalSignal   = dryOutValue + earlyOutValue + mainOutValue
                                         + FLOAT_EPSILON;
            const float  wetBalance    = (earlyOutValue + mainOutValue) / totalSignal;
            const float  compensation  = sinf(wetBalance * M_PI_2);
            state.makeupGain   = OUTPUT_VOLUME_BOOST
                                 * (1.0f + compensation * MAKEUP_GAIN_STRENGTH);
            state.scaledDryOut = reverb->GetScaledParameter(::Parameter::DryOut);
            state.outputLevelsDirty = false;
        }
        const float makeupGain = state.makeupGain;

        if (reverseIntoReverb) {
            // SWITCH_4 off: reverse the dry guitar and inject it into the reverb input.
            // The reverb re-emits dryOut*(dry+injectedReverse); we subtract
            // dryOut*injectedReverse at the output so the dry pass-through stays the
            // clean, non-reversed guitar and the reverse is heard only through the wet
            // tail. With early/late at zero the reverb adds nothing, so no reverse plays.
            reverseDelay.Process(audioInputBuffer, reverseOutputBuffer, AUDIO_BUFFER_SIZE);
            for (size_t i = 0; i < AUDIO_BUFFER_SIZE; i++) {
                state.reverseMix += (reverseTarget - state.reverseMix) * REVERSE_MIX_SMOOTHING;
                float injectedReverse = reverseOutputBuffer[i] * REVERSE_LEVEL * state.reverseMix;
                reverseOutputBuffer[i] = injectedReverse; // retained for dry-pass-through cancellation
                reverbInputBuffer[i]   = audioInputBuffer[i] + injectedReverse;
            }
            reverb->Process(reverbInputBuffer, audioOutputBuffer, AUDIO_BUFFER_SIZE);
            const float scaledDryOut = state.scaledDryOut;

            for (size_t i = 0; i < size; i++) {
                if (state.bypass) {
                    out[0][i] = in[0][i];
                }
                else {
                    out[0][i] = (audioOutputBuffer[i] - scaledDryOut * reverseOutputBuffer[i]) * makeupGain;
                }
            }
        }
        else {
            // SWITCH_4 on: today's behavior — reverse records the reverb output and its
            // reversed copy is mixed straight into the output (reverse audible on its own).
            reverb->Process(audioInputBuffer, audioOutputBuffer, AUDIO_BUFFER_SIZE);
            reverseDelay.Process(audioOutputBuffer, reverseOutputBuffer, AUDIO_BUFFER_SIZE);

            for (size_t i = 0; i < size; i++) {
                state.reverseMix += (reverseTarget - state.reverseMix) * REVERSE_MIX_SMOOTHING;
                if (state.bypass) {
                    out[0][i] = in[0][i];
                }
                else {
                    float wet = audioOutputBuffer[i] * makeupGain;
                    float rev = reverseOutputBuffer[i] * makeupGain * REVERSE_LEVEL * state.reverseMix;
                    out[0][i] = wet + rev;
                }
            }
        }
    } else { // Preset change in progress, bypass audio to avoid race condition
        for (size_t i = 0; i < size; i++) {
            out[0][i] = in[0][i];
        }
    }
}

/*
 * Main loop
 */

int main(void) {
    __set_FPSCR(__get_FPSCR() | (1u << 24)); // FZ: flush denormals to zero in hardware
    hw.Init();

    // LEDs first: presetErrorLoop() below is the only way a parse failure can be
    // reported on the pedal.
    state.led1.Init(hw.seed.GetPin(Terrarium::LED_1), false);
    state.led1.Update();

    state.led2.Init(hw.seed.GetPin(Terrarium::LED_2), false);
    state.led2.Update();

    // Parse the embedded presets.toml. SDRAM is only usable after hw.Init(), and
    // the ReverbController below is the first consumer of custom_pool, so the
    // scratch arena window is exactly here.
    char presetErr[128];
    if (!loadPresetBank(presetErr, sizeof presetErr))
        presetErrorLoop();

    const float sampleRate = hw.AudioSampleRate();

    // Initialize audio processing libraries
    AudioLib::ValueTables::Init();
    CloudSeed::FastSin::Init();

    // Initialize reverb controller
    reverb = new CloudSeed::ReverbController(sampleRate);
    reverb->ClearBuffers();

    // Initialize reverse delay stage (records dry input or reverb output for backward playback)
    const int bootGrainSamples =
        (int)(sampleRate * reverseWindowMs(REVERSE_GRAIN_NORM) / 1000.0f);
    reverseDelay.Init(reverseDelayBuffer, REVERSE_BUFFER_SIZE, bootGrainSamples);
    reverseDelay.ClearBuffers();

    // Give the knobs real ADC smoothing: libdaisy's default slew computes to a
    // pass-through at this callback rate (see hid/ctrl.cpp:16).
    for (int i = 0; i < kKnobCount; i++)
        hw.knob[kKnobIndex[i]].SetCoeff(KNOB_SMOOTHING_COEFF);

    // Initialize previous parameter values
    state.prevReverseGrainSamples = bootGrainSamples;
    state.prevNumDelayLines = 0.0f; // Let the audio callback capture the real value of active lines
    state.prevReverseTaps = false;

    // Initialize state
    state.bypass = true;
    state.triggerPresetChange = false;
    state.triggerBypassSave = false;
    state.triggerSettingsSave = false;
    state.triggerPresetBlink = false;
    state.presetChangeInProgress = false;
    state.reverseDelayOn = false;
    state.reverseMix = 0.0f;
    state.samplesPerMs = sampleRate / 1000.0f;
    state.activeKnobBank = 0;
    state.reverseDelayNorm = REVERSE_GRAIN_NORM;
    state.knobResetPending = false;
    state.outputLevelsDirty = true;
    state.makeupGain        = OUTPUT_VOLUME_BOOST;
    state.scaledDryOut      = 0.0f;

    // Initialize persistent storage with default settings
    Settings defaultSettings = {
        SETTINGS_VERSION,  // version
        0,                 // currentPreset (default to Chorus preset)
        true               // bypass (default to bypassed/silent on first boot)
    };
    SavedSettings.Init(defaultSettings);

    // Load settings from persistent storage (with resilience to failures)
    loadSettings();

    // Reflect the restored bypass state on LED1 (Init() above only configured GPIO
    // polarity; it did not light LED1 for a restored non-bypassed startup state)
    state.led1.Set(state.bypass ? 0.0f : 1.0f);
    state.led1.Update();

    // Start audio processing
    hw.StartAdc();

    // Converge the knob one-poles before the first audio callback. AnalogControl
    // starts at 0.0 and only filters toward the pot position while
    // ProcessAnalogControls() runs (libdaisy/src/hid/ctrl.cpp:13,45), which
    // otherwise first happens inside the audio callback: the KnobBank snapshot
    // would capture ~5% of every real position and the settling ramp would be read
    // as a deliberate turn. 200 ms at KNOB_SMOOTHING_COEFF leaves <0.01% error.
    for (int i = 0; i < 200; i++) {
        hw.ProcessAnalogControls();
        System::Delay(1);
    }

    hw.StartAudio(audioCallback);

    while (true) {
        // Handle preset changes (moved from audio callback for better performance)
        if (state.triggerPresetChange) {
            state.triggerPresetChange = false;

            // Set flag to prevent audio processing during preset change
            // This prevents race condition where audio callback tries to process
            // while buffers are being cleared/modified
            state.presetChangeInProgress = true;

            // Park the knobs before the load so no callback during it can push a
            // stale position into the incoming preset.
            state.knobResetPending = true;
            cyclePreset();
            saveSettings();
            startBlinkSequence(state.blinkPattern);

            // Small delay to ensure all buffers are fully initialized
            // before audio processing resumes
            System::Delay(10);

            state.presetChangeInProgress = false; // Re-enable audio processing
        }

        // Handle bypass persistence (moved from audio callback for better performance,
        // same deferred-write pattern as preset changes above)
        if (state.triggerBypassSave) {
            state.triggerBypassSave = false;
            saveSettings();
        }

        // Handle settings save (moved from audio callback for better performance)
        if (state.triggerSettingsSave) {
            SavedSettings.Save();  // Write locally stored settings to the external flash
            state.triggerSettingsSave = false;
        }

        // Update LED blink state machine
        updateBlinkState();

        // This keeps power-hungry transistors active in the STM32, preventing it from entering
        // a low power state every time we exit the audio callback. This "work" greatly reduces an
        // audible 1khz whine.
        dummy_trig_value = sinf(0.12345f);
    }
}
