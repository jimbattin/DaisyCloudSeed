// Host test for ValidStoredBankText (src/stored_bank.h): which stored bank header and
// text the pedal accepts at boot, instead of falling back to the embedded presets.toml.
#include <string.h>
#include <vector>

#include "check.h"
#include "stored_bank.h"

static const uint32_t kImage = 0x12345678u;

static StoredBankHeader headerFor(const std::vector<char>& text) {
    return StoredBankHeader{STORED_BANK_MAGIC, (uint32_t)text.size(),
                            Fnv1a32(text.data(), (uint32_t)text.size()), kImage};
}

static bool accepted(const StoredBankHeader& h, const std::vector<char>& text) {
    uint32_t length = 0xDEADu;
    const char* p = ValidStoredBankText(h, text.data(), kImage, length);
    if (p) {
        CHECK(p == text.data());
        CHECK(length == h.length);
    } else {
        CHECK(length == 0xDEADu);  // untouched on rejection
    }
    return p != nullptr;
}

int main() {
    std::vector<char> text(PresetProtocol::kMaxTextBytes + 16, 'x');
    for (size_t i = 0; i < text.size(); ++i) text[i] = static_cast<char>(' ' + i % 90);

    std::vector<char> small(text.begin(), text.begin() + 1000);
    StoredBankHeader h = headerFor(small);
    CHECK(accepted(h, small));

    // Erased flash reads all ones: never a bank.
    StoredBankHeader erased;
    memset(&erased, 0xFF, sizeof erased);
    CHECK(!accepted(erased, small));

    StoredBankHeader bad = h;
    bad.magic ^= 1;
    CHECK(!accepted(bad, small));

    // Stored by a different firmware image: discarded on reflash.
    bad = h;
    bad.firmwareHash = kImage + 1;
    CHECK(!accepted(bad, small));

    // Text corrupted after the header was written (or the header from an older upload).
    std::vector<char> corrupt = small;
    corrupt[500] ^= 1;
    CHECK(!accepted(h, corrupt));
    bad = h;
    bad.textHash ^= 0x80000000u;
    CHECK(!accepted(bad, small));

    // Length bounds: 1 .. kMaxTextBytes. A header claiming more is rejected before the
    // text is hashed, so it can never read past the stored-bank region.
    std::vector<char> one(text.begin(), text.begin() + 1);
    CHECK(accepted(headerFor(one), one));
    std::vector<char> max(text.begin(), text.begin() + PresetProtocol::kMaxTextBytes);
    CHECK(accepted(headerFor(max), max));
    std::vector<char> over(text.begin(), text.begin() + PresetProtocol::kMaxTextBytes + 1);
    CHECK(!accepted(headerFor(over), over));
    bad = h;
    bad.length = 0;
    bad.textHash = Fnv1a32(small.data(), 0);
    CHECK(!accepted(bad, small));

    // The header's length is what is hashed and returned, not the buffer's.
    bad = h;
    bad.length = 999;
    CHECK(!accepted(bad, small));  // hash covers 1000 bytes
    bad.textHash = Fnv1a32(small.data(), 999);
    CHECK(accepted(bad, small));

    return CheckSummary("stored_bank_test");
}
