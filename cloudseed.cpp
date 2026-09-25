// Modified version of DaisyCloudSeed by Keith Bloemer.
// Intended for the Terrarrium guitar pedal hardware.
// Code has been modified for mono processing (originally stereo) to 
// allow up to 5 delay lines on the Daisy Seed.

#include "daisy_petal.h"
#include "terrarium.h"
#include "cmsis_gcc.h"
#include <atomic>
#include <cmath>
#include <stdio.h>
#include <string.h>

#include "CloudSeed/ReverbController.h"
#include "CloudSeed/FastSin.h"
#include "CloudSeed/AudioLib/ValueTables.h"
#include "CloudSeed/ReverseDelay.h"
#include "preset_bank.h"
#include "knob_bank.h"
#include "preset_footswitch.h"

using namespace daisy;
using namespace terrarium;  // This is important for mapping the correct controls to the Daisy Seed on Terrarium PCB

// Constants
constexpr size_t AUDIO_BUFFER_SIZE = 48;
constexpr float OUTPUT_VOLUME_BOOST = 1.2f;
constexpr float MAKEUP_GAIN_STRENGTH = 0.8f;  // Max additional gain when fully wet (0.0-1.0)
constexpr float FLOAT_EPSILON = 1e-6f;
constexpr float HALF_PI = 1.57079632679489661923f;  // π/2 for equal-power curves
constexpr float TWO_PI  = 6.28318530717958647692f;

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

// Increment this when changing the settings struct so the software will know
// to reset to defaults if this ever changes.
constexpr int SETTINGS_VERSION = 2;

// Settings are written to flash this long after the last preset/bypass change, so a
// burst of footswitch presses costs one QSPI sector erase instead of one per press.
constexpr uint32_t SETTINGS_SAVE_DELAY_MS = 3000;

// Terrarium controls. The values are indices into hw.switches[] (terrarium.h).
constexpr int LINE_COUNT_SWITCH    = Terrarium::SWITCH_1;  // off = 2 lines, on = preset max
constexpr int BLOOM_SWITCH         = Terrarium::SWITCH_2;  // reverse multitap gain order
constexpr int REVERSE_ON_SWITCH    = Terrarium::SWITCH_3;  // reverse voice on/off
constexpr int REVERSE_ROUTE_SWITCH = Terrarium::SWITCH_4;  // off = into reverb, on = direct mix
constexpr int BYPASS_FOOTSWITCH    = Terrarium::FOOTSWITCH_1;
constexpr int PRESET_FOOTSWITCH    = Terrarium::FOOTSWITCH_2;

// LED blink pattern configuration (defined early for use in preset config)
struct BlinkPattern {
    int numBlinks;           // Number of times to blink
    uint32_t onDurationMs;   // How long LED stays on per blink
    uint32_t offDurationMs;  // How long LED stays off between blinks
    uint32_t pauseAfterMs;   // Pause after all blinks complete
};

// Blink state machine
struct BlinkState {
    bool         active             = false;
    int          currentBlink       = 0;
    bool         ledOn              = false;
    uint32_t     lastTransitionTime = 0;
    BlinkPattern pattern            = {};
};

// Presets are defined in presets.toml, embedded into the firmware image by
// presets_toml.s and parsed once at boot into gPresets.
extern "C" {
    extern const char     presets_toml[];
    extern const uint32_t presets_toml_len;  // includes the terminating NUL
}

static PresetBank gPresets;
static KnobBank gKnobs;
static PresetFootswitch gPresetFs;

// Persistent Settings
struct Settings {
    int version;        // Version of the settings struct
    int currentPreset;  // 0 .. gPresets.count-1
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

static bool     gSettingsSavePending = false;  // RAM copy differs from what was last saved
static uint32_t gSettingsChangedAtMs = 0;      // System::GetNow() of the last change

// Global state structure
struct PedalState {
    // Shared with the main loop (volatile). The audio callback interrupts the main
    // loop, so these must be re-read from memory on every access.
    volatile bool bypass                 = true;
    volatile bool triggerPresetChange    = false;  // set by the callback on an FS2 tap
    volatile bool presetChangeInProgress = false;  // main loop is loading a preset: pass through
    volatile bool knobResetPending       = false;  // main loop changed the preset; re-snapshot knobs
    int currentPreset = 0;  // main loop only

    // Active preset configuration, written only while presetChangeInProgress (or
    // before audio starts) by loadPreset(). The audio callback and the blink state
    // machine read these, never gPresets.
    KnobTarget   knobMap[kKnobBanks][kKnobCount] = {};
    float        maxDelayLines = 0.0f;
    BlinkPattern blinkPattern  = {};
    bool         outputLevelsDirty = true;  // DryOut/EarlyOut/MainOut changed

    // Audio callback only.
    float prevNumDelayLines       = 0.0f;   // 0 forces the first callback to write LineCount
    bool  prevBloom               = false;
    int   prevReverseGrainSamples = 0;      // last window length written to reverseDelay
    float reverseDelayNorm = REVERSE_GRAIN_NORM; // normalized reverse-window value; the current
                                                 // value of the "reverse.delay" target, which
                                                 // has no params[] slot
    float reverseMix   = 0.0f;  // smoothed 0..1 crossfade for the reverse voice
    float samplesPerMs = 0.0f;  // sampleRate / 1000, precomputed for the reverse-time knob
    // Derived from DryOut/EarlyOut/MainOut; recomputed only when those change.
    float makeupGain   = OUTPUT_VOLUME_BOOST;
    float scaledDryOut = 0.0f;

    // Once audio runs, only the callback calls Update(): Led::Update() is a
    // read-modify-write and would race with the main loop.
    Led led1;
    Led led2;
};

// knob1..knob6 in presets.toml order -> hw.knob[] indices (Terrarium ADC order)
static const int kKnobIndex[kKnobCount] = {
    Terrarium::KNOB_1, Terrarium::KNOB_2, Terrarium::KNOB_3,
    Terrarium::KNOB_4, Terrarium::KNOB_5, Terrarium::KNOB_6};

// Declare a local daisy_petal for hardware access
static DaisyPetal hw;
static PedalState state;
static CloudSeed::ReverbController* reverb = nullptr;
static CloudSeed::ReverseDelay reverseDelay;

// Persistent Storage Declaration. Using type Settings and passed the device's qspi handle
static PersistentStorage<Settings> SavedSettings(hw.seed.qspi);

static BlinkState led2BlinkState;

// Unrecoverable boot failure (preset parse, SDRAM pool exhausted, unexpected audio
// block size). Blink both LEDs at 5 Hz forever and never start audio, so the failure
// is unmistakable on the pedal. Runs before any audio callback, so it drives the LEDs
// itself.
[[noreturn]] static void fatalErrorLoop() {
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
 * Memory pool for delay lines
 */

static constexpr size_t alignUp8(size_t n) {
    return (n + 7u) & ~static_cast<size_t>(7u);
}

// This is used in the modified CloudSeed code for allocating
// delay line memory to SDRAM (64MB available on Daisy)
constexpr size_t CUSTOM_POOL_SIZE = 48u * 1024u * 1024u;
DSY_SDRAM_BSS __attribute__((aligned(32))) static char custom_pool[CUSTOM_POOL_SIZE];
DSY_SDRAM_BSS static float reverseDelayBuffer[REVERSE_BUFFER_SIZE];
static size_t pool_index = 0;

// Bump allocator, no free. Returns 8-byte aligned blocks. Declared extern by the
// CloudSeed headers, so the signature and external linkage must stay as they are.
void* custom_pool_allocate(size_t size) {
    const size_t aligned = alignUp8(size);
    if (aligned > CUSTOM_POOL_SIZE - pool_index)
        fatalErrorLoop();  // callers placement-new into the result; 0x0 is ITCMRAM on the H750
    void* ptr = &custom_pool[pool_index];
    pool_index += aligned;
    return ptr;
}

/*
 * Boot-only TOML parse arena
 */

// Carved from the head of custom_pool. Nothing else has allocated from the pool
// yet (the reverb is constructed afterwards), so the whole region is handed back
// simply by abandoning it. The heap is deliberately avoided: libnosys' _sbrk
// grows unchecked from end = 0x30008000 into the 256 KB RAM_D2 region.
constexpr size_t TOML_ARENA_SIZE = 512 * 1024;
static size_t toml_arena_index = 0;

static void* toml_arena_alloc(size_t size) {
    const size_t aligned = alignUp8(size);
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

/*
 * Presets
 */

static void loadPreset(int presetIndex) {
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

static void cyclePreset() {
    state.currentPreset = (state.currentPreset + 1) % gPresets.count;
    loadPreset(state.currentPreset);
}

/*
 * Persistent settings
 */

static void loadSettings() {
    // A layout change (SETTINGS_VERSION mismatch) discards the stored struct.
    if (SavedSettings.GetSettings().version != SETTINGS_VERSION)
        SavedSettings.RestoreDefaults();

    const Settings& s = SavedSettings.GetSettings();
    state.currentPreset =
        (s.currentPreset >= 0 && s.currentPreset < gPresets.count) ? s.currentPreset : 0;
    state.bypass = s.bypass;
    loadPreset(state.currentPreset);
}

// Main loop only. Mirrors pedal state into the RAM copy of the settings and writes
// flash SETTINGS_SAVE_DELAY_MS after the last change, so a burst of preset/bypass
// changes costs one QSPI sector erase. Save() skips the erase entirely when flash
// already matches (PersistentStorage::StoreSettingsIfChanged).
static void serviceSettingsSave() {
    Settings&  s      = SavedSettings.GetSettings();
    const bool bypass = state.bypass;
    if (s.currentPreset != state.currentPreset || s.bypass != bypass) {
        s.currentPreset      = state.currentPreset;
        s.bypass             = bypass;
        gSettingsSavePending = true;
        gSettingsChangedAtMs = System::GetNow();
    }
    if (gSettingsSavePending && System::GetNow() - gSettingsChangedAtMs >= SETTINGS_SAVE_DELAY_MS) {
        SavedSettings.Save();
        gSettingsSavePending = false;
    }
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
    const float* params = reverb->GetAllParameters();
    // A disabled input filter is heard as fully open, and applyKnobTarget() enables
    // it on the first write, so the takeover glide must start from the open end, not
    // from the stored cutoff.
    switch ((::Parameter)target.paramIndex) {
    case ::Parameter::HighPass:
        if (params[(int)::Parameter::HiPassEnabled] < 0.5f)
            return 0.0f;
        break;
    case ::Parameter::LowPass:
        if (params[(int)::Parameter::LowPassEnabled] < 0.5f)
            return 1.0f;
        break;
    default:
        break;
    }
    return params[target.paramIndex];
}

/*
 * LED blink
 */

// Start a blink sequence. LED2 itself is pushed to the pin by the audio callback.
static void startBlinkSequence(const BlinkPattern& pattern) {
    led2BlinkState.active = true;
    led2BlinkState.currentBlink = 0;
    led2BlinkState.ledOn = false;
    led2BlinkState.lastTransitionTime = System::GetNow();
    led2BlinkState.pattern = pattern;
    state.led2.Set(0.0f);
}

// Update blink state machine (call this in main loop)
static void updateBlinkState() {
    // If bypassed, turn off LED2 and deactivate blinking
    if (state.bypass) {
        if (led2BlinkState.active) {
            led2BlinkState.active = false;
            state.led2.Set(0.0f);
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
                led2BlinkState.ledOn = true;
                led2BlinkState.lastTransitionTime = now;
            }
        }
    }
}

/*
 * Main audio callback
 */

// Audio buffers, shared by both render paths. main() guarantees the hardware block
// size equals AUDIO_BUFFER_SIZE.
static float gInputBuffer[AUDIO_BUFFER_SIZE];
static float gWetBuffer[AUDIO_BUFFER_SIZE];
static float gReverseBuffer[AUDIO_BUFFER_SIZE];
static float gReverbInputBuffer[AUDIO_BUFFER_SIZE];

static void processFootswitches() {
    // Each accessor is read exactly once per callback: libdaisy's edge flags are only
    // valid for the update in which they occur (hid/switch.h:62-66).

    // (De-)Activate bypass and toggle LED when the left footswitch is released
    if (hw.switches[BYPASS_FOOTSWITCH].FallingEdge()) {
        state.bypass = !state.bypass;
        state.led1.Set(state.bypass ? 0.0f : 1.0f);
    }

    // Holding the preset footswitch is the secondary-bank gesture. Its release cycles
    // the preset (actual change happens in the main loop) only if no secondary knob
    // wrote a value during the hold.
    if (gPresetFs.Update(hw.switches[PRESET_FOOTSWITCH].Pressed(),
                         hw.switches[PRESET_FOOTSWITCH].FallingEdge())) {
        state.triggerPresetChange = true;
    }
}

// Every reverb write lives here, and the caller skips it while the main loop is
// loading a preset: it is rewriting state.knobMap and all 47 engine parameters at the
// same time. The knob re-park is gated for the same reason (it must not straddle a
// knobMap rewrite); knobResetPending simply stays set until the load completes.
static void updateEngineControls(int bank, const float* knobPositions) {
    // Any bank or preset transition re-parks every knob (knob_bank.h): a knob
    // writes nothing until it is turned, then glides its target to the pot's
    // position over kKnobGlideBlocks blocks and tracks it from there.
    float knobCurrent[kKnobCount];
    for (int i = 0; i < kKnobCount; i++)
        knobCurrent[i] = knobTargetValue(state.knobMap[bank][i]);

    KnobWrite knobWrites[kKnobCount];
    const int knobWriteCount = gKnobs.Scan(
        bank, state.knobResetPending, knobPositions, knobCurrent, knobWrites);
    state.knobResetPending = false;
    for (int w = 0; w < knobWriteCount; w++)
        applyKnobTarget(state.knobMap[bank][knobWrites[w].knob], knobWrites[w].value);

    // A secondary knob that wrote during this hold turns the FS2 release into a
    // no-op. Entering bank 1 re-parks (zero writes), so the press itself never counts.
    if (bank == 1 && knobWriteCount > 0)
        gPresetFs.MarkEdited();

    // SWITCH_1: off = 2 delay lines, on = the preset's max. Both candidates are exact
    // copies, so != is an exact change test.
    const float numDelayLines = hw.switches[LINE_COUNT_SWITCH].Pressed()
        ? state.maxDelayLines
        : 2.0f;
    if (numDelayLines != state.prevNumDelayLines) {
        reverb->SetParameter(::Parameter::LineCount, numDelayLines);
        state.prevNumDelayLines = numDelayLines;
    }

    // SWITCH_2: Bloom (reverse multitap gain order)
    const bool bloom = hw.switches[BLOOM_SWITCH].Pressed();
    if (bloom != state.prevBloom) {
        reverb->SetParameter(::Parameter::isReverse, bloom ? 1.0f : 0.0f);
        state.prevBloom = bloom;
    }
}

// Makeup gain and the dry-cancellation scale depend only on the three output
// levels, so they are derived when those change, not once per block.
static void refreshOutputLevels() {
    if (!state.outputLevelsDirty)
        return;
    const float* levels        = reverb->GetAllParameters();
    const float  dryOutValue   = levels[(int)::Parameter::DryOut];
    const float  earlyOutValue = levels[(int)::Parameter::EarlyOut];
    const float  mainOutValue  = levels[(int)::Parameter::MainOut];
    const float  totalSignal   = dryOutValue + earlyOutValue + mainOutValue
                                 + FLOAT_EPSILON;
    const float  wetBalance    = (earlyOutValue + mainOutValue) / totalSignal;
    const float  compensation  = sinf(wetBalance * HALF_PI);
    state.makeupGain   = OUTPUT_VOLUME_BOOST
                         * (1.0f + compensation * MAKEUP_GAIN_STRENGTH);
    state.scaledDryOut = reverb->GetScaledParameter(::Parameter::DryOut);
    state.outputLevelsDirty = false;
}

// SWITCH_4 off: reverse the dry guitar and inject it into the reverb input.
// The reverb re-emits dryOut*(dry+injectedReverse); we subtract
// dryOut*injectedReverse at the output so the dry pass-through stays the
// clean, non-reversed guitar and the reverse is heard only through the wet
// tail. With early/late at zero the reverb adds nothing, so no reverse plays.
static void renderReverseIntoReverb(float reverseTarget, float* out) {
    reverseDelay.Process(gInputBuffer, gReverseBuffer, AUDIO_BUFFER_SIZE);
    float mix = state.reverseMix;
    for (size_t i = 0; i < AUDIO_BUFFER_SIZE; i++) {
        mix += (reverseTarget - mix) * REVERSE_MIX_SMOOTHING;
        const float injected = gReverseBuffer[i] * REVERSE_LEVEL * mix;
        gReverseBuffer[i]     = injected;  // retained for dry-pass-through cancellation
        gReverbInputBuffer[i] = gInputBuffer[i] + injected;
    }
    state.reverseMix = mix;

    reverb->Process(gReverbInputBuffer, gWetBuffer, AUDIO_BUFFER_SIZE);

    const float dry  = state.scaledDryOut;
    const float gain = state.makeupGain;
    for (size_t i = 0; i < AUDIO_BUFFER_SIZE; i++)
        out[i] = (gWetBuffer[i] - dry * gReverseBuffer[i]) * gain;
}

// SWITCH_4 on: the reverse records the reverb output and its reversed copy is mixed
// straight into the output (reverse audible on its own).
static void renderReverseDirect(float reverseTarget, float* out) {
    reverb->Process(gInputBuffer, gWetBuffer, AUDIO_BUFFER_SIZE);
    reverseDelay.Process(gWetBuffer, gReverseBuffer, AUDIO_BUFFER_SIZE);

    float       mix  = state.reverseMix;
    const float gain = state.makeupGain;
    for (size_t i = 0; i < AUDIO_BUFFER_SIZE; i++) {
        mix += (reverseTarget - mix) * REVERSE_MIX_SMOOTHING;
        out[i] = (gWetBuffer[i] + gReverseBuffer[i] * REVERSE_LEVEL * mix) * gain;
    }
    state.reverseMix = mix;
}

// This runs at a fixed rate, to prepare audio samples
static void audioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    /*size*/) {
    hw.ProcessAnalogControls();
    hw.ProcessDigitalControls();
    state.led1.Update();
    state.led2.Update();
    processFootswitches();

    // The main loop cannot run while this callback does, so read each shared flag once.
    const bool bypass = state.bypass;
    if (state.presetChangeInProgress) {  // main loop is loading a preset: pass through
        memcpy(out[0], in[0], AUDIO_BUFFER_SIZE * sizeof(float));
        return;
    }

    float knobPositions[kKnobCount];
    for (int i = 0; i < kKnobCount; i++)
        knobPositions[i] = hw.knob[kKnobIndex[i]].Value();
    // Holding the preset footswitch selects bank 1 until its release edge.
    updateEngineControls(gPresetFs.Held() ? 1 : 0, knobPositions);
    refreshOutputLevels();

    // SWITCH_3: reverse voice on/off. The reverse window length is a knob_map
    // target ("reverse.delay"), not a SWITCH_3 overload of KNOB_4.
    const float reverseTarget = hw.switches[REVERSE_ON_SWITCH].Pressed() ? 1.0f : 0.0f;
    memcpy(gInputBuffer, in[0], AUDIO_BUFFER_SIZE * sizeof(float));  // left channel
    // The reverb runs even when bypassed: that suppresses an audible 1 kHz whine.
    if (hw.switches[REVERSE_ROUTE_SWITCH].Pressed())
        renderReverseDirect(reverseTarget, out[0]);
    else
        renderReverseIntoReverb(reverseTarget, out[0]);
    if (bypass)
        memcpy(out[0], in[0], AUDIO_BUFFER_SIZE * sizeof(float));
}

/*
 * Main loop
 */

// Main-loop busy work. Keeps the FPU and the load/store path active between audio
// callbacks instead of letting the core idle, which audibly reduces a 1 kHz whine.
// The phase is volatile, so every pass performs a real load, a real sinf() on the FPU
// and two real stores; a constant argument would be folded away at compile time.
static volatile float gBusyPhase = 0.0f;
static volatile float gBusySink  = 0.0f;

static void keepCoreBusy() {
    float phase = gBusyPhase + 0.001f;
    if (phase >= TWO_PI)
        phase -= TWO_PI;
    gBusyPhase = phase;
    gBusySink  = sinf(phase);
}

int main(void) {
    __set_FPSCR(__get_FPSCR() | (1u << 24)); // FZ: flush denormals to zero in hardware
    hw.Init();

    // LEDs first: fatalErrorLoop() is the only way a boot failure can be reported
    // on the pedal.
    state.led1.Init(hw.seed.GetPin(Terrarium::LED_1), false);
    state.led1.Update();

    state.led2.Init(hw.seed.GetPin(Terrarium::LED_2), false);
    state.led2.Update();

    // Every callback loop and buffer is sized by AUDIO_BUFFER_SIZE.
    if (hw.AudioBlockSize() != AUDIO_BUFFER_SIZE)
        fatalErrorLoop();

    // Parse the embedded presets.toml. SDRAM is only usable after hw.Init(), and
    // the ReverbController below is the first consumer of custom_pool, so the
    // scratch arena window is exactly here.
    char presetErr[128];
    if (!loadPresetBank(presetErr, sizeof presetErr))
        fatalErrorLoop();

    const float sampleRate = hw.AudioSampleRate();

    // Initialize audio processing libraries
    AudioLib::ValueTables::Init();
    CloudSeed::FastSin::Init();

    // Initialize reverb controller (loadSettings() below clears and loads a preset)
    reverb = new CloudSeed::ReverbController(sampleRate);

    // Initialize reverse delay stage (records dry input or reverb output for backward
    // playback). Init() clears the buffer.
    const int bootGrainSamples =
        (int)(sampleRate * reverseWindowMs(REVERSE_GRAIN_NORM) / 1000.0f);
    reverseDelay.Init(reverseDelayBuffer, REVERSE_BUFFER_SIZE, bootGrainSamples);
    state.samplesPerMs            = sampleRate / 1000.0f;
    state.prevReverseGrainSamples = bootGrainSamples;

    // Give the knobs real ADC smoothing: libdaisy's default slew computes to a
    // pass-through at this callback rate (see hid/ctrl.cpp:16).
    for (int i = 0; i < kKnobCount; i++)
        hw.knob[kKnobIndex[i]].SetCoeff(KNOB_SMOOTHING_COEFF);

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
        // Preset changes run here, not in the audio callback. The callback passes
        // audio through while presetChangeInProgress is set, so it never sees a
        // half-loaded preset; loadPreset() is synchronous, so the gate reopens as soon
        // as it returns.
        if (state.triggerPresetChange) {
            state.triggerPresetChange    = false;
            state.presetChangeInProgress = true;
            // Park the knobs before the load so no callback during it can push a
            // stale position into the incoming preset.
            state.knobResetPending       = true;
            std::atomic_signal_fence(std::memory_order_seq_cst);  // gate closed before any preset write
            cyclePreset();
            startBlinkSequence(state.blinkPattern);
            std::atomic_signal_fence(std::memory_order_seq_cst);  // preset fully written before reopening
            state.presetChangeInProgress = false;
        }

        serviceSettingsSave();

        // Update LED blink state machine
        updateBlinkState();

        keepCoreBusy();
    }
}
