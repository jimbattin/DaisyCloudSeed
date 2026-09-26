#include <stdio.h>
#include <string.h>

#include "daisy_seed.h"
#include "sdram_pool.h"
#include "pedal_leds.h"

// The built-in preset bank: presets.toml, embedded into the firmware image by
// presets_toml.s (see EmbeddedPresetText()).
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
 * TOML parse arena (boot and USB upload validation)
 */

// A dedicated SDRAM buffer, not part of custom_pool: after boot the reverb owns the
// pool, and a USB upload is validated by a full parse at runtime. Costs 512 KB of
// SDRAM. Every parse starts from an empty arena and releases it on return. The heap
// is deliberately avoided: libnosys' _sbrk grows unchecked from end = 0x30008000
// into the 256 KB RAM_D2 region.
constexpr size_t TOML_ARENA_SIZE = 512 * 1024;
DSY_SDRAM_BSS __attribute__((aligned(8))) static char toml_arena[TOML_ARENA_SIZE];
static size_t toml_arena_index = 0;

static void* toml_arena_alloc(size_t size) {
    const size_t aligned = alignUp8(size);
    if (toml_arena_index + aligned > TOML_ARENA_SIZE) return nullptr;
    void* ptr = &toml_arena[toml_arena_index];
    toml_arena_index += aligned;
    return ptr;
}

static void toml_arena_free(void*) {}

bool ParsePresetText(const char* text, uint32_t length, PresetBank& bank, char* err, int errLen) {
    if (length == 0) { snprintf(err, errLen, "empty preset text"); return false; }
    if (memchr(text, 0, length)) { snprintf(err, errLen, "NUL byte in preset text"); return false; }
    toml_arena_index = 0;
    // toml_parse() mutates its input and needs a terminating NUL, so parse a scratch
    // copy, never the source (.rodata blob, QSPI mapping or upload buffer).
    char* scratch = static_cast<char*>(toml_arena_alloc(length + 1));
    if (!scratch) { snprintf(err, errLen, "arena too small"); return false; }
    memcpy(scratch, text, length);
    scratch[length] = '\0';
    const bool ok = ParsePresetBank(scratch, bank, err, errLen,
                                    toml_arena_alloc, toml_arena_free);
    toml_arena_index = 0;
    return ok;
}

const char* EmbeddedPresetText(uint32_t& length) {
    length = presets_toml_len - 1;
    return presets_toml;
}
