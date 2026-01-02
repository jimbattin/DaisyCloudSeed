// Modified version of DaisyCloudSeed by Keith Bloemer.
// Intended for the Terrarrium guitar pedal hardware.
// Code has been modified for mono processing (originally stereo) to 
// allow up to 5 delay lines on the Daisy Seed.

#include "daisy_petal.h"
#include "daisysp.h"
#include "terrarium.h"

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
constexpr int MAX_PRESET_INDEX = 7;
constexpr int NUM_SWITCHES = 4;
constexpr float FLOAT_EPSILON = 1e-6f;

// Switch indices for delay line control
static const int DELAY_LINE_SWITCHES[NUM_SWITCHES] = {
    Terrarium::SWITCH_1,
    Terrarium::SWITCH_2,
    Terrarium::SWITCH_3,
    Terrarium::SWITCH_4
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
    Led led1;
    Led led2;
};

// Declare a local daisy_petal for hardware access
DaisyPetal hw;
PedalState state;
CloudSeed::ReverbController* reverb = nullptr;
  
// This is used in the modified CloudSeed code for allocating 
// delay line memory to SDRAM (64MB available on Daisy)
#define CUSTOM_POOL_SIZE (48*1024*1024)
DSY_SDRAM_BSS char custom_pool[CUSTOM_POOL_SIZE];
size_t pool_index = 0;
int allocation_count = 0;
void* custom_pool_allocate(size_t size)
{
        if (pool_index + size >= CUSTOM_POOL_SIZE)
        {
                return 0;
        }
        void* ptr = &custom_pool[pool_index];
        pool_index += size;
        return ptr;
}

void cyclePreset()
{
    state.currentPreset++;
    if (state.currentPreset > MAX_PRESET_INDEX) {
        state.currentPreset = 0;
    }

    reverb->ClearBuffers();

    switch (state.currentPreset) {
        case 0:
            reverb->initFactoryChorus();
            break;
        case 1:
            reverb->initFactoryDullEchos();
            break;
        case 2:
            reverb->initFactoryHyperplane();
            break;
        case 3:
            reverb->initFactoryMediumSpace();
            break;
        case 4:
            reverb->initFactoryNoiseInTheHallway();
            break;
        case 5:
            reverb->initFactoryRubiKaFields();
            break;
        case 6:
            reverb->initFactorySmallRoom();
            break;
        case 7:
            reverb->initFactory90sAreBack();
            break;
        // case 8:
        //     reverb->initFactoryThroughTheLookingGlass(); // Only preset that sounds scratchy (using 4-5 delay lines, mono) causes buffer underruns
        //                                                   //   TODO Try slight modifications to this preset to allow to work
        default:
            break;
    }
}


// Audio buffers (moved outside callback to avoid repeated allocation)
static float audioInputBuffer[AUDIO_BUFFER_SIZE];
static float audioOutputBuffer[AUDIO_BUFFER_SIZE];

// Helper function to check if float values differ significantly
inline bool hasChanged(float prev, float current) {
    return (prev < current - FLOAT_EPSILON) || (prev > current + FLOAT_EPSILON);
}

// This runs at a fixed rate, to prepare audio samples
static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    hw.ProcessAnalogControls();
    hw.ProcessDigitalControls();
    state.led1.Update();
    state.led2.Update();

    // Process all parameter values
    const float dryout_value = state.dryOut.Process();
    const float earlyout_value = state.earlyOut.Process();
    const float mainout_value = state.mainOut.Process();
    const float time_value = state.delayTime.Process();
    const float diffusion_value = state.diffusion.Process();
    const float tap_decay_value = state.tapDecay.Process();

    // Update reverb parameters only when changed
    if (hasChanged(state.prevDryOut, dryout_value)) {
        reverb->SetParameter(::Parameter::DryOut, dryout_value);
        state.prevDryOut = dryout_value;
    }

    if (hasChanged(state.prevEarlyOut, earlyout_value)) {
        reverb->SetParameter(::Parameter::EarlyOut, earlyout_value);
        state.prevEarlyOut = earlyout_value;
    }

    if (hasChanged(state.prevMainOut, mainout_value)) {
        reverb->SetParameter(::Parameter::MainOut, mainout_value);
        state.prevMainOut = mainout_value;
    }

    if (hasChanged(state.prevDelayTime, time_value)) {
        reverb->SetParameter(::Parameter::LineDecay, time_value);
        state.prevDelayTime = time_value;
    }

    if (hasChanged(state.prevDiffusion, diffusion_value)) {
        reverb->SetParameter(::Parameter::LateDiffusionFeedback, diffusion_value);
        state.prevDiffusion = diffusion_value;
    }

    if (hasChanged(state.prevTapDecay, tap_decay_value)) {
        reverb->SetParameter(::Parameter::TapDecay, tap_decay_value);
        state.prevTapDecay = tap_decay_value;
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

    if (hasChanged(state.prevNumDelayLines, numDelayLines)) {
        // TODO: Determine if ClearBuffers() is needed when changing delay line count
        reverb->SetParameter(::Parameter::LineCount, numDelayLines);
        state.prevNumDelayLines = numDelayLines;
    }

    // Copy input to buffer
    for (size_t i = 0; i < size; i++) {
        audioInputBuffer[i] = in[0][i];
    }

    // (De-)Activate bypass and toggle LED when left footswitch is pressed
    if (hw.switches[Terrarium::FOOTSWITCH_1].RisingEdge()) {
        state.bypass = !state.bypass;
        state.led1.Set(state.bypass ? 0.0f : 1.0f);
    }

    // Cycle available models
    if (hw.switches[Terrarium::FOOTSWITCH_2].RisingEdge()) {
        cyclePreset();
    }

    if (!state.bypass) {
        reverb->Process(audioInputBuffer, audioOutputBuffer, AUDIO_BUFFER_SIZE);
        for (size_t i = 0; i < size; i++) {
            out[0][i] = audioOutputBuffer[i] * OUTPUT_VOLUME_BOOST;
        }
    } else {
        for (size_t i = 0; i < size; i++) {
            out[0][i] = in[0][i];
        }
    }
}

int main(void)
{
    hw.Init();
    const float samplerate = hw.AudioSampleRate();

    // Initialize audio processing libraries
    AudioLib::ValueTables::Init();
    CloudSeed::FastSin::Init();

    // Initialize reverb controller
    reverb = new CloudSeed::ReverbController(samplerate);
    reverb->ClearBuffers();
    reverb->initFactoryChorus();

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
    state.currentPreset = 0;
    state.bypass = true;

    // Initialize LEDs
    state.led1.Init(hw.seed.GetPin(Terrarium::LED_1), false);
    state.led1.Update();

    state.led2.Init(hw.seed.GetPin(Terrarium::LED_2), false);
    state.led2.Update();

    // Start audio processing
    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    while (1) {
        System::Delay(10);
    }
}
