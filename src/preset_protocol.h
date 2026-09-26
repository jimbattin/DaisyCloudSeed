#ifndef PRESET_PROTOCOL_H
#define PRESET_PROTOCOL_H

// USB-MIDI SysEx protocol v1 for uploading, reading back and reverting the preset
// bank (presets.toml text). docs/USB_MIDI.md describes it for host authors; this
// file is normative. Host-portable: no libdaisy.
//
// Frame:  F0 7D 43 53 <cmd> <body...> F7    (7D = non-commercial ID, 43 53 = "CS")
// Reply:  F0 7D 43 53 <cmd|0x40> <status> <body...> F7
// Numbers are 7-bit groups, least significant first: u14 = 2 bytes, u21 = 3 bytes,
// u32 = 5 bytes (the fifth carries bits 28-31). Hash: 32-bit FNV-1a over the text.
//
//   0x01 INFO    -> version, maxTextBytes u21, chunkBytes u14, source (0 built-in,
//                   1 uploaded), activeLength u21, activeHash u32, presetCount
//   0x02 BEGIN   length u21, hash u32        starts (or restarts) a session
//   0x03 DATA    seq u14, 1..240 text bytes  -> seq u14; seq == last accepted is re-ACKed
//   0x04 COMMIT  checks length, hash, parser; stores to QSPI and reboots after Ok;
//                ParseError carries the parser's message
//   0x05 REVERT  erases the stored bank and reboots after Ok
//   0x06 READ    index u14 -> index u14, activeLength u21, text[index*240 ..+240)
//   0x07 ABORT   ends the session
//
// Host side: stop-and-wait (one frame in flight); wait up to 10 s for the COMMIT
// reply. After Ok to COMMIT or REVERT the USB port disappears for about 3 s while
// the pedal reboots (bootloader grace window, then the app re-enumerates). A frame
// with a foreign header is ignored without a reply.

#include <stddef.h>
#include <stdint.h>

constexpr uint8_t kSysExManufacturer = 0x7D, kSysExTag0 = 0x43, kSysExTag1 = 0x53;
constexpr uint8_t kProtocolVersion   = 1;

enum class UploadStatus : uint8_t { Ok = 0, BadFrame, NoSession, BadSeq, BadLength,
                                    LengthMismatch, HashMismatch, ParseError, FlashError };
enum class UploadAction { None, Commit, Revert };

// Bytewise 32-bit FNV-1a.
uint32_t Fnv1a32(const void* data, uint32_t length);

// Bytes of one SysEx frame between F0 and F7, filled from the USB interrupt.
// Single slot: bytes arriving while a frame is Ready() are dropped.
class SysExAssembler {
public:
    static constexpr size_t kMaxFrame = 256;
    void Feed(const uint8_t* data, size_t size);  // USB ISR only
    bool Ready() const;                           // main loop
    const uint8_t* Frame() const { return buf_; } // valid while Ready()
    size_t Length() const { return len_; }
    void Release();                               // main loop: slot free for the next frame
private:
    uint8_t buf_[kMaxFrame];
    size_t  len_      = 0;
    bool    inFrame_  = false;
    bool    overflow_ = false;
    volatile bool ready_ = false;
};

// Packs one complete SysEx message (starting F0, ending F7) into USB-MIDI 1.0 event
// packets on cable 0: CIN 0x4 for each full 3-byte group without F7, and a final
// packet with CIN 0x5/0x6/0x7 (1/2/3 bytes) that contains the F7, zero-padded.
// Returns bytes written (a multiple of 4), or 0 if `outCap` is too small or `msg`
// is not F0 ... F7.
size_t PackSysExUsbMidi(const uint8_t* msg, size_t len, uint8_t* out, size_t outCap);

struct ActiveBank {
    const char* text;
    uint32_t    length;
    uint32_t    hash;
    bool        uploaded;
    int         presetCount;
};

// Returns true if `text` is a valid preset bank; otherwise writes a message to `err`.
using BankValidator = bool (*)(const char* text, uint32_t length, char* err, int errLen);

class PresetProtocol {
public:
    static constexpr uint32_t kMaxTextBytes     = 98304;
    static constexpr uint32_t kChunkBytes       = 240;
    static constexpr uint32_t kSessionTimeoutMs = 5000;
    static constexpr size_t   kMaxReply         = 256;  // whole reply incl. F0/F7

    // rxBuffer holds kMaxTextBytes; `active` describes the bank the pedal booted.
    void Init(char* rxBuffer, BankValidator validate, const ActiveBank& active);
    // frame = bytes between F0 and F7. Commit/Revert: the caller performs the flash
    // operation, then calls Finish() to build the reply.
    UploadAction Handle(const uint8_t* frame, size_t len, uint32_t nowMs);
    void Finish(UploadAction action, bool ok);
    void Tick(uint32_t nowMs);  // ends a session idle for kSessionTimeoutMs
    bool SessionActive() const { return session_; }
    const ActiveBank& Active() const { return active_; }  // as passed to Init()
    const uint8_t* Reply() const { return reply_; }
    size_t ReplyLength() const { return replyLen_; }  // 0 = send nothing
    const char* ReceivedText() const { return rx_; }
    uint32_t ReceivedLength() const { return received_; }
    uint32_t ReceivedHash() const { return expectedHash_; }

private:
    void beginReply(uint8_t cmd, UploadStatus status);
    void put(uint8_t b);
    void putU14(uint32_t v);
    void putU21(uint32_t v);
    void putU32(uint32_t v);
    void endReply();
    void reply(uint8_t cmd, UploadStatus status) { beginReply(cmd, status); endReply(); }

    char*         rx_       = nullptr;
    BankValidator validate_ = nullptr;
    ActiveBank    active_   = {};
    bool          session_  = false;
    uint32_t      expectedLength_ = 0;
    uint32_t      expectedHash_   = 0;
    uint32_t      received_       = 0;
    uint32_t      nextSeq_        = 0;
    uint32_t      lastActivityMs_ = 0;
    uint8_t       reply_[kMaxReply];
    size_t        replyLen_ = 0;
};

#endif
