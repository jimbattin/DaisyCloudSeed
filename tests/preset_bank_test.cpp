// Host tests for ParsePresetBank / ParsePresetBankText (src/preset_bank.cpp) against a
// fixture that is independent of the live presets.toml: the baseline parses, and each
// mutation is rejected (or accepted) with the expected message.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
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

// malloc-backed allocator that counts live blocks, to prove every allocation is released.
static int gLiveBlocks = 0;
static void* countingAlloc(size_t n) { ++gLiveBlocks; return malloc(n); }
static void countingFree(void* p) { if (p) --gLiveBlocks; free(p); }
static void* failingAlloc(size_t) { return nullptr; }

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
    {"led_on_ms = 150", "led_on_ms = 150.5", 1,
     "preset 0: led_on_ms must be a whole number 0..60000"},
    {"led_on_ms = 150", "led_on_ms = \"150\"", 1,
     "preset 0: led_on_ms must be a whole number 0..60000"},
    {"led_pause_ms = 5000", "led_pause_ms = 60001", 1,
     "preset 0: led_pause_ms must be a whole number 0..60000"},
    {"name = \"Chorus\"", "name = \"0123456789012345678901234567890X\"", 1,
     "preset 0: name longer than 31 bytes"},
    // Non-ASCII anywhere is rejected with its position, in a value or a comment.
    {"name = \"Chorus\"", "name = \"Chor\xC3\xBCs\"", 1, "non-ASCII byte 0xC3"},
    {"[preset.knob_map]", "[preset.knob_map]  # \xE2\x80\x94", 1, "non-ASCII byte 0xE2"},
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

    // A float LED timing is honoured, not silently replaced by the default.
    {
        std::string t = base;
        CHECK(Mutate(t, "led_on_ms = 150", "led_on_ms = 300.0"));
        CHECK(Parse(t, gBank, err));
        CHECK(gBank.presets[0].onDurationMs == 300);
    }

    // The longest name that fits is kept whole.
    {
        const char* longest = "0123456789012345678901234567890";
        std::string t = base;
        CHECK(Mutate(t, "name = \"Chorus\"", "name = \"0123456789012345678901234567890\""));
        CHECK(Parse(t, gBank, err));
        CHECK(strcmp(gBank.presets[0].name, longest) == 0);
    }

    // A non-ASCII byte is reported at its 1-based line and column.
    {
        std::string t = base;
        CHECK(Mutate(t, "name = \"Chorus\"", "name = \"Chor\xC3\xBCs\""));
        const size_t at = t.find('\xC3');
        const size_t lineStart = t.rfind('\n', at) + 1;
        const int line = 1 + (int)std::count(t.begin(), t.begin() + at, '\n');
        const int column = 1 + (int)(at - lineStart);
        char expected[64];
        snprintf(expected, sizeof expected, "line %d, column %d: non-ASCII byte 0xC3", line,
                 column);
        CHECK(!Parse(t, gBank, err));
        CHECK(err == expected);
        if (err != expected)
            fprintf(stderr, "  expected '%s', got '%s'\n", expected, err.c_str());
    }

    // ParsePresetBankText: the entry point for text that is not NUL-terminated (the
    // embedded blob, memory-mapped QSPI, a USB upload buffer).
    {
        char errBuf[192];
        // Exactly `length` bytes are parsed: trailing bytes (no NUL) are never read as TOML.
        std::vector<char> buf(base.begin(), base.end());
        const char trailer[] = "\n[[[not toml";
        buf.insert(buf.end(), trailer, trailer + sizeof trailer - 1);
        const std::vector<char> before = buf;
        gBank.count = -1;
        CHECK(ParsePresetBankText(buf.data(), (uint32_t)base.size(), gBank, errBuf,
                                  sizeof errBuf, countingAlloc, countingFree));
        CHECK(gBank.count == 2);
        CHECK(buf == before);  // the source is never mutated
        CHECK(gLiveBlocks == 0);
        CHECK(!ParsePresetBankText(buf.data(), (uint32_t)buf.size(), gBank, errBuf,
                                   sizeof errBuf, countingAlloc, countingFree));
        CHECK(strncmp(errBuf, "toml:", 5) == 0);
        CHECK(gBank.count == 0);
        CHECK(gLiveBlocks == 0);  // released on failure too

        CHECK(ParsePresetBankText(base.data(), (uint32_t)base.size(), gBank, errBuf,
                                  sizeof errBuf, countingAlloc, countingFree));
        CHECK(!ParsePresetBankText(base.data(), 0, gBank, errBuf, sizeof errBuf,
                                   countingAlloc, countingFree));
        CHECK(strstr(errBuf, "empty") != nullptr);
        CHECK(gBank.count == 0);

        // A NUL byte inside the text: the parser would otherwise stop there silently.
        std::string withNul = base;
        withNul[withNul.find("[[preset]]", 1) - 1] = '\0';
        CHECK(!ParsePresetBankText(withNul.data(), (uint32_t)withNul.size(), gBank, errBuf,
                                   sizeof errBuf, countingAlloc, countingFree));
        CHECK(strstr(errBuf, "NUL byte") != nullptr);

        CHECK(!ParsePresetBankText(base.data(), (uint32_t)base.size(), gBank, errBuf,
                                   sizeof errBuf, failingAlloc, countingFree));
        CHECK(strstr(errBuf, "arena too small") != nullptr);
        CHECK(gLiveBlocks == 0);
    }

    return CheckSummary("preset_bank_test");
}
