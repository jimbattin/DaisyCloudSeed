// Host-side proof that presets.toml parses and yields the expected values.
// Usage: preset_check <presets.toml>
// Prints the same dump tools/gen_presets_toml.py writes to
// build/presets_expected.txt, so the two can be diffed.

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

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <presets.toml>\n", argv[0]);
        return 2;
    }

    char* text = readFile(argv[1]);
    if (!text)
        return 2;

    PresetBank bank;
    char       err[192];
    const bool ok = ParsePresetBank(text, bank, err, sizeof err, malloc, free);
    free(text);

    if (!ok)
    {
        fprintf(stderr, "%s\n", err);
        return 1;
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
