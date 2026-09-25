// Host-side check that presets.toml is accepted by the firmware's own parser.
// Usage: preset_check --validate|--print-knob-map <presets.toml>
// --validate        prints a one-line summary; exits 1 (with the parser's error
//                   message on stderr) if the pedal would reject the file.
// --print-knob-map  prints the resolved knob assignments of every preset.

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
// toml_arena_alloc, cloudseed.cpp:216-225): same size, same 8-byte alignment,
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
    enum Mode
    {
        Mode_None,
        Mode_Validate,
        Mode_PrintKnobMap
    } mode           = Mode_None;
    const char* path = NULL;
    bool        bad  = false;

    for (int i = 1; i < argc; i++)
    {
        Mode flag = Mode_None;
        if (strcmp(argv[i], "--validate") == 0)
            flag = Mode_Validate;
        else if (strcmp(argv[i], "--print-knob-map") == 0)
            flag = Mode_PrintKnobMap;

        if (flag != Mode_None)
        {
            if (mode != Mode_None)
                bad = true;
            mode = flag;
        }
        else if (!path)
            path = argv[i];
        else
            bad = true;
    }

    if (bad || mode == Mode_None || !path)
    {
        fprintf(stderr, "usage: %s --validate|--print-knob-map <presets.toml>\n",
                argv[0]);
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

    if (mode == Mode_PrintKnobMap)
    {
        static const char* const kBankSuffix[2] = {"a", "b"};
        for (int p = 0; p < bank.count; p++)
        {
            for (int b = 0; b < kKnobBanks; b++)
            {
                for (int k = 0; k < kKnobCount; k++)
                {
                    const KnobTarget& t = bank.presets[p].knobMap[b][k];
                    printf("preset %d knob%d_%s = %s\n", p, k + 1, kBankSuffix[b],
                           t.kind == KnobTarget_ReverseDelay
                               ? "reverse.delay"
                               : kParameterNames[t.paramIndex]);
                }
            }
        }
        return 0;
    }

    printf("%s: %d presets valid, boot arena peak %zu of %zu bytes\n", path,
           bank.count, gArenaPeak, kArenaSize);
    return 0;
}
