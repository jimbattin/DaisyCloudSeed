#ifndef SDRAM_POOL_H
#define SDRAM_POOL_H

#include <stddef.h>
#include <stdint.h>

#include "preset_bank.h"

// 8-byte aligned bump allocation from the 48 MB SDRAM pool; never freed. Exhaustion
// is FatalErrorLoop(). The CloudSeed headers declare this extern with exactly this
// signature, so it keeps external linkage and this name.
void* custom_pool_allocate(size_t size);

// Parses `length` bytes of preset TOML (not NUL-terminated) into `bank`, using a
// dedicated 512 KB SDRAM parse arena. Boot (after hw.Init(): SDRAM usable) or main
// loop only: one arena, not reentrant. On failure writes a message into `err` and
// returns false; `bank` is then unspecified.
bool ParsePresetText(const char* text, uint32_t length, PresetBank& bank, char* err, int errLen);

// The embedded presets.toml (the built-in bank); `length` excludes the terminating NUL.
const char* EmbeddedPresetText(uint32_t& length);

#endif
