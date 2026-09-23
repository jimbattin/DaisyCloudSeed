// Parses the embedded presets.toml document into a PresetBank.
//
// Depends only on CloudSeed/Parameter.h, CloudSeed/ParameterNames.h, tomlc99 and
// libc, so the exact same code runs on the host for `make presets-check`.

#include "preset_bank.h"

#include <stdio.h>
#include <string.h>

#include "CloudSeed/ParameterNames.h"
#include "toml.h"

namespace {

#define PARAM(x) (int) Parameter::x

// Signal-flow grouping of the 45 preset-controlled parameters. Must stay in sync
// with GROUPS in tools/gen_presets_toml.py and with the layout of presets.toml.
const int kInputParams[] = {
    PARAM(InputMix), PARAM(PreDelay), PARAM(HiPassEnabled),
    PARAM(HighPass), PARAM(LowPassEnabled), PARAM(LowPass)};

const int kEarlyParams[] = {
    PARAM(TapCount), PARAM(TapLength), PARAM(TapGain), PARAM(TapDecay)};

const int kEarlyDiffusionParams[] = {
    PARAM(DiffusionEnabled), PARAM(DiffusionStages), PARAM(DiffusionDelay),
    PARAM(DiffusionFeedback), PARAM(EarlyDiffusionModAmount),
    PARAM(EarlyDiffusionModRate)};

const int kLateParams[] = {
    PARAM(LineDelay), PARAM(LineDecay), PARAM(LineModAmount),
    PARAM(LineModRate), PARAM(LateStageTap), PARAM(Interpolation)};

const int kLateDiffusionParams[] = {
    PARAM(LateDiffusionEnabled), PARAM(LateDiffusionStages),
    PARAM(LateDiffusionDelay), PARAM(LateDiffusionFeedback),
    PARAM(LateDiffusionModAmount), PARAM(LateDiffusionModRate)};

const int kLateEqParams[] = {
    PARAM(LowShelfEnabled), PARAM(PostLowShelfGain),
    PARAM(PostLowShelfFrequency), PARAM(HighShelfEnabled),
    PARAM(PostHighShelfGain), PARAM(PostHighShelfFrequency),
    PARAM(CutoffEnabled), PARAM(PostCutoffFrequency)};

const int kSeedParams[] = {
    PARAM(TapSeed), PARAM(DiffusionSeed), PARAM(DelaySeed),
    PARAM(PostDiffusionSeed), PARAM(CrossSeed)};

const int kOutputParams[] = {
    PARAM(DryOut), PARAM(PredelayOut), PARAM(EarlyOut), PARAM(MainOut)};

struct ParamGroup {
    const char* name;
    const int*  indices;
    int         count;
};

#define GROUP(name, arr) {name, arr, (int)(sizeof(arr) / sizeof(arr[0]))}

const ParamGroup kGroups[] = {
    GROUP("input", kInputParams),
    GROUP("early", kEarlyParams),
    GROUP("early_diffusion", kEarlyDiffusionParams),
    GROUP("late", kLateParams),
    GROUP("late_diffusion", kLateDiffusionParams),
    GROUP("late_eq", kLateEqParams),
    GROUP("seeds", kSeedParams),
    GROUP("output", kOutputParams),
};

const int kGroupCount = (int)(sizeof(kGroups) / sizeof(kGroups[0]));

// Driven live by SWITCH_1 (line count) and SWITCH_2 (bloom); never from a file.
bool isRuntimeParameter(int index)
{
    return index == PARAM(LineCount) || index == PARAM(isReverse);
}

int findParameter(const char* name)
{
    for (int i = 0; i < (int)Parameter::Count; i++)
    {
        if (strcmp(name, kParameterNames[i]) == 0)
            return i;
    }
    return -1;
}

int groupOfParameter(int paramIndex)
{
    for (int g = 0; g < kGroupCount; g++)
    {
        for (int k = 0; k < kGroups[g].count; k++)
        {
            if (kGroups[g].indices[k] == paramIndex)
                return g;
        }
    }
    return -1;
}

// Accepts both `5.0` and `5`: TOML types them differently, the engine does not.
bool readNumber(const toml_table_t* table, const char* key, double& out)
{
    toml_datum_t asDouble = toml_double_in(table, key);
    if (asDouble.ok)
    {
        out = asDouble.u.d;
        return true;
    }

    toml_datum_t asInt = toml_int_in(table, key);
    if (asInt.ok)
    {
        out = (double)asInt.u.i;
        return true;
    }

    return false;
}

// Rejects any key the schema does not define. Covers scalars, arrays and
// sub-tables: toml_key_in() enumerates them in that order (tomlc99's
// toml_key_in, third_party/tomlc99/toml.c:1865-1877). `context` is prefixed
// into the message; pass "" for the document root.
bool rejectUnknownKeys(const toml_table_t* table, const char* const* allowed,
                       int allowedCount, const char* context, char* err,
                       int errLen)
{
    const int total = toml_table_nkval(table) + toml_table_narr(table)
                      + toml_table_ntab(table);

    for (int i = 0; i < total; i++)
    {
        const char* key = toml_key_in(table, i);
        if (!key)
            continue;

        bool known = false;
        for (int a = 0; a < allowedCount && !known; a++)
            known = strcmp(key, allowed[a]) == 0;

        if (!known)
        {
            snprintf(err, errLen, "%sunknown key '%s'", context, key);
            return false;
        }
    }

    return true;
}

bool readOptionalMs(const toml_table_t* table, const char* key, int index,
                    uint32_t defaultValue, uint32_t& out, char* err, int errLen)
{
    toml_datum_t value = toml_int_in(table, key);
    if (!value.ok)
    {
        out = defaultValue;
        return true;
    }

    if (value.u.i < 0 || value.u.i > 60000)
    {
        snprintf(err, errLen, "preset %d: %s out of range 0..60000", index, key);
        return false;
    }

    out = (uint32_t)value.u.i;
    return true;
}

bool parseParams(const toml_table_t* preset, int index, PresetData& out,
                 char* err, int errLen)
{
    const toml_table_t* params = toml_table_in(preset, "params");
    if (!params)
    {
        snprintf(err, errLen, "preset %d: missing [preset.params]", index);
        return false;
    }

    // Rejects unknown groups, and also a parameter or array written directly
    // under [preset.params] instead of inside one of the eight groups.
    const char* allowedGroups[kGroupCount];
    for (int g = 0; g < kGroupCount; g++)
        allowedGroups[g] = kGroups[g].name;

    char context[48];
    snprintf(context, sizeof context, "preset %d: [preset.params]: ", index);
    if (!rejectUnknownKeys(params, allowedGroups, kGroupCount, context, err,
                           errLen))
        return false;

    for (int i = 0; i < (int)Parameter::Count; i++)
        out.params[i] = 0.0f;

    bool seen[(int)Parameter::Count];
    for (int i = 0; i < (int)Parameter::Count; i++)
        seen[i] = false;

    for (int g = 0; g < kGroupCount; g++)
    {
        const toml_table_t* group = toml_table_in(params, kGroups[g].name);
        if (!group)
        {
            snprintf(err, errLen, "preset %d: missing [preset.params.%s]", index,
                     kGroups[g].name);
            return false;
        }

        // The keyCount walk below only sees scalars, so a nested table or an
        // array inside a group would otherwise vanish silently.
        if (toml_table_narr(group) != 0 || toml_table_ntab(group) != 0)
        {
            snprintf(err, errLen,
                     "preset %d: group '%s' must contain only parameter values",
                     index, kGroups[g].name);
            return false;
        }

        const int keyCount = toml_table_nkval(group);
        for (int k = 0; k < keyCount; k++)
        {
            const char* key = toml_key_in(group, k);
            if (!key)
                continue;

            const int paramIndex = findParameter(key);
            if (paramIndex < 0)
            {
                snprintf(err, errLen, "preset %d: unknown parameter '%s'", index,
                         key);
                return false;
            }

            if (isRuntimeParameter(paramIndex))
            {
                snprintf(err, errLen,
                         "preset %d: '%s' is runtime-controlled and must not "
                         "appear",
                         index, key);
                return false;
            }

            const int owner = groupOfParameter(paramIndex);
            if (owner != g)
            {
                snprintf(err, errLen,
                         "preset %d: parameter '%s' belongs in group '%s', "
                         "found in '%s'",
                         index, key, kGroups[owner].name, kGroups[g].name);
                return false;
            }

            double value = 0.0;
            if (!readNumber(group, key, value))
            {
                snprintf(err, errLen, "preset %d: '%s' is not a number", index,
                         key);
                return false;
            }

            out.params[paramIndex] = (float)value;
            seen[paramIndex]       = true;
        }
    }

    for (int i = 0; i < (int)Parameter::Count; i++)
    {
        if (!isRuntimeParameter(i) && !seen[i])
        {
            snprintf(err, errLen, "preset %d: missing parameter '%s'", index,
                     kParameterNames[i]);
            return false;
        }
    }

    return true;
}

const char* const kKnobKeys[kKnobBanks][kKnobCount] = {
    {"knob1_a", "knob2_a", "knob3_a", "knob4_a", "knob5_a", "knob6_a"},
    {"knob1_b", "knob2_b", "knob3_b", "knob4_b", "knob5_b", "knob6_b"}};

// Resolves "group.Param" (or the literal "reverse.delay") into a KnobTarget.
// `key` is the knobN_x key name, used only for error messages.
bool parseKnobTarget(const char* text, const char* key, int index,
                     KnobTarget& out, char* err, int errLen)
{
    const char* dot = strchr(text, '.');
    if (!dot || dot == text || dot[1] == '\0')
    {
        snprintf(err, errLen,
                 "preset %d: knob_map: %s: '%s' must be \"group.Parameter\"",
                 index, key, text);
        return false;
    }

    const size_t groupLen = (size_t)(dot - text);
    const char*  param    = dot + 1;

    if (groupLen == 7 && strncmp(text, "reverse", 7) == 0)
    {
        if (strcmp(param, "delay") != 0)
        {
            snprintf(err, errLen,
                     "preset %d: knob_map: %s: 'reverse' has only 'delay'",
                     index, key);
            return false;
        }
        out.kind       = KnobTarget_ReverseDelay;
        out.paramIndex = 0;
        return true;
    }

    int group = -1;
    for (int g = 0; g < kGroupCount; g++)
    {
        if (strlen(kGroups[g].name) == groupLen
            && strncmp(text, kGroups[g].name, groupLen) == 0)
        {
            group = g;
            break;
        }
    }
    if (group < 0)
    {
        snprintf(err, errLen, "preset %d: knob_map: %s: unknown group in '%s'",
                 index, key, text);
        return false;
    }

    const int paramIndex = findParameter(param);
    if (paramIndex < 0)
    {
        snprintf(err, errLen, "preset %d: knob_map: %s: unknown parameter '%s'",
                 index, key, param);
        return false;
    }
    if (isRuntimeParameter(paramIndex))
    {
        snprintf(err, errLen,
                 "preset %d: knob_map: %s: '%s' is runtime-controlled and "
                 "cannot be mapped",
                 index, key, param);
        return false;
    }
    if (groupOfParameter(paramIndex) != group)
    {
        snprintf(err, errLen,
                 "preset %d: knob_map: %s: '%s' is not in group '%s'", index,
                 key, param, kGroups[group].name);
        return false;
    }

    out.kind       = KnobTarget_Param;
    out.paramIndex = (uint8_t)paramIndex;
    return true;
}

bool parseKnobMap(const toml_table_t* preset, int index, PresetData& out,
                  char* err, int errLen, void (*dealloc)(void*))
{
    const toml_table_t* map = toml_table_in(preset, "knob_map");
    if (!map)
    {
        snprintf(err, errLen, "preset %d: missing [preset.knob_map]", index);
        return false;
    }

    const char* allowed[kKnobBanks * kKnobCount];
    for (int b = 0; b < kKnobBanks; b++)
        for (int k = 0; k < kKnobCount; k++)
            allowed[b * kKnobCount + k] = kKnobKeys[b][k];

    char context[48];
    snprintf(context, sizeof context, "preset %d: [preset.knob_map]: ", index);
    if (!rejectUnknownKeys(map, allowed, kKnobBanks * kKnobCount, context, err,
                           errLen))
        return false;

    for (int b = 0; b < kKnobBanks; b++)
    {
        for (int k = 0; k < kKnobCount; k++)
        {
            toml_datum_t value = toml_string_in(map, kKnobKeys[b][k]);
            if (!value.ok)
            {
                snprintf(err, errLen,
                         "preset %d: knob_map: missing or non-string '%s'",
                         index, kKnobKeys[b][k]);
                return false;
            }

            const bool ok = parseKnobTarget(value.u.s, kKnobKeys[b][k], index,
                                            out.knobMap[b][k], err, errLen);
            dealloc(value.u.s);
            if (!ok)
                return false;
        }
    }

    return true;
}

bool parsePreset(const toml_table_t* preset, int index, PresetData& out,
                 char* err, int errLen, void (*dealloc)(void*))
{
    static const char* const kPresetKeys[] = {
        "name", "blinks", "led_on_ms", "led_off_ms", "led_pause_ms",
        "max_delay_lines", "params", "knob_map"};

    char context[32];
    snprintf(context, sizeof context, "preset %d: ", index);
    if (!rejectUnknownKeys(preset, kPresetKeys, 8, context, err, errLen))
        return false;

    toml_datum_t name = toml_string_in(preset, "name");
    if (!name.ok)
    {
        snprintf(err, errLen, "preset %d: missing name", index);
        return false;
    }

    const bool emptyName = name.u.s[0] == '\0';
    if (!emptyName)
    {
        strncpy(out.name, name.u.s, kMaxPresetNameLen - 1);
        out.name[kMaxPresetNameLen - 1] = '\0';
    }
    dealloc(name.u.s);

    if (emptyName)
    {
        snprintf(err, errLen, "preset %d: empty name", index);
        return false;
    }

    toml_datum_t blinks = toml_int_in(preset, "blinks");
    if (!blinks.ok || blinks.u.i < 1 || blinks.u.i > 20)
    {
        snprintf(err, errLen, "preset %d: blinks out of range 1..20", index);
        return false;
    }
    out.blinks = (int)blinks.u.i;

    if (!readOptionalMs(preset, "led_on_ms", index, 150, out.onDurationMs, err,
                        errLen))
        return false;
    if (!readOptionalMs(preset, "led_off_ms", index, 150, out.offDurationMs, err,
                        errLen))
        return false;
    if (!readOptionalMs(preset, "led_pause_ms", index, 5000, out.pauseAfterMs,
                        err, errLen))
        return false;

    double maxDelayLines = 0.0;
    if (!readNumber(preset, "max_delay_lines", maxDelayLines)
        || (float)maxDelayLines < 1.0f || (float)maxDelayLines > 5.0f)
    {
        snprintf(err, errLen, "preset %d: max_delay_lines out of range 1..5",
                 index);
        return false;
    }
    out.maxDelayLines = (float)maxDelayLines;

    if (!parseParams(preset, index, out, err, errLen))
        return false;

    return parseKnobMap(preset, index, out, err, errLen, dealloc);
}

bool parseRoot(const toml_table_t* root, PresetBank& bank, char* err, int errLen,
               void (*dealloc)(void*))
{
    const toml_array_t* presets = toml_array_in(root, "preset");
    if (!presets)
    {
        snprintf(err, errLen, "missing [[preset]] array");
        return false;
    }

    const int count = toml_array_nelem(presets);
    if (count < 1 || count > kMaxPresets)
    {
        snprintf(err, errLen, "preset count %d out of range 1..%d", count,
                 kMaxPresets);
        return false;
    }

    static const char* const kRootKeys[] = {"preset"};
    if (!rejectUnknownKeys(root, kRootKeys, 1, "", err, errLen))
        return false;

    for (int i = 0; i < count; i++)
    {
        const toml_table_t* preset = toml_table_at(presets, i);
        if (!preset)
        {
            snprintf(err, errLen, "preset %d: not a table", i);
            return false;
        }

        if (!parsePreset(preset, i, bank.presets[i], err, errLen, dealloc))
            return false;
    }

    bank.count = count;
    return true;
}

}  // namespace

bool ParsePresetBank(char* toml, PresetBank& bank, char* err, int errLen,
                     void* (*alloc)(size_t), void (*dealloc)(void*))
{
    bank.count = 0;
    if (errLen > 0)
        err[0] = '\0';

    toml_set_memutil(alloc, dealloc);

    char          parseErr[160];
    toml_table_t* root = toml_parse(toml, parseErr, sizeof parseErr);
    if (!root)
    {
        snprintf(err, errLen, "toml: %s", parseErr);
        return false;
    }

    const bool ok = parseRoot(root, bank, err, errLen, dealloc);
    toml_free(root);

    if (!ok)
        bank.count = 0;

    return ok;
}
