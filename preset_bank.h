#ifndef PRESET_BANK_H
#define PRESET_BANK_H

#include <stddef.h>
#include <stdint.h>

#include "CloudSeed/Parameter.h"

constexpr int kMaxPresets       = 16;
constexpr int kMaxPresetNameLen = 32;

constexpr int kKnobCount = 6;   // knob1..knob6
constexpr int kKnobBanks = 2;   // 0 = primary ("_a"), 1 = secondary ("_b")

// What a knob writes to. Param targets index PresetData::params (i.e.
// (int)Parameter); ReverseDelay is the reverse-delay window length, which is not
// a reverb parameter.
enum KnobTargetKind : uint8_t { KnobTarget_Param = 0, KnobTarget_ReverseDelay = 1 };

struct KnobTarget {
    KnobTargetKind kind;
    uint8_t paramIndex;  // valid only when kind == KnobTarget_Param
};

struct PresetData {
    char     name[kMaxPresetNameLen];
    int      blinks;
    uint32_t onDurationMs;
    uint32_t offDurationMs;
    uint32_t pauseAfterMs;
    float    maxDelayLines;
    float    params[(int)Parameter::Count];
    // [preset.params.reverse] delay: normalized reverse-window length (knob target "reverse.delay")
    float    reverseDelay;
    // [bank][knob]; bank 0 = primary, bank 1 = secondary (preset footswitch (FS2) held)
    KnobTarget knobMap[kKnobBanks][kKnobCount];
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
