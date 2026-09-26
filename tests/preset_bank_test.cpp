// Host tests for ParsePresetBank (src/preset_bank.cpp) against a fixture that is
// independent of the live presets.toml: the baseline parses, and each mutation is
// rejected (or accepted) with the expected message.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "check.h"
#include "preset_bank.h"

// Replaces the nth occurrence of `from`, counting from the first [[preset]] on so
// the fixture's header comment is never touched. False if not found.
static bool Mutate(std::string& t, const char* from, const char* to, int nth = 1) {
    size_t pos = t.find("\n[[preset]]");
    if (pos == std::string::npos)
        return false;
    for (int i = 0; i < nth; i++) {
        pos = t.find(from, i == 0 ? pos : pos + 1);
        if (pos == std::string::npos)
            return false;
    }
    t.replace(pos, strlen(from), to);
    return true;
}

static PresetBank gBank;  // 4.8 KB

static bool Parse(std::string t, PresetBank& b, std::string& err) {
    std::vector<char> buf(t.begin(), t.end());
    buf.push_back('\0');
    char errBuf[256] = {};
    const bool ok = ParsePresetBank(buf.data(), b, errBuf, sizeof errBuf, malloc, free);
    err = errBuf;
    return ok;
}

static std::string ReadFile(const char* path) {
    std::string out;
    FILE* fp = fopen(path, "rb");
    if (!fp)
        return out;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, fp)) > 0)
        out.append(chunk, n);
    fclose(fp);
    return out;
}

struct Reject {
    const char* from;
    const char* to;
    int         nth;
    const char* expected;
};

static const char kToggleMapBlock[] =
    "[preset.toggle_map]\n"
    "toggle1_a = \"delay_lines.max\"\n"
    "toggle2_a = \"early.isReverse\"\n"
    "toggle3_a = \"reverse.enabled\"\n"
    "toggle4_a = \"reverse.direct_mix\"\n"
    "toggle1_b = \"delay_lines.max\"\n"
    "toggle2_b = \"early.isReverse\"\n"
    "toggle3_b = \"reverse.enabled\"\n"
    "toggle4_b = \"reverse.direct_mix\"\n";

static const Reject kRejects[] = {
    {"toggle1_a = \"delay_lines.max\"", "toggle1_a = \"output.DryOut\"", 1,
     "toggle_map: toggle1_a: 'DryOut' is not an on/off parameter"},
    {"toggle2_b = \"early.isReverse\"", "toggle2_b = \"reverse.delay\"", 1,
     "'reverse' toggles only 'enabled' or 'direct_mix'"},
    {"toggle1_a = \"delay_lines.max\"", "toggle1_a = \"late.LineCount\"", 1,
     "runtime-controlled"},
    {"toggle3_a = \"reverse.enabled\"", "toggle3_a = \"late_eq.HiPassEnabled\"", 1,
     "is not in group 'late_eq'"},
    {"toggle1_a = \"delay_lines.max\"", "toggle1_a = \"delay_lines.min\"", 1,
     "'delay_lines' has only 'max'"},
    {"toggle1_a = \"delay_lines.max\"", "toggle1_a = \"nogroup\"", 1,
     "must be \"group.Parameter\""},
    {"toggle4_b = \"reverse.direct_mix\"\n", "", 1,
     "toggle_map: missing or non-string 'toggle4_b'"},
    {"toggle4_b = \"reverse.direct_mix\"\n",
     "toggle4_b = \"reverse.direct_mix\"\ntoggle5_a = \"early.isReverse\"\n", 1,
     "[preset.toggle_map]: unknown key 'toggle5_a'"},
    {kToggleMapBlock, "", 1, "missing [preset.toggle_map]"},
    {"max = 0.0", "max = 2.0", 1, "'delay_lines.max' = 2 out of range 0..1"},
    {"default_delay_lines = 2.0", "default_delay_lines = 2.5", 1,
     "preset 0: default_delay_lines must be a whole number 1..5"},
    {"default_delay_lines = 2.0", "default_delay_lines = 0.0", 1,
     "preset 0: default_delay_lines must be a whole number 1..5"},
    {"default_delay_lines = 2.0\n", "", 1,
     "preset 0: default_delay_lines must be a whole number 1..5"},
    {"default_delay_lines = 2.0", "default_delay_lines = 5.0", 2,
     "preset 1: default_delay_lines exceeds max_delay_lines"},
    {"max_delay_lines = 5.0", "max_delay_lines = 5.5", 1,
     "preset 0: max_delay_lines must be a whole number 1..5"},
    {"enabled    = 0.0", "enabled    = nan", 1, "'reverse.enabled' = nan out of range 0..1"},
    {"isReverse = 0.0\n", "", 1, "missing parameter 'isReverse'"},
    {"knob1_a = \"output.DryOut\"", "knob1_a = \"reverse.enabled\"", 1,
     "'reverse' has only 'delay'"},
};

struct Accept {
    const char* from;
    const char* to;
};

static const Accept kAccepts[] = {
    {"toggle1_a = \"delay_lines.max\"", "toggle1_a = \"early_diffusion.DiffusionStages\""},
    {"toggle2_a = \"early.isReverse\"", "toggle2_a = \"late.Interpolation\""},
};

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s tests/fixtures/two_presets.toml\n", argv[0]);
        return 2;
    }
    const std::string base = ReadFile(argv[1]);
    CHECK(!base.empty());

    std::string err;
    CHECK(Parse(base, gBank, err));
    CHECK(gBank.count == 2);
    CHECK(gBank.presets[1].maxDelayLines == 4.0f);
    CHECK(gBank.presets[0].toggleMap[0][0].kind == ToggleTarget_DelayLinesMax);
    CHECK(gBank.presets[0].knobMap[1][5].kind == KnobTarget_ReverseDelay);

    for (const Reject& r : kRejects) {
        std::string t = base;
        CHECK(Mutate(t, r.from, r.to, r.nth));
        const bool ok = Parse(t, gBank, err);
        CHECK(!ok);
        CHECK(err.find(r.expected) != std::string::npos);
        if (ok || err.find(r.expected) == std::string::npos)
            fprintf(stderr, "  expected '%s', got '%s'\n", r.expected, err.c_str());
    }

    for (const Accept& a : kAccepts) {
        std::string t = base;
        CHECK(Mutate(t, a.from, a.to));
        const bool ok = Parse(t, gBank, err);
        CHECK(ok);
        if (!ok)
            fprintf(stderr, "  unexpected rejection: '%s'\n", err.c_str());
    }

    return CheckSummary("preset_bank_test");
}
