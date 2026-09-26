#ifndef SDRAM_POOL_H
#define SDRAM_POOL_H

#include <stddef.h>

#include "preset_bank.h"

// 8-byte aligned bump allocation from the 48 MB SDRAM pool; never freed. Exhaustion
// is FatalErrorLoop(). The CloudSeed headers declare this extern with exactly this
// signature, so it keeps external linkage and this name.
void* custom_pool_allocate(size_t size);

// Boot only, after hw.Init() (SDRAM usable) and before the first
// custom_pool_allocate(): parses the embedded presets.toml into `bank` from a
// scratch arena carved from the head of the pool, then abandons the arena. On
// failure writes a message into `err` and returns false.
bool LoadEmbeddedPresetBank(PresetBank& bank, char* err, int errLen);

#endif
