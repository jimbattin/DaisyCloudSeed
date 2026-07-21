// Modified version of DaisyCloudSeed by Keith Bloemer.
// Intended for the Terrarrium guitar pedal hardware.
// Code has been modified for mono processing (originally stereo) to 
// allow up to 5 delay lines on the Daisy Seed.

#include "daisy_petal.h"
#include "daisysp.h"
#include "terrarium.h"
#include <cmath>
#include <array>

#include "../../CloudSeed/Default.h"
#include "../../CloudSeed/ReverbController.h"
#include "../../CloudSeed/FastSin.h"
#include "../../CloudSeed/AudioLib/ValueTables.h"
#include "../../CloudSeed/AudioLib/MathDefs.h"

using namespace daisy;
using namespace daisysp;
using namespace terrarium;  // This is important for mapping the correct controls to the Daisy Seed on Terrarium PCB

// Constants
constexpr size_t AUDIO_BUFFER_SIZE = 48;
constexpr float OUTPUT_VOLUME_BOOST = 1.2f;
constexpr float MAKEUP_GAIN_STRENGTH = 0.8f;  // Max additional gain when fully wet (0.0-1.0)
constexpr int NUM_SWITCHES = 4;
constexpr float FLOAT_EPSILON = 1e-6f;

#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923  // π/2 for equal-power curves
#endif

// Increment this when changing the settings struct so the software will know
// to reset to defaults if this ever changes.
#define SETTINGS_VERSION 1

// Switch indices for delay line control
static const int DELAY_LINE_SWITCHES[NUM_SWITCHES] = {
    Terrarium::SWITCH_1,
    Terrarium::SWITCH_2,
    Terrarium::SWITCH_3,
    Terrarium::SWITCH_4
};

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

// Preset configuration structure
struct PresetConfig {
    void (CloudSeed::ReverbController::*initFunction)();  // Function pointer to init method
    BlinkPattern blinkPattern;
    const char* name;  // Preset name for reference
    const float max_delay_lines; // Limit the number of lines to prevent skipping/dropped audio
};

// Array of all available presets
// To add/remove presets, simply modify this array - no other code changes needed
const PresetConfig PRESETS[] = {
    {
        .initFunction = &CloudSeed::ReverbController::initFactoryChorus,
        .blinkPattern = {.numBlinks = 1, .onDurationMs = 150, .offDurationMs = 150, .pauseAfterMs = 5000},
        .name = "Chorus",
        .max_delay_lines = 5.0f
    },
    {
        .initFunction = &CloudSeed::ReverbController::initFactoryDullEchos,
        .blinkPattern = {.numBlinks = 2, .onDurationMs = 150, .offDurationMs = 150, .pauseAfterMs = 5000},
        .name = "Dull Echos",
        .max_delay_lines = 5.0f
    },
    {
        .initFunction = &CloudSeed::ReverbController::initFactoryHyperplane,
        .blinkPattern = {.numBlinks = 3, .onDurationMs = 150, .offDurationMs = 150, .pauseAfterMs = 5000},
        .name = "Hyperplane",
        .max_delay_lines = 5.0f
    },
    {
        .initFunction = &CloudSeed::ReverbController::initFactoryMediumSpace,
        .blinkPattern = {.numBlinks = 4, .onDurationMs = 150, .offDurationMs = 150, .pauseAfterMs = 5000},
        .name = "Medium Space",
        .max_delay_lines = 5.0f
    },
    {
        .initFunction = &CloudSeed::ReverbController::initFactoryNoiseInTheHallway,
        .blinkPattern = {.numBlinks = 5, .onDurationMs = 150, .offDurationMs = 150, .pauseAfterMs = 5000},
        .name = "Noise in the Hallway",
        .max_delay_lines = 5.0f
    },
    {
        .initFunction = &CloudSeed::ReverbController::initFactoryRubiKaFields,
        .blinkPattern = {.numBlinks = 6, .onDurationMs = 150, .offDurationMs = 150, .pauseAfterMs = 5000},
        .name = "Rubi Ka Fields",
        .max_delay_lines = 5.0f
    },
    {
        .initFunction = &CloudSeed::ReverbController::initFactorySmallRoom,
        .blinkPattern = {.numBlinks = 7, .onDurationMs = 150, .offDurationMs = 150, .pauseAfterMs = 5000},
        .name = "Small Room",
        .max_delay_lines = 5.0f
    },
    {
        .initFunction = &CloudSeed::ReverbController::initFactory90sAreBack,
        .blinkPattern = {.numBlinks = 8, .onDurationMs = 150, .offDurationMs = 150, .pauseAfterMs = 5000},
        .name = "90s Are Back",
        .max_delay_lines = 5.0f
    },
    {
        // This preset is CPU intensive and starts crackling with more than 2-3 delay lines active
        .initFunction = &CloudSeed::ReverbController::initFactoryThroughTheLookingGlass,
        .blinkPattern = {.numBlinks = 9, .onDurationMs = 150, .offDurationMs = 150, .pauseAfterMs = 5000},
        .name = "Through the Looking Glass",
        .max_delay_lines = 3.0f
    },
    {
        .initFunction = &CloudSeed::ReverbController::initFactoryDarkPlate,
        .blinkPattern = {.numBlinks = 10, .onDurationMs = 150, .offDurationMs = 150, .pauseAfterMs = 5000},
        .name = "Dark Plate",
        .max_delay_lines = 5.0f
    }
};

constexpr int NUM_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);

// Persistent Settings
struct Settings {
    int version;        // Version of the settings struct
    int currentPreset;  // Currently selected preset (0-8)

    // Overloading the != operator
    // This is necessary as this operator is used in the PersistentStorage source code
    bool operator!=(const Settings& a) const {
        return !(
            a.version == version &&
            a.currentPreset == currentPreset
        );
    }
};

// Global state structure
struct PedalState {
    // Parameters
    ::daisy::Parameter dryOut;
    ::daisy::Parameter earlyOut;
    ::daisy::Parameter mainOut;
    ::daisy::Parameter delayTime;
    ::daisy::Parameter diffusion;
    ::daisy::Parameter tapDecay;

    // Previous parameter values (for change detection)
    float prevDryOut;
    float prevEarlyOut;
    float prevMainOut;
    float prevDelayTime;
    float prevDiffusion;
    float prevTapDecay;
    float prevNumDelayLines;

    // State
    bool bypass;
    int currentPreset;
    bool triggerPresetChange;    // Set true in audio callback when preset switch pressed
    bool triggerSettingsSave;    // Set true when settings need to be saved
    bool triggerPresetBlink;     // Set true when we should blink LED to show preset
    bool presetChangeInProgress; // Set true while preset is being changed (prevents audio processing)
    Led led1;
    Led led2;
};

// Declare a local daisy_petal for hardware access
DaisyPetal hw;
PedalState state;
CloudSeed::ReverbController* reverb = nullptr;

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
 * Presets
 */

void loadPreset(int presetIndex) {
    // Validate preset index
    if (presetIndex < 0 || presetIndex >= NUM_PRESETS) {
        presetIndex = 0;  // Default to first preset if invalid
    }

    reverb->ClearBuffers();

    // Call the preset's initialization function using member function pointer
    (reverb->*(PRESETS[presetIndex].initFunction))();
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
    if (state.currentPreset < 0 || state.currentPreset >= NUM_PRESETS) {
        state.currentPreset = 0;
    }

    loadPreset(state.currentPreset);
}

void saveSettings() {
    // Reference to local copy of settings stored in flash
    Settings &localSettings = SavedSettings.GetSettings();

    localSettings.version = SETTINGS_VERSION;
    localSettings.currentPreset = state.currentPreset;

    state.triggerSettingsSave = true;
}

void cyclePreset() {
    state.currentPreset = (state.currentPreset + 1) % NUM_PRESETS;
    loadPreset(state.currentPreset);
}


// Helper function to check if float values differ significantly
inline bool hasChanged(float prev, float current) {
    return (prev < current - FLOAT_EPSILON) || (prev > current + FLOAT_EPSILON);
}

/*
 * LED blink
 */

// Get the blink pattern for a given preset index
BlinkPattern getPresetBlinkPattern(int presetIndex) {
    // Validate preset index
    if (presetIndex < 0 || presetIndex >= NUM_PRESETS) {
        presetIndex = 0;  // Default to first preset if invalid
    }
    return PRESETS[presetIndex].blinkPattern;
}

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
        startBlinkSequence(getPresetBlinkPattern(state.currentPreset));
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

    hw.ProcessAnalogControls();
    hw.ProcessDigitalControls();
    state.led1.Update();
    state.led2.Update();

    //
    // Process footswitches
    //

    // (De-)Activate bypass and toggle LED when left footswitch is pressed
    if (hw.switches[Terrarium::FOOTSWITCH_1].RisingEdge()) {
        state.bypass = !state.bypass;
        state.led1.Set(state.bypass ? 0.0f : 1.0f);
    }

    // Cycle available models (actual preset change happens in main loop)
    if (hw.switches[Terrarium::FOOTSWITCH_2].RisingEdge()) {
        state.triggerPresetChange = true;
    }

    //
    // Process knobs and toggle switches
    //

    // Process all parameter values
    const float dryOutValue = state.dryOut.Process();
    const float earlyOutValue = state.earlyOut.Process();
    const float mainOutValue = state.mainOut.Process();
    const float timeValue = state.delayTime.Process();
    const float diffusionValue = state.diffusion.Process();
    const float tapDecayValue = state.tapDecay.Process();

    // Update reverb parameters only when changed
    if (hasChanged(state.prevDryOut, dryOutValue)) {
        reverb->SetParameter(::Parameter::DryOut, dryOutValue);
        state.prevDryOut = dryOutValue;
    }

    if (hasChanged(state.prevEarlyOut, earlyOutValue)) {
        reverb->SetParameter(::Parameter::EarlyOut, earlyOutValue);
        state.prevEarlyOut = earlyOutValue;
    }

    if (hasChanged(state.prevMainOut, mainOutValue)) {
        reverb->SetParameter(::Parameter::MainOut, mainOutValue);
        state.prevMainOut = mainOutValue;
    }

    if (hasChanged(state.prevDelayTime, timeValue)) {
        reverb->SetParameter(::Parameter::LineDecay, timeValue);
        state.prevDelayTime = timeValue;
    }

    if (hasChanged(state.prevDiffusion, diffusionValue)) {
        reverb->SetParameter(::Parameter::LateDiffusionFeedback, diffusionValue);
        state.prevDiffusion = diffusionValue;
    }

    if (hasChanged(state.prevTapDecay, tapDecayValue)) {
        reverb->SetParameter(::Parameter::TapDecay, tapDecayValue);
        state.prevTapDecay = tapDecayValue;
    }

    // Delay Line Switches
    // The .Pressed() function below counts an 'ON' switch as pressed.
    // Total number of switches on sets how many delay lines are activated (1 - 5)
    float numDelayLines = 1.0f;
    for (int i = 0; i < NUM_SWITCHES; i++) {
        if (hw.switches[DELAY_LINE_SWITCHES[i]].Pressed()) {
            numDelayLines += 1.0f;
        }
    }
    // Do not exceed the preset's limit for delay lines
    // Only needed for "Through the Looking Glass" at the moment
    if (numDelayLines > PRESETS[state.currentPreset].max_delay_lines) {
        numDelayLines = PRESETS[state.currentPreset].max_delay_lines;
    }

    if (hasChanged(state.prevNumDelayLines, numDelayLines)) {
        // TODO: Determine if ClearBuffers() is needed when changing delay line count
        reverb->SetParameter(::Parameter::LineCount, numDelayLines);
        state.prevNumDelayLines = numDelayLines;
    }

    //
    // Process audio
    //

    // Copy input to buffer
    for (size_t i = 0; i < size; i++) {
        audioInputBuffer[i] = in[0][i]; // left channel
    }

    // Apply effect or bypass
    // IMPORTANT: Skip reverb processing if preset change is in progress to avoid race condition
    if (!state.bypass && !state.presetChangeInProgress) {
        reverb->Process(audioInputBuffer, audioOutputBuffer, AUDIO_BUFFER_SIZE);

        // Calculate dynamic makeup gain using equal-power crossfade compensation
        // This maintains perceived loudness as dry/wet balance changes
        float totalSignal = dryOutValue + earlyOutValue + mainOutValue + FLOAT_EPSILON;
        float wetBalance = (earlyOutValue + mainOutValue) / totalSignal;

        // Equal-power compensation curve
        // wetBalance=0 (all dry): sin(0)=0 → no extra gain
        // wetBalance=1 (all wet): sin(π/2)=1 → maximum extra gain
        float compensation = sinf(wetBalance * M_PI_2);
        float makeupGain = OUTPUT_VOLUME_BOOST * (1.0f + compensation * MAKEUP_GAIN_STRENGTH);

        for (size_t i = 0; i < size; i++) {
            out[0][i] = audioOutputBuffer[i] * makeupGain;
        }
    } else {
        for (size_t i = 0; i < size; i++) {
            out[0][i] = in[0][i]; // left channel only
        }
    }
}

/*
 * Main loop
 */

int main(void) {
    hw.Init();
    const float sampleRate = hw.AudioSampleRate();

    // Initialize audio processing libraries
    AudioLib::ValueTables::Init();
    CloudSeed::FastSin::Init();

    // Initialize reverb controller
    reverb = new CloudSeed::ReverbController(sampleRate);
    reverb->ClearBuffers();

    // Initialize parameters
    state.dryOut.Init(hw.knob[Terrarium::KNOB_1], 0.0f, 1.0f, ::daisy::Parameter::LINEAR);
    state.earlyOut.Init(hw.knob[Terrarium::KNOB_2], 0.0f, 1.0f, ::daisy::Parameter::LINEAR);
    state.mainOut.Init(hw.knob[Terrarium::KNOB_3], 0.0f, 1.0f, ::daisy::Parameter::LINEAR);
    state.diffusion.Init(hw.knob[Terrarium::KNOB_4], 0.0f, 1.0f, ::daisy::Parameter::LINEAR);
    state.tapDecay.Init(hw.knob[Terrarium::KNOB_5], 0.0f, 1.0f, ::daisy::Parameter::LINEAR);
    state.delayTime.Init(hw.knob[Terrarium::KNOB_6], 0.0f, 1.0f, ::daisy::Parameter::LINEAR);

    // Initialize previous parameter values
    state.prevDryOut = 0.0f;
    state.prevEarlyOut = 0.0f;
    state.prevMainOut = 0.0f;
    state.prevDelayTime = 0.0f;
    state.prevDiffusion = 0.0f;
    state.prevTapDecay = 0.0f;
    state.prevNumDelayLines = 1.0f; // Start with 1 delay line (no switches pressed)

    // Initialize state
    state.bypass = true;
    state.triggerPresetChange = false;
    state.triggerSettingsSave = false;
    state.triggerPresetBlink = false;
    state.presetChangeInProgress = false;

    // Initialize LEDs
    state.led1.Init(hw.seed.GetPin(Terrarium::LED_1), false);
    state.led1.Update();

    state.led2.Init(hw.seed.GetPin(Terrarium::LED_2), false);
    state.led2.Update();

    // Initialize persistent storage with default settings
    Settings defaultSettings = {
        SETTINGS_VERSION,  // version
        0                  // currentPreset (default to Chorus preset)
    };
    SavedSettings.Init(defaultSettings);

    // Load settings from persistent storage (with resilience to failures)
    loadSettings();

    // Start audio processing
    hw.StartAdc();
    hw.StartAudio(audioCallback);

    while (true) {
        // Handle preset changes (moved from audio callback for better performance)
        if (state.triggerPresetChange) {
            state.triggerPresetChange = false;

            // Set flag to prevent audio processing during preset change
            // This prevents race condition where audio callback tries to process
            // while buffers are being cleared/modified
            state.presetChangeInProgress = true;

            cyclePreset();
            saveSettings();
            startBlinkSequence(getPresetBlinkPattern(state.currentPreset));

            // Small delay to ensure all buffers are fully initialized
            // before audio processing resumes
            System::Delay(10);

            state.presetChangeInProgress = false; // Re-enable audio processing
        }

        // Handle settings save (moved from audio callback for better performance)
        if (state.triggerSettingsSave) {
            SavedSettings.Save();  // Write locally stored settings to the external flash
            state.triggerSettingsSave = false;
        }

        // Update LED blink state machine
        updateBlinkState();

        System::Delay(10);
    }
}
