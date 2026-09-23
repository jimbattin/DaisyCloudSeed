// Host-side proof that presets.toml parses and yields the expected values.
// Usage: preset_check [--validate] <presets.toml>
// Without --validate, prints the same dump tools/gen_presets_toml.py writes to
// build/presets_expected.txt, so the two can be diffed.
// With --validate, prints nothing but a one-line summary and exits non-zero if
// the firmware's own parser would reject the file.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CloudSeed/ParameterNames.h"
#include "preset_bank.h"

static char* readFile(const char* path)
{
    FILE* fp = fopen(path, "rb");
    if (!fp)
    {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    const long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0)
    {
        fprintf(stderr, "cannot size %s\n", path);
        fclose(fp);
        return NULL;
    }

    char* buffer = (char*)malloc((size_t)size + 1);
    if (!buffer)
    {
        fprintf(stderr, "out of memory reading %s\n", path);
        fclose(fp);
        return NULL;
    }

    const size_t got = fread(buffer, 1, (size_t)size, fp);
    fclose(fp);
    buffer[got] = '\0';
    return buffer;
}

// Mirror of the firmware's boot-time parse arena (TOML_ARENA_SIZE and
// toml_arena_alloc, cloudseed.cpp:193-204): same size, same 8-byte alignment,
// no reuse on free. Host pointers are 64-bit, so tomlc99's node allocations are
// at least as large here as on the 32-bit target: fitting here implies fitting
// on the pedal.
static const size_t kArenaSize = 512 * 1024;
static char         gArena[kArenaSize];
static size_t       gArenaIndex = 0;
static size_t       gArenaPeak  = 0;

static void* arenaAlloc(size_t size)
{
    const size_t aligned = (size + 7u) & ~(size_t)7u;
    if (gArenaIndex + aligned > kArenaSize)
    {
        fprintf(stderr,
                "boot parse arena exhausted: needs more than %zu bytes "
                "(TOML_ARENA_SIZE, cloudseed.cpp)\n",
                kArenaSize);
        exit(1);
    }

    void* ptr = &gArena[gArenaIndex];
    gArenaIndex += aligned;
    if (gArenaIndex > gArenaPeak)
        gArenaPeak = gArenaIndex;
    return ptr;
}

static void arenaFree(void*) {}

int main(int argc, char** argv)
{
    bool        validateOnly = false;
    const char* path         = NULL;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--validate") == 0)
            validateOnly = true;
        else if (!path)
            path = argv[i];
        else
            path = NULL;
    }

    if (!path)
    {
        fprintf(stderr, "usage: %s [--validate] <presets.toml>\n", argv[0]);
        return 2;
    }

    char* text = readFile(path);
    if (!text)
        return 2;

    // Same shape as loadPresetBank(): a scratch copy of the NUL-terminated blob
    // is the arena's first allocation, because toml_parse() mutates its input.
    const size_t blobLen = strlen(text) + 1;
    char*        scratch = (char*)arenaAlloc(blobLen);
    memcpy(scratch, text, blobLen);
    free(text);

    PresetBank bank;
    char       err[192];
    if (!ParsePresetBank(scratch, bank, err, sizeof err, arenaAlloc, arenaFree))
    {
        fprintf(stderr, "%s: %s\n", path, err);
        return 1;
    }

    if (validateOnly)
    {
        printf("%s: %d presets valid, boot arena peak %zu of %zu bytes\n", path,
               bank.count, gArenaPeak, kArenaSize);
        return 0;
    }

    for (int p = 0; p < bank.count; p++)
    {
        const PresetData& preset = bank.presets[p];
        printf("preset %d name=\"%s\" blinks=%d on=%u off=%u pause=%u "
               "max_delay_lines=%.6g\n",
               p, preset.name, preset.blinks, (unsigned)preset.onDurationMs,
               (unsigned)preset.offDurationMs, (unsigned)preset.pauseAfterMs,
               (double)preset.maxDelayLines);

        for (int i = 0; i < (int)Parameter::Count; i++)
        {
            if (i == (int)Parameter::LineCount || i == (int)Parameter::isReverse)
                continue;
            printf("  %s = %.9g\n", kParameterNames[i], (double)preset.params[i]);
        }
    }

    return 0;
}
