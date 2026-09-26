// Modified version of DaisyCloudSeed by Keith Bloemer.
// Intended for the Terrarrium guitar pedal hardware.
// Code has been modified for mono processing (originally stereo) to 
// allow up to 5 delay lines on the Daisy Seed.

#include "daisy_petal.h"
#include "terrarium.h"
#include "cmsis_gcc.h"
#include <atomic>
#include <cmath>
#include <string.h>

#include "CloudSeed/ReverbController.h"
#include "CloudSeed/FastSin.h"
#include "CloudSeed/AudioLib/ValueTables.h"
#include "CloudSeed/ReverseDelay.h"
#include "preset_bank.h"
#include "knob_bank.h"
#include "toggle_bank.h"
#include "footswitch_gestures.h"
#include "pedal_leds.h"
#include "sdram_pool.h"
#include "pedal_storage.h"

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
constexpr float REVERSE_TIME_MIN_MS = 20.0f;   // "reverse.delay" = 0.0
constexpr float REVERSE_TIME_MAX_MS = 2000.0f; // "reverse.delay" = 1.0
constexpr float REVERSE_LEVEL       = 0.7f;    // output scale of the reversed signal
constexpr float REVERSE_MIX_SMOOTHING = 0.002f;// per-sample one-pole toward target 0/1

// One-pole coefficient applied to every knob at boot. libdaisy's default slew
// computes to 1.0 at this callback rate (hid/ctrl.cpp:16 with a 1 kHz update and
// the 0.002 s default), i.e. no filtering at all; 0.05 is ~20 ms.
constexpr float KNOB_SMOOTHING_COEFF = 0.05f;

// Terrarium footswitches. The values are indices into hw.switches[] (terrarium.h).
// The four toggles are mapped per preset through [preset.toggle_map] (kToggleIndex).
constexpr int BYPASS_FOOTSWITCH    = Terrarium::FOOTSWITCH_1;
constexpr int PRESET_FOOTSWITCH    = Terrarium::FOOTSWITCH_2;

// presets.toml, embedded by presets_toml.s and parsed once at boot
// (LoadEmbeddedPresetBank()).
static PresetBank gPresets;
static KnobBank gKnobs;
static ToggleBank gToggles;
static FootswitchGestures gFootswitches;

// Global state structure
struct PedalState {
    // Shared with the main loop (volatile). The audio callback interrupts the main
    // loop, so these must be re-read from memory on every access.
    volatile bool bypass                 = true;
    volatile bool triggerPresetChange    = false;  // set by the callback on an FS2 tap
    volatile bool triggerPresetRestore   = false;  // set by the callback on the FS1+FS2 5 s hold
    volatile bool saveSnapshotReady      = false;  // callback published gSaveSnapshot; main loop clears
    volatile bool presetChangeInProgress = false;  // main loop is loading a preset: pass through
    volatile bool controlResetPending    = false;  // main loop changed the preset; re-snapshot knobs and toggles
    int currentPreset = 0;  // main loop only

    // Active preset configuration, written only while presetChangeInProgress (or
    // before audio starts) by loadPreset(). The audio callback and the blink state
    // machine read these, never gPresets.
    KnobTarget   knobMap[kControlBanks][kKnobCount] = {};
    ToggleTarget toggleMap[kControlBanks][kToggleCount] = {};
    // A toggle (either bank) targets HiPassEnabled / LowPassEnabled: knobs leave that
    // enable alone.
    bool         hiPassOwnedByToggle  = false;
    bool         lowPassOwnedByToggle = false;
    float        defaultDelayLines = 2.0f;  // lines while "delay_lines.max" is off
    float        maxDelayLines = 0.0f;
    BlinkPattern blinkPattern  = {};
    bool         outputLevelsDirty = true;  // DryOut/EarlyOut/MainOut changed

    // Audio callback, and loadPreset() behind the presetChangeInProgress gate.
    int   prevReverseGrainSamples = 0;      // last window length written to reverseDelay
    float reverseDelayNorm        = 0.0f;   // set by loadPreset(); current value of the
                                            // "reverse.delay" target, which has no params[] slot
    // Toggle pseudo-targets, set by loadPreset() and then by the levers.
    bool  delayLinesMax           = false;  // "delay_lines.max": max_delay_lines, else default_delay_lines
    bool  reverseEnabled          = false;  // "reverse.enabled": reverse voice on
    bool  reverseDirectMix        = false;  // "reverse.direct_mix": on = direct mix, off = into reverb

    // Audio callback only.
    float prevNumDelayLines       = 0.0f;   // 0 forces the first callback to write LineCount
    float reverseMix   = 0.0f;  // smoothed 0..1 crossfade for the reverse voice
    float samplesPerMs = 0.0f;  // sampleRate / 1000, precomputed for the reverse-time knob
    // Derived from DryOut/EarlyOut/MainOut; recomputed only when those change.
    float makeupGain   = OUTPUT_VOLUME_BOOST;
    float scaledDryOut = 0.0f;
};

// knob1..knob6 in presets.toml order -> hw.knob[] indices (Terrarium ADC order)
static const int kKnobIndex[kKnobCount] = {
    Terrarium::KNOB_1, Terrarium::KNOB_2, Terrarium::KNOB_3,
    Terrarium::KNOB_4, Terrarium::KNOB_5, Terrarium::KNOB_6};

// toggle1..toggle4 in presets.toml order -> hw.switches[] indices (terrarium.h)
static const int kToggleIndex[kToggleCount] = {
    Terrarium::SWITCH_1, Terrarium::SWITCH_2, Terrarium::SWITCH_3, Terrarium::SWITCH_4};

// Declare a local daisy_petal for hardware access
static DaisyPetal hw;
static PedalState state;
static PedalStorage gStorage(hw.seed.qspi);
static CloudSeed::ReverbController* reverb = nullptr;
static CloudSeed::ReverseDelay reverseDelay;

// FS1 5 s hold: the callback snapshots the engine into gSaveSnapshot on the next
// block the preset-change gate is open, then publishes it with saveSnapshotReady.
static bool       gSaveRequested = false;  // audio callback only
static UserPreset gSaveSnapshot;           // callback writes while !saveSnapshotReady;
                                           // main loop reads while it is set

// Reverse voice record buffer (ReverseDelay); lives in SDRAM next to custom_pool.
DSY_SDRAM_BSS static float reverseDelayBuffer[REVERSE_BUFFER_SIZE];

/*
 * Presets
 */

// Antilog reverse-window mapping via the same ValueTables pattern the engine uses for
// Parameter::LineDelay (CloudSeed/ReverbController.h:114): min + table * span.
// Response3Oct is (8^x - 1) / 7 normalized, so 0.0 -> 20ms, 0.4771 -> ~500ms, 1.0 -> 2000ms.
static float reverseWindowMs(float norm) {
    return REVERSE_TIME_MIN_MS
         + AudioLib::ValueTables::Get(norm, AudioLib::ValueTables::Response3Oct)
           * (REVERSE_TIME_MAX_MS - REVERSE_TIME_MIN_MS);
}

// Sets the reverse window from a normalized "reverse.delay" value. Called by the
// audio callback (knob) and by loadPreset() (only while presetChangeInProgress is
// set, or before audio starts), so the two never run concurrently.
static void applyReverseWindow(float norm) {
    const int grainSamples = (int)(reverseWindowMs(norm) * state.samplesPerMs);
    if (grainSamples != state.prevReverseGrainSamples) {
        reverseDelay.SetGrainSamples(grainSamples);
        state.prevReverseGrainSamples = grainSamples;
    }
    state.reverseDelayNorm = norm;
}

// True if any toggle in either bank of `p` targets `param`.
static bool toggleMapTargets(const PresetData& p, ::Parameter param) {
    for (int b = 0; b < kControlBanks; b++)
        for (int t = 0; t < kToggleCount; t++)
            if (p.toggleMap[b][t].kind == ToggleTarget_Param
                && p.toggleMap[b][t].paramIndex == (int)param)
                return true;
    return false;
}

// Loads a preset: the user's saved edit if that slot holds one, else the factory
// values from presets.toml.
static void loadPreset(int presetIndex) {
    // Validate preset index
    if (presetIndex < 0 || presetIndex >= gPresets.count) {
        presetIndex = 0;  // Default to first preset if invalid
    }

    const PresetData& p      = gPresets.presets[presetIndex];
    const UserPreset& u      = gStorage.UserSlot(presetIndex);
    const bool        edited = u.valid == USER_PRESET_VALID;

    reverb->ClearBuffers();
    reverb->LoadPreset(edited ? u.params : p.params);
    applyReverseWindow(edited ? u.reverseDelay : p.reverseDelay);
    state.delayLinesMax    = (edited ? u.delayLinesMax : p.delayLinesMax) >= 0.5f;
    state.reverseEnabled   = (edited ? u.reverseEnabled : p.reverseEnabled) >= 0.5f;
    state.reverseDirectMix = (edited ? u.reverseDirectMix : p.reverseDirectMix) >= 0.5f;

    // Apply the preset's non-parameter configuration to pedal state. This is the
    // only place gPresets is read after boot; the audio callback uses the copies.
    // None of it is user-editable, so it always comes from presets.toml: the saved
    // delayLinesMax flag only chooses between the two line counts.
    memcpy(state.knobMap, p.knobMap, sizeof state.knobMap);
    memcpy(state.toggleMap, p.toggleMap, sizeof state.toggleMap);
    state.hiPassOwnedByToggle  = toggleMapTargets(p, ::Parameter::HiPassEnabled);
    state.lowPassOwnedByToggle = toggleMapTargets(p, ::Parameter::LowPassEnabled);
    state.defaultDelayLines = p.defaultDelayLines;
    state.maxDelayLines     = p.maxDelayLines;
    state.blinkPattern  = BlinkPattern{p.blinks, p.onDurationMs, p.offDurationMs,
                                       p.pauseAfterMs};
    state.outputLevelsDirty = true;
}

// Main loop only. Loads `index` behind the presetChangeInProgress gate; parks the
// knobs and toggles first.
static void loadPresetGated(int index) {
    state.presetChangeInProgress = true;
    // Park the knobs and toggles before the load so no callback during it can push a
    // stale position into the incoming preset.
    state.controlResetPending    = true;
    std::atomic_signal_fence(std::memory_order_seq_cst);  // gate closed before any preset write
    state.currentPreset = index;
    loadPreset(index);
    std::atomic_signal_fence(std::memory_order_seq_cst);  // preset fully written before reopening
    state.presetChangeInProgress = false;
}

// Writes one knob value to its mapped destination.
static void applyKnobTarget(const KnobTarget& target, float value) {
    if (target.kind == KnobTarget_ReverseDelay) {
        applyReverseWindow(value);
        return;
    }

    const ::Parameter param = (::Parameter)target.paramIndex;
    reverb->SetParameter(param, value);

    switch (param) {
    // The two input filters are switch-gated and off in most presets, so a knob
    // mapped to one of them would otherwise be silent. Enabling on first touch
    // leaves an untouched preset exactly as authored. A toggle that targets the
    // enable owns it, so the knob then only moves the cutoff.
    case ::Parameter::HighPass:
        if (!state.hiPassOwnedByToggle
            && reverb->GetAllParameters()[(int)::Parameter::HiPassEnabled] < 0.5f)
            reverb->SetParameter(::Parameter::HiPassEnabled, 1.0f);
        break;
    case ::Parameter::LowPass:
        if (!state.lowPassOwnedByToggle
            && reverb->GetAllParameters()[(int)::Parameter::LowPassEnabled] < 0.5f)
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

// Writes one lever position (up = on) to its mapped destination. No toggle target
// feeds the makeup gain, so outputLevelsDirty is left alone.
static void applyToggleTarget(const ToggleTarget& target, bool on) {
    switch (target.kind) {
    case ToggleTarget_DelayLinesMax:
        state.delayLinesMax = on;
        break;
    case ToggleTarget_ReverseEnabled:
        state.reverseEnabled = on;
        break;
    case ToggleTarget_ReverseDirectMix:
        state.reverseDirectMix = on;
        break;
    case ToggleTarget_Param:
        reverb->SetParameter((::Parameter)target.paramIndex, on ? 1.0f : 0.0f);
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
    // from the stored cutoff. When a toggle owns the enable the knob does not touch
    // it, so the glide starts from the stored cutoff.
    switch ((::Parameter)target.paramIndex) {
    case ::Parameter::HighPass:
        if (!state.hiPassOwnedByToggle && params[(int)::Parameter::HiPassEnabled] < 0.5f)
            return 0.0f;
        break;
    case ::Parameter::LowPass:
        if (!state.lowPassOwnedByToggle && params[(int)::Parameter::LowPassEnabled] < 0.5f)
            return 1.0f;
        break;
    default:
        break;
    }
    return params[target.paramIndex];
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
    const FootswitchEvents ev = gFootswitches.Update(
        hw.switches[BYPASS_FOOTSWITCH].Pressed(), hw.switches[BYPASS_FOOTSWITCH].FallingEdge(),
        hw.switches[PRESET_FOOTSWITCH].Pressed(), hw.switches[PRESET_FOOTSWITCH].FallingEdge());

    // FS1 released (not after a save hold, not part of a chord): toggle bypass.
    if (ev.toggleBypass) {
        state.bypass = !state.bypass;
        SetBypassLed(state.bypass);
    }

    // FS2 released with no secondary knob write during the hold: next preset (the
    // actual change happens in the main loop).
    if (ev.cyclePreset)
        state.triggerPresetChange = true;

    // FS1 held 5 s: snapshot the engine into the current preset (audioCallback()).
    if (ev.savePreset)
        gSaveRequested = true;

    // FS1 + FS2 held 5 s: restore the current preset to factory (main loop).
    if (ev.restorePreset)
        state.triggerPresetRestore = true;
}

// Every reverb write lives here, and the caller skips it while the main loop is
// loading a preset: it is rewriting state.knobMap / state.toggleMap and all 47 engine
// parameters at the same time. The knob and toggle re-park is gated for the same
// reason (it must not straddle a map rewrite); controlResetPending simply stays set
// until the load completes.
static void updateEngineControls(int bank, const float* knobPositions,
                                 const bool* togglePositions) {
    // Any bank or preset transition re-parks every knob (knob_bank.h): a knob
    // writes nothing until it is turned, then glides its target to the pot's
    // position over kKnobGlideBlocks blocks and tracks it from there.
    float knobCurrent[kKnobCount];
    for (int i = 0; i < kKnobCount; i++)
        knobCurrent[i] = knobTargetValue(state.knobMap[bank][i]);

    const bool reset = state.controlResetPending;
    KnobWrite knobWrites[kKnobCount];
    const int knobWriteCount = gKnobs.Scan(
        bank, reset, knobPositions, knobCurrent, knobWrites);
    for (int w = 0; w < knobWriteCount; w++)
        applyKnobTarget(state.knobMap[bank][knobWrites[w].knob], knobWrites[w].value);

    // Toggles are parked the same way (toggle_bank.h): a lever writes its position
    // only once it is flipped after a transition.
    ToggleWrite toggleWrites[kToggleCount];
    const int toggleWriteCount = gToggles.Scan(bank, reset, togglePositions, toggleWrites);
    state.controlResetPending = false;
    for (int w = 0; w < toggleWriteCount; w++)
        applyToggleTarget(state.toggleMap[bank][toggleWrites[w].toggle], toggleWrites[w].on);

    // A secondary knob or toggle that wrote during this hold turns the FS2 release
    // into a no-op. Entering bank 1 re-parks (zero writes), so the press itself never
    // counts.
    if (bank == 1 && knobWriteCount + toggleWriteCount > 0)
        gFootswitches.MarkEdited();

    // "delay_lines.max": off = the preset's default_delay_lines, on = its
    // max_delay_lines. Both are exact copies, so != is an exact change test. The
    // toggle writes above run first, so a flip lands in the same block.
    const float numDelayLines = state.delayLinesMax ? state.maxDelayLines
                                                    : state.defaultDelayLines;
    if (numDelayLines != state.prevNumDelayLines) {
        reverb->SetParameter(::Parameter::LineCount, numDelayLines);
        state.prevNumDelayLines = numDelayLines;
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

// "reverse.direct_mix" off: reverse the dry guitar and inject it into the reverb input.
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

// "reverse.direct_mix" on: the reverse records the reverb output and its reversed copy is mixed
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
    UpdateLeds();
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
    bool togglePositions[kToggleCount];
    for (int i = 0; i < kToggleCount; i++)
        togglePositions[i] = hw.switches[kToggleIndex[i]].Pressed();
    // Holding the preset footswitch selects bank 1 until its release edge.
    updateEngineControls(gFootswitches.PresetHeld() ? 1 : 0, knobPositions,
                         togglePositions);

    // FS1 save hold: snapshot here, where the gate is open, so a half-loaded preset
    // is never captured. LineCount comes along; LoadPreset() skips it.
    if (gSaveRequested && !state.saveSnapshotReady) {
        memcpy(gSaveSnapshot.params, reverb->GetAllParameters(), sizeof gSaveSnapshot.params);
        gSaveSnapshot.reverseDelay     = state.reverseDelayNorm;
        gSaveSnapshot.delayLinesMax    = state.delayLinesMax ? 1.0f : 0.0f;
        gSaveSnapshot.reverseEnabled   = state.reverseEnabled ? 1.0f : 0.0f;
        gSaveSnapshot.reverseDirectMix = state.reverseDirectMix ? 1.0f : 0.0f;
        gSaveSnapshot.valid        = USER_PRESET_VALID;
        gSaveRequested             = false;
        std::atomic_signal_fence(std::memory_order_seq_cst);  // snapshot written before publishing
        state.saveSnapshotReady    = true;
    }
    refreshOutputLevels();

    // "reverse.enabled": reverse voice on/off; "reverse.direct_mix" picks the routing.
    // Both are toggle targets, set by loadPreset() and the levers.
    const float reverseTarget = state.reverseEnabled ? 1.0f : 0.0f;
    memcpy(gInputBuffer, in[0], AUDIO_BUFFER_SIZE * sizeof(float));  // left channel
    // The reverb runs even when bypassed: that suppresses an audible 1 kHz whine.
    if (state.reverseDirectMix)
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

    // LEDs first: FatalErrorLoop() is the only way a boot failure can be reported
    // on the pedal.
    LedsInit(hw.seed.GetPin(Terrarium::LED_1), hw.seed.GetPin(Terrarium::LED_2));

    // Every callback loop and buffer is sized by AUDIO_BUFFER_SIZE.
    if (hw.AudioBlockSize() != AUDIO_BUFFER_SIZE)
        FatalErrorLoop();

    // Parse the embedded presets.toml. SDRAM is only usable after hw.Init(), and
    // the ReverbController below is the first consumer of custom_pool, so the
    // scratch arena window is exactly here.
    char presetErr[128];
    if (!LoadEmbeddedPresetBank(gPresets, presetErr, sizeof presetErr))
        FatalErrorLoop();

    const float sampleRate = hw.AudioSampleRate();

    // Initialize audio processing libraries
    AudioLib::ValueTables::Init();
    CloudSeed::FastSin::Init();

    // Initialize reverb controller (loadPreset() below clears and loads a preset)
    reverb = new CloudSeed::ReverbController(sampleRate);

    // Initialize reverse delay stage (records dry input or reverb output for backward
    // playback). Init() clears the buffer; loadPreset() applies the preset's
    // reverse.delay window.
    reverseDelay.Init(reverseDelayBuffer, REVERSE_BUFFER_SIZE, 0);
    state.samplesPerMs = sampleRate / 1000.0f;

    // Give the knobs real ADC smoothing: libdaisy's default slew computes to a
    // pass-through at this callback rate (see hid/ctrl.cpp:16).
    for (int i = 0; i < kKnobCount; i++)
        hw.knob[kKnobIndex[i]].SetCoeff(KNOB_SMOOTHING_COEFF);

    // Flash settings and user preset edits (wiped if a different firmware image saved
    // them). Must precede loadPreset(), which reads the user slots.
    gStorage.Init();
    state.currentPreset = gStorage.RestoredPreset(gPresets.count);
    state.bypass        = gStorage.RestoredBypass();
    loadPreset(state.currentPreset);

    // Reflect the restored bypass state on LED1 (Init() above only configured GPIO
    // polarity; it did not light LED1 for a restored non-bypassed startup state)
    SetBypassLed(state.bypass);
    UpdateLeds();

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
        // Save and restore precede the preset change below: a snapshot pending across
        // the blocking flash write belongs to the preset loaded when it was taken.
        if (state.saveSnapshotReady) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
            gStorage.UserSlot(state.currentPreset) = gSaveSnapshot;
            std::atomic_signal_fence(std::memory_order_seq_cst);
            state.saveSnapshotReady = false;
            gStorage.SaveUserPresets();  // blocking QSPI erase+write; audio keeps running
            StartConfirmBlink();
        }
        if (state.triggerPresetRestore) {
            state.triggerPresetRestore = false;
            gStorage.UserSlot(state.currentPreset) = UserPreset{};
            loadPresetGated(state.currentPreset);  // factory sound now; also drops unsaved knob tweaks
            gStorage.SaveUserPresets();                // no erase if the slot was already empty
            StartConfirmBlink();
        }

        // Preset changes run here, not in the audio callback. The callback passes
        // audio through while presetChangeInProgress is set, so it never sees a
        // half-loaded preset; loadPreset() is synchronous, so the gate reopens as soon
        // as it returns.
        if (state.triggerPresetChange) {
            state.triggerPresetChange = false;
            loadPresetGated((state.currentPreset + 1) % gPresets.count);
            StartPresetBlink(state.blinkPattern);
        }

        gStorage.ServiceSettingsSave(state.currentPreset, state.bypass);

        // LED1+LED2 save/restore confirmation, else the LED2 preset pattern
        if (!ServiceConfirmBlink(state.bypass))
            ServicePresetBlink(state.bypass, state.blinkPattern);

        keepCoreBusy();
    }
}
