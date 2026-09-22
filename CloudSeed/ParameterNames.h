#ifndef PARAMETERNAMES
#define PARAMETERNAMES

#include "Parameter.h"

// Indexed by (int)Parameter; order must match Parameter.h exactly.
static const char* const kParameterNames[(int)Parameter::Count] = {
    "InputMix", "PreDelay", "HighPass", "LowPass",
    "TapCount", "TapLength", "TapGain", "TapDecay", "isReverse",
    "DiffusionEnabled", "DiffusionStages", "DiffusionDelay", "DiffusionFeedback",
    "LineCount", "LineDelay", "LineDecay",
    "LateDiffusionEnabled", "LateDiffusionStages", "LateDiffusionDelay", "LateDiffusionFeedback",
    "PostLowShelfGain", "PostLowShelfFrequency", "PostHighShelfGain", "PostHighShelfFrequency", "PostCutoffFrequency",
    "EarlyDiffusionModAmount", "EarlyDiffusionModRate", "LineModAmount", "LineModRate",
    "LateDiffusionModAmount", "LateDiffusionModRate",
    "TapSeed", "DiffusionSeed", "DelaySeed", "PostDiffusionSeed",
    "CrossSeed", "DryOut", "PredelayOut", "EarlyOut", "MainOut",
    "HiPassEnabled", "LowPassEnabled", "LowShelfEnabled", "HighShelfEnabled", "CutoffEnabled", "LateStageTap",
    "Interpolation"
};

#endif
