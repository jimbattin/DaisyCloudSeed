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
#include "toggle_bank.h"
#include "footswitch_gestures.h"

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

// Increment this when changing the settings struct so the software will know
// to reset to defaults if this ever changes.
constexpr int SETTINGS_VERSION = 2;

// Settings are written to flash this long after the last preset/bypass change, so a
// burst of footswitch presses costs one QSPI sector erase instead of one per press.
constexpr uint32_t SETTINGS_SAVE_DELAY_MS = 3000;

// User preset edits live in the 4 KB QSPI sector after Settings (offset 0, sector 0).
// Both sit in the first 256 KB the Daisy bootloader never touches.
constexpr uint32_t USER_PRESETS_QSPI_OFFSET = 0x1000;
constexpr uint32_t QSPI_SECTOR_BYTES        = 4096;
constexpr uint32_t USER_PRESET_VALID        = 1u;  // erased flash reads 0xFFFFFFFF

// Save / restore confirmation: both LEDs blink together this many times.
constexpr int      CONFIRM_BLINKS   = 3;
constexpr uint32_t CONFIRM_BLINK_MS = 80;  // on and off time

// Terrarium footswitches. The values are indices into hw.switches[] (terrarium.h).
// The four toggles are mapped per preset through [preset.toggle_map] (kToggleIndex).
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
    // Linker symbols bounding .text + .rodata (libdaisy/core/STM32H750IB_sram.lds:36,47),
    // which include the embedded presets.toml. Used as the firmware identity.
    extern const uint32_t _stext[];
    extern const uint32_t _etext[];
}

static PresetBank gPresets;
static KnobBank gKnobs;
static ToggleBank gToggles;
static FootswitchGestures gFootswitches;

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

// One preset's saved edit. Every field is 4 bytes, so there is no padding and the
// memcmp below is exact. `valid` is last: QSPI pages are programmed in ascending
// order, so a slot whose `valid` reads USER_PRESET_VALID was written completely.
struct UserPreset {
    float    params[(int)::Parameter::Count];  // reverb->GetAllParameters() at save time
    float    reverseDelay;                     // state.reverseDelayNorm at save time
    float    delayLinesMax;                    // state.delayLinesMax at save time, 0.0 or 1.0
    float    reverseEnabled;                   // state.reverseEnabled at save time, 0.0 or 1.0
    float    reverseDirectMix;                 // state.reverseDirectMix at save time, 0.0 or 1.0
    uint32_t valid;                            // USER_PRESET_VALID, else load from presets.toml
};

struct UserPresets {
    uint32_t   firmwareHash;                   // firmwareImageHash() of the image that saved these
    UserPreset presets[kMaxPresets];

    // Required by PersistentStorage: decides whether Save() erases and writes.
    bool operator!=(const UserPresets& a) const { return memcmp(this, &a, sizeof *this) != 0; }
};

// +4: PersistentStorage prefixes its State word.
static_assert(sizeof(UserPresets) + 4 <= QSPI_SECTOR_BYTES,
              "user presets must fit one QSPI sector");

static bool     gSettingsSavePending = false;  // RAM copy differs from what was last saved
static uint32_t gSettingsChangedAtMs = 0;      // System::GetNow() of the last change

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

    // Once audio runs, only the callback calls Update(): Led::Update() is a
    // read-modify-write and would race with the main loop.
    Led led1;
    Led led2;
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
static CloudSeed::ReverbController* reverb = nullptr;
static CloudSeed::ReverseDelay reverseDelay;

// Persistent Storage Declaration. Using type Settings and passed the device's qspi handle
static PersistentStorage<Settings> SavedSettings(hw.seed.qspi);

// User preset edits, one slot per preset (see UserPresets).
static PersistentStorage<UserPresets> SavedUserPresets(hw.seed.qspi);

// FS1 5 s hold: the callback snapshots the engine into gSaveSnapshot on the next
// block the preset-change gate is open, then publishes it with saveSnapshotReady.
static bool       gSaveRequested = false;  // audio callback only
static UserPreset gSaveSnapshot;           // callback writes while !saveSnapshotReady;
                                           // main loop reads while it is set

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
    const UserPreset& u      = SavedUserPresets.GetSettings().presets[presetIndex];
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

// FNV-1a over the loaded code + read-only data. Any change to code or presets.toml
// changes it, so saved edits never outlive the firmware that wrote them.
static uint32_t firmwareImageHash() {
    uint32_t h = 2166136261u;
    for (const uint32_t* p = _stext; p < _etext; ++p) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

static void initUserPresets() {
    const uint32_t hash     = firmwareImageHash();
    UserPresets    defaults = {};
    defaults.firmwareHash   = hash;
    SavedUserPresets.Init(defaults, USER_PRESETS_QSPI_OFFSET);
    // Edits written by a different firmware image are discarded (one sector erase).
    if (SavedUserPresets.GetSettings().firmwareHash != hash)
        SavedUserPresets.RestoreDefaults();
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

static bool     gConfirmActive  = false;  // main loop only
static uint32_t gConfirmStartMs = 0;

// Main loop only: starts the save/restore confirmation blink.
static void startConfirmBlink() {
    gConfirmActive  = true;
    gConfirmStartMs = System::GetNow();
}

// Drives LED1+LED2 together through CONFIRM_BLINKS on/off cycles, shown even when
// bypassed. Returns false once idle; on completion restores LED1 to the bypass state
// and hands LED2 back to updateBlinkState(), which restarts the preset pattern.
static bool updateConfirmBlink() {
    if (!gConfirmActive)
        return false;
    const uint32_t step = (System::GetNow() - gConfirmStartMs) / CONFIRM_BLINK_MS;
    if (step >= 2u * CONFIRM_BLINKS) {
        gConfirmActive = false;
        state.led1.Set(state.bypass ? 0.0f : 1.0f);
        state.led2.Set(0.0f);
        led2BlinkState.active = false;
        return false;
    }
    const float level = (step % 2u == 0u) ? 1.0f : 0.0f;
    state.led1.Set(level);
    state.led2.Set(level);
    return true;
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
        state.led1.Set(state.bypass ? 0.0f : 1.0f);
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
    // playback). Init() clears the buffer; loadSettings() applies the preset's
    // reverse.delay window.
    reverseDelay.Init(reverseDelayBuffer, REVERSE_BUFFER_SIZE, 0);
    state.samplesPerMs = sampleRate / 1000.0f;

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

    // User preset edits (wiped if a different firmware image saved them). Must
    // precede loadSettings(), whose loadPreset() reads them.
    initUserPresets();

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
        // Save and restore precede the preset change below: a snapshot pending across
        // the blocking flash write belongs to the preset loaded when it was taken.
        if (state.saveSnapshotReady) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
            SavedUserPresets.GetSettings().presets[state.currentPreset] = gSaveSnapshot;
            std::atomic_signal_fence(std::memory_order_seq_cst);
            state.saveSnapshotReady = false;
            SavedUserPresets.Save();  // blocking QSPI erase+write; audio keeps running
            startConfirmBlink();
        }
        if (state.triggerPresetRestore) {
            state.triggerPresetRestore = false;
            SavedUserPresets.GetSettings().presets[state.currentPreset] = UserPreset{};
            loadPresetGated(state.currentPreset);  // factory sound now; also drops unsaved knob tweaks
            SavedUserPresets.Save();                // no erase if the slot was already empty
            startConfirmBlink();
        }

        // Preset changes run here, not in the audio callback. The callback passes
        // audio through while presetChangeInProgress is set, so it never sees a
        // half-loaded preset; loadPreset() is synchronous, so the gate reopens as soon
        // as it returns.
        if (state.triggerPresetChange) {
            state.triggerPresetChange = false;
            loadPresetGated((state.currentPreset + 1) % gPresets.count);
            startBlinkSequence(state.blinkPattern);
        }

        serviceSettingsSave();

        // LED1+LED2 save/restore confirmation, else the LED2 preset pattern
        if (!updateConfirmBlink())
            updateBlinkState();

        keepCoreBusy();
    }
}
