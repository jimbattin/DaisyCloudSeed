#include <stdio.h>
#include <string.h>

#include "daisy_seed.h"
#include "sdram_pool.h"
#include "pedal_leds.h"

// Presets are defined in presets.toml, embedded into the firmware image by
// presets_toml.s and parsed once at boot by LoadEmbeddedPresetBank().
extern "C" {
    extern const char     presets_toml[];
    extern const uint32_t presets_toml_len;  // includes the terminating NUL
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
static size_t pool_index = 0;

// Bump allocator, no free. Returns 8-byte aligned blocks. Declared extern by the
// CloudSeed headers, so the signature and external linkage must stay as they are.
void* custom_pool_allocate(size_t size) {
    const size_t aligned = alignUp8(size);
    if (aligned > CUSTOM_POOL_SIZE - pool_index)
        FatalErrorLoop();  // callers placement-new into the result; 0x0 is ITCMRAM on the H750
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

bool LoadEmbeddedPresetBank(PresetBank& bank, char* err, int errLen) {
    toml_arena_index = 0;
    // toml_parse() mutates its input, so parse a scratch copy, never the .rodata blob.
    char* scratch = static_cast<char*>(toml_arena_alloc(presets_toml_len));
    if (!scratch) { snprintf(err, errLen, "arena too small"); return false; }
    memcpy(scratch, presets_toml, presets_toml_len);
    const bool ok = ParsePresetBank(scratch, bank, err, errLen,
                                    toml_arena_alloc, toml_arena_free);
    toml_arena_index = 0;  // release: custom_pool is untouched from here on
    return ok;
}
