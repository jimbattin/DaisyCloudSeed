#ifndef STORED_BANK_H
#define STORED_BANK_H

// QSPI format of the preset bank uploaded over USB (docs/USB_MIDI.md), and the check
// that decides whether the pedal boots it. PedalStorage does the flash I/O.
// Host-portable: no libdaisy.

#include <stdint.h>

#include "preset_protocol.h"

// The header has its own 4 KB sector, so REVERT erases one sector; the text follows
// it. Everything stays below the 256 KB the Daisy bootloader never touches (its
// program area starts at 0x40000).
constexpr uint32_t STORED_BANK_HEADER_OFFSET = 0x10000;
constexpr uint32_t STORED_BANK_TEXT_OFFSET   = 0x11000;  // up to PresetProtocol::kMaxTextBytes
constexpr uint32_t STORED_BANK_MAGIC         = 0x31425343u;  // "CSB1"
static_assert(STORED_BANK_TEXT_OFFSET + PresetProtocol::kMaxTextBytes <= 0x40000,
              "stored bank must stay below the bootloader's program area");

struct StoredBankHeader {
    uint32_t magic;         // STORED_BANK_MAGIC; erased flash reads 0xFFFFFFFF
    uint32_t length;        // text bytes, no NUL
    uint32_t textHash;      // Fnv1a32() of the text
    uint32_t firmwareHash;  // firmware image hash of the image that stored it
};

// Returns `text` and sets `length` when `header` describes a bank stored by the image
// whose hash is `imageHash`, with a length of 1..kMaxTextBytes and a matching text
// hash; otherwise nullptr. `text` is read only after the header checks pass, and then
// only `header.length` bytes, so an erased or corrupt header never walks past the
// stored-bank region.
inline const char* ValidStoredBankText(const StoredBankHeader& header, const char* text,
                                       uint32_t imageHash, uint32_t& length)
{
    if (header.magic != STORED_BANK_MAGIC || header.firmwareHash != imageHash
        || header.length == 0 || header.length > PresetProtocol::kMaxTextBytes)
        return nullptr;
    if (Fnv1a32(text, header.length) != header.textHash)
        return nullptr;
    length = header.length;
    return text;
}

#endif
