#ifndef PRESET_BANK_H
#define PRESET_BANK_H

#include <stddef.h>
#include <stdint.h>

#include "CloudSeed/Parameter.h"

constexpr int kMaxPresets       = 16;
constexpr int kMaxPresetNameLen = 32;

constexpr int kKnobCount = 6;   // knob1..knob6
constexpr int kControlBanks = 2;  // 0 = primary ("_a"), 1 = secondary ("_b")
constexpr int kToggleCount  = 4;  // toggle1..toggle4 = SWITCH_1..SWITCH_4

// What a knob writes to. Param targets index PresetData::params (i.e.
// (int)Parameter); ReverseDelay is the reverse-delay window length, which is not
// a reverb parameter.
enum KnobTargetKind : uint8_t { KnobTarget_Param = 0, KnobTarget_ReverseDelay = 1 };

struct KnobTarget {
    KnobTargetKind kind;
    uint8_t paramIndex;  // valid only when kind == KnobTarget_Param
};

// What a toggle switch writes to: on (lever up) = 1.0, off = 0.0. Param targets
// are the on/off Parameters accepted by the parser (kToggleParams); the others
// are pedal functions with no Parameter slot, stored as 0.0/1.0 (on when >= 0.5).
enum ToggleTargetKind : uint8_t {
    ToggleTarget_Param            = 0,
    ToggleTarget_DelayLinesMax    = 1,  // "delay_lines.max": off = default_delay_lines, on = max_delay_lines
    ToggleTarget_ReverseEnabled   = 2,  // "reverse.enabled": reverse voice on/off
    ToggleTarget_ReverseDirectMix = 3,  // "reverse.direct_mix": off = into reverb, on = direct mix
};

struct ToggleTarget {
    ToggleTargetKind kind;
    uint8_t paramIndex;  // valid only when kind == ToggleTarget_Param
};

struct PresetData {
    char     name[kMaxPresetNameLen];
    int      blinks;
    uint32_t onDurationMs;
    uint32_t offDurationMs;
    uint32_t pauseAfterMs;
    float    defaultDelayLines;  // lines while "delay_lines.max" is off; whole number 1..maxDelayLines
    float    maxDelayLines;
    float    params[(int)Parameter::Count];
    // [preset.params.reverse] delay: normalized reverse-window length (knob target "reverse.delay")
    float    reverseDelay;
    // Stored state of the toggle pseudo-targets, 0..1, on when >= 0.5:
    // [preset.params.delay_lines] max, [preset.params.reverse] enabled / direct_mix
    float    delayLinesMax;
    float    reverseEnabled;
    float    reverseDirectMix;
    // [bank][knob]; bank 0 = primary, bank 1 = secondary (preset footswitch (FS2) held)
    KnobTarget knobMap[kControlBanks][kKnobCount];
    // [bank][toggle]; bank 0 = primary, bank 1 = secondary (preset footswitch (FS2) held)
    ToggleTarget toggleMap[kControlBanks][kToggleCount];
};

struct PresetBank {
    int        count;
    PresetData presets[kMaxPresets];
};

// Parses `toml` (NUL-terminated, MUTATED in place) into `bank`. The text must be
// plain ASCII: any byte >= 0x80 is rejected before parsing. Every parser
// allocation goes through alloc/dealloc and is released before this returns.
// Returns true on success; on failure sets bank.count = 0 and writes a
// NUL-terminated message into err.
bool ParsePresetBank(char* toml, PresetBank& bank, char* err, int errLen,
                     void* (*alloc)(size_t), void (*dealloc)(void*));

// Parses `length` bytes of preset TOML that need not be NUL-terminated (the embedded
// blob, memory-mapped QSPI, a USB upload buffer) without modifying them: rejects
// empty text and embedded NUL bytes, then runs ParsePresetBank() on a NUL-terminated
// scratch copy that is the first allocation through `alloc`. Same result contract
// as ParsePresetBank().
bool ParsePresetBankText(const char* text, uint32_t length, PresetBank& bank, char* err,
                         int errLen, void* (*alloc)(size_t), void (*dealloc)(void*));

#endif
