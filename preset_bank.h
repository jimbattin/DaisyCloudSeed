#ifndef PRESET_BANK_H
#define PRESET_BANK_H

#include <stddef.h>
#include <stdint.h>

#include "CloudSeed/Parameter.h"

static const int kMaxPresets       = 16;
static const int kMaxPresetNameLen = 32;

struct PresetData {
    char     name[kMaxPresetNameLen];
    int      blinks;
    uint32_t onDurationMs;
    uint32_t offDurationMs;
    uint32_t pauseAfterMs;
    float    maxDelayLines;
    float    params[(int)Parameter::Count];
};

struct PresetBank {
    int        count;
    PresetData presets[kMaxPresets];
};

// Parses `toml` (NUL-terminated, MUTATED in place) into `bank`. Every parser
// allocation goes through alloc/dealloc and is released before this returns.
// Returns true on success; on failure sets bank.count = 0 and writes a
// NUL-terminated message into err.
bool ParsePresetBank(char* toml, PresetBank& bank, char* err, int errLen,
                     void* (*alloc)(size_t), void (*dealloc)(void*));

#endif
