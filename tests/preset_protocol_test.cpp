// Host test for the USB-MIDI preset protocol: SysEx framing from the USB stream,
// USB-MIDI packing of replies, and the upload/read/revert state machine, plus an
// end-to-end run through the real preset parser (fixture path in argv[1]).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "check.h"
#include "preset_bank.h"
#include "preset_protocol.h"

typedef std::vector<uint8_t> Bytes;

static bool gValid = true;
static bool stubValidator(const char*, uint32_t, char* err, int errLen) {
    if (gValid) return true;
    snprintf(err, errLen, "preset 1: stub error");
    return false;
}

static void appendU14(Bytes& b, uint32_t v) { b.push_back(v & 0x7F); b.push_back((v >> 7) & 0x7F); }
static void appendU21(Bytes& b, uint32_t v) { appendU14(b, v); b.push_back((v >> 14) & 0x7F); }
static void appendU32(Bytes& b, uint32_t v) {
    appendU21(b, v); b.push_back((v >> 21) & 0x7F); b.push_back((v >> 28) & 0x0F);
}
static uint32_t readU14(const uint8_t* p) { return p[0] | p[1] << 7; }
static uint32_t readU21(const uint8_t* p) { return readU14(p) | p[2] << 14; }
static uint32_t readU32(const uint8_t* p) {
    return readU21(p) | (uint32_t)p[3] << 21 | (uint32_t)p[4] << 28;
}

// Frame body between F0 and F7.
static Bytes frame(uint8_t cmd) { return Bytes{0x7D, 0x43, 0x53, cmd}; }

// Every reply is one well-formed SysEx message of ours: F0 7D 43 53 <cmd|0x40> <status>
// ... F7, at most kMaxReply bytes, with every byte in between below 0x80.
static void checkReplyWellFormed(const PresetProtocol& p) {
    const size_t n = p.ReplyLength();
    if (n == 0) return;
    const uint8_t* r = p.Reply();
    bool ok = n >= 7 && n <= PresetProtocol::kMaxReply && r[0] == 0xF0
              && r[1] == kSysExManufacturer && r[2] == kSysExTag0 && r[3] == kSysExTag1
              && (r[4] & 0x40) != 0 && r[n - 1] == 0xF7;
    for (size_t i = 1; ok && i + 1 < n; ++i) ok = r[i] < 0x80;
    CHECK(ok);
}

static std::string defaultActiveText() {
    std::string s;
    for (int i = 0; i < 1000; ++i) s.push_back(static_cast<char>('a' + i % 26));
    return s;
}

struct Fixture {
    PresetProtocol proto;
    std::vector<char> rx = std::vector<char>(PresetProtocol::kMaxTextBytes);
    std::string activeText;
    explicit Fixture(std::string text = defaultActiveText(), BankValidator v = stubValidator,
                     uint32_t hash = 0, bool useHash = false)
        : activeText(text) {
        ActiveBank active = {activeText.c_str(), (uint32_t)activeText.size(),
                             useHash ? hash : Fnv1a32(activeText.data(), (uint32_t)activeText.size()),
                             true, 7};
        proto.Init(rx.data(), v, active);
    }
    UploadAction send(const Bytes& f, uint32_t now = 0) {
        const UploadAction a = proto.Handle(f.data(), f.size(), now);
        checkReplyWellFormed(proto);
        return a;
    }
    void finish(UploadAction a, bool ok) { proto.Finish(a, ok); checkReplyWellFormed(proto); }
    // Reply cmd and status; -1 when no reply.
    int replyCmd() const { return proto.ReplyLength() ? proto.Reply()[4] : -1; }
    int status() const { return proto.ReplyLength() ? proto.Reply()[5] : -1; }
    UploadAction begin(uint32_t length, uint32_t hash, uint32_t now = 0) {
        Bytes f = frame(0x02); appendU21(f, length); appendU32(f, hash); return send(f, now);
    }
    UploadAction data(uint32_t seq, const std::string& chunk, uint32_t now = 0) {
        Bytes f = frame(0x03); appendU14(f, seq); f.insert(f.end(), chunk.begin(), chunk.end());
        return send(f, now);
    }
    // BEGIN, every chunk, COMMIT. Returns COMMIT's action; stops early on a non-Ok reply.
    UploadAction upload(const std::string& text, uint32_t chunkBytes = PresetProtocol::kChunkBytes) {
        begin((uint32_t)text.size(), Fnv1a32(text.data(), (uint32_t)text.size()));
        if (status() != 0) return UploadAction::None;
        uint32_t seq = 0;
        for (size_t off = 0; off < text.size(); off += chunkBytes, ++seq) {
            data(seq, text.substr(off, chunkBytes));
            if (status() != 0) return UploadAction::None;
        }
        return send(frame(0x04));
    }
};

static std::string makeText(size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) s.push_back(static_cast<char>(' ' + i % 90));
    return s;
}

static void testAssembler() {
    SysExAssembler a;
    const uint8_t p1[] = {0xF0, 0x7D, 0x43}, p2[] = {0xF8, 0x53, 0x01}, p3[] = {0xF7};
    a.Feed(p1, sizeof p1);
    CHECK(!a.Ready());
    a.Feed(p2, sizeof p2);  // 0xF8 (clock) interleaved mid-frame is ignored
    a.Feed(p3, sizeof p3);
    CHECK(a.Ready());
    CHECK(a.Length() == 4);
    CHECK(memcmp(a.Frame(), "\x7D\x43\x53\x01", 4) == 0);

    // A frame arriving while the slot is busy is dropped entirely.
    const uint8_t busy[] = {0xF0, 0x11, 0x22, 0xF7};
    a.Feed(busy, sizeof busy);
    CHECK(a.Ready() && a.Length() == 4 && a.Frame()[0] == 0x7D);
    a.Release();
    CHECK(!a.Ready());
    const uint8_t next[] = {0xF0, 0x01, 0x02, 0xF7};
    a.Feed(next, sizeof next);
    CHECK(a.Ready() && a.Length() == 2 && a.Frame()[1] == 0x02);
    a.Release();

    // 257 data bytes overflow the slot: never ready.
    Bytes big(1, 0xF0);
    big.insert(big.end(), SysExAssembler::kMaxFrame + 1, 0x11);
    big.push_back(0xF7);
    a.Feed(big.data(), big.size());
    CHECK(!a.Ready());
    // Exactly kMaxFrame bytes fit.
    big.erase(big.begin() + 1);
    a.Feed(big.data(), big.size());
    CHECK(a.Ready() && a.Length() == SysExAssembler::kMaxFrame);
    a.Release();

    // A stray status byte (note on) aborts the frame; its F7 then completes nothing.
    const uint8_t stray[] = {0xF0, 0x01, 0x90, 0x40, 0x7F, 0x02, 0xF7};
    a.Feed(stray, sizeof stray);
    CHECK(!a.Ready());
}

static void testPacker() {
    uint8_t out[16];
    const uint8_t m3[] = {0xF0, 0x01, 0xF7};
    CHECK(PackSysExUsbMidi(m3, 3, out, sizeof out) == 4);
    CHECK(memcmp(out, "\x07\xF0\x01\xF7", 4) == 0);

    const uint8_t m4[] = {0xF0, 0x01, 0x02, 0xF7};
    CHECK(PackSysExUsbMidi(m4, 4, out, sizeof out) == 8);
    CHECK(memcmp(out, "\x04\xF0\x01\x02\x05\xF7\x00\x00", 8) == 0);

    const uint8_t m5[] = {0xF0, 0x01, 0x02, 0x03, 0xF7};
    CHECK(PackSysExUsbMidi(m5, 5, out, sizeof out) == 8);
    CHECK(memcmp(out, "\x04\xF0\x01\x02\x06\x03\xF7\x00", 8) == 0);

    const uint8_t m6[] = {0xF0, 0x01, 0x02, 0x03, 0x04, 0xF7};
    CHECK(PackSysExUsbMidi(m6, 6, out, sizeof out) == 8);
    CHECK(memcmp(out, "\x04\xF0\x01\x02\x07\x03\x04\xF7", 8) == 0);

    CHECK(PackSysExUsbMidi(m6, 6, out, 7) == 0);  // no room for the second packet
}

static void testInfo() {
    Fixture fx;
    fx.send(frame(0x01));
    const uint8_t* r = fx.proto.Reply();
    CHECK(fx.proto.ReplyLength() == 23);
    CHECK(fx.replyCmd() == 0x41 && fx.status() == 0);
    CHECK(r[6] == kProtocolVersion);
    CHECK(readU21(r + 7) == PresetProtocol::kMaxTextBytes);
    CHECK(readU14(r + 10) == PresetProtocol::kChunkBytes);
    CHECK(r[12] == 1);  // uploaded
    CHECK(readU21(r + 13) == 1000);
    CHECK(readU32(r + 16) == Fnv1a32(fx.activeText.data(), 1000));
    CHECK(r[21] == 7);
    CHECK(r[22] == 0xF7);
}

static void testUpload() {
    Fixture fx;
    fx.begin(0, 0);
    CHECK(fx.status() == (int)UploadStatus::BadLength && !fx.proto.SessionActive());
    fx.begin(PresetProtocol::kMaxTextBytes + 1, 0);
    CHECK(fx.status() == (int)UploadStatus::BadLength && !fx.proto.SessionActive());

    const std::string text = makeText(500);
    const uint32_t hash = Fnv1a32(text.data(), 500);
    fx.begin(500, hash);
    CHECK(fx.replyCmd() == 0x42 && fx.status() == 0 && fx.proto.SessionActive());
    fx.data(0, text.substr(0, 240));
    CHECK(fx.replyCmd() == 0x43 && fx.status() == 0 && readU14(fx.proto.Reply() + 6) == 0);
    fx.data(0, text.substr(0, 240));  // duplicate after a lost ACK: re-ACKed, not appended
    CHECK(fx.status() == 0 && fx.proto.SessionActive());
    fx.data(1, text.substr(240, 240));
    fx.data(2, text.substr(480, 20));
    CHECK(fx.status() == 0 && readU14(fx.proto.Reply() + 6) == 2);
    CHECK(fx.send(frame(0x04)) == UploadAction::Commit);
    CHECK(fx.proto.ReplyLength() == 0 && !fx.proto.SessionActive());
    CHECK(fx.proto.ReceivedLength() == 500 && fx.proto.ReceivedHash() == hash);
    CHECK(memcmp(fx.proto.ReceivedText(), text.data(), 500) == 0);
    fx.finish(UploadAction::Commit, true);
    CHECK(fx.replyCmd() == 0x44 && fx.status() == 0 && fx.proto.ReplyLength() == 7);
    fx.finish(UploadAction::Commit, false);
    CHECK(fx.replyCmd() == 0x44 && fx.status() == (int)UploadStatus::FlashError);
}

static void testUploadErrors() {
    const std::string text = makeText(500);
    const uint32_t hash = Fnv1a32(text.data(), 500);

    Fixture gap;  // seq gap ends the session
    gap.begin(500, hash);
    gap.data(1, text.substr(0, 240));
    CHECK(gap.status() == (int)UploadStatus::BadSeq && !gap.proto.SessionActive());
    gap.data(0, text.substr(0, 240));
    CHECK(gap.status() == (int)UploadStatus::NoSession);

    Fixture over;  // more bytes than BEGIN declared
    over.begin(300, hash);
    over.data(0, text.substr(0, 240));
    over.data(1, text.substr(240, 240));
    CHECK(over.status() == (int)UploadStatus::BadLength && !over.proto.SessionActive());

    Fixture shortLen;
    shortLen.begin(500, hash);
    shortLen.data(0, text.substr(0, 240));
    CHECK(shortLen.send(frame(0x04)) == UploadAction::None);
    CHECK(shortLen.replyCmd() == 0x44 && shortLen.status() == (int)UploadStatus::LengthMismatch);
    CHECK(!shortLen.proto.SessionActive());

    Fixture badHash;
    badHash.begin(20, hash);
    badHash.data(0, text.substr(0, 20));
    CHECK(badHash.send(frame(0x04)) == UploadAction::None);
    CHECK(badHash.status() == (int)UploadStatus::HashMismatch);

    Fixture parse;
    gValid = false;
    parse.begin(20, Fnv1a32(text.data(), 20));
    parse.data(0, text.substr(0, 20));
    CHECK(parse.send(frame(0x04)) == UploadAction::None);
    gValid = true;
    CHECK(parse.status() == (int)UploadStatus::ParseError && !parse.proto.SessionActive());
    const char* msg = "preset 1: stub error";
    const size_t msgLen = strlen(msg);
    CHECK(parse.proto.ReplyLength() == 6 + msgLen + 1);
    CHECK(memcmp(parse.proto.Reply() + 6, msg, msgLen) == 0);

    Fixture none;  // COMMIT and DATA without BEGIN
    none.send(frame(0x04));
    CHECK(none.status() == (int)UploadStatus::NoSession);
    none.data(0, "x");
    CHECK(none.status() == (int)UploadStatus::NoSession);
}

static void testRead() {
    Fixture fx;
    Bytes f = frame(0x06); appendU14(f, 0);
    fx.send(f);
    const uint8_t* r = fx.proto.Reply();
    CHECK(fx.replyCmd() == 0x46 && fx.status() == 0);
    CHECK(readU14(r + 6) == 0 && readU21(r + 8) == 1000);
    CHECK(fx.proto.ReplyLength() == 11 + 240 + 1);
    CHECK(memcmp(r + 11, fx.activeText.data(), 240) == 0);

    f = frame(0x06); appendU14(f, 4);  // 1000 = 4 * 240 + 40
    fx.send(f);
    CHECK(fx.status() == 0 && fx.proto.ReplyLength() == 11 + 40 + 1);
    CHECK(memcmp(fx.proto.Reply() + 11, fx.activeText.data() + 960, 40) == 0);

    f = frame(0x06); appendU14(f, 5);
    fx.send(f);
    CHECK(fx.replyCmd() == 0x46 && fx.status() == (int)UploadStatus::BadSeq);
}

static void testSessionLifetime() {
    Fixture fx;
    fx.begin(500, 0, 1000);
    fx.proto.Tick(1000 + 4999);
    CHECK(fx.proto.SessionActive());
    fx.data(0, "abc", 2000);  // accepted DATA restarts the timer
    fx.proto.Tick(2000 + 4999);
    CHECK(fx.proto.SessionActive());
    fx.proto.Tick(2000 + 5000);
    CHECK(!fx.proto.SessionActive());

    // INFO and READ neither need nor refresh a session.
    fx.begin(500, 0, 0);
    fx.send(frame(0x01), 4000);
    fx.proto.Tick(5000);
    CHECK(!fx.proto.SessionActive());

    // Timer arithmetic survives the 32-bit millisecond wrap.
    fx.begin(500, 0, 0xFFFFFF00u);
    fx.proto.Tick(0xFFFFFF00u + 4999);
    CHECK(fx.proto.SessionActive());
    fx.proto.Tick(0xFFFFFF00u + 5000);
    CHECK(!fx.proto.SessionActive());

    fx.begin(500, 0);
    fx.send(frame(0x07));
    CHECK(fx.replyCmd() == 0x47 && fx.status() == 0 && !fx.proto.SessionActive());

    fx.begin(500, 0);
    CHECK(fx.send(frame(0x05)) == UploadAction::Revert);
    CHECK(fx.proto.ReplyLength() == 0 && !fx.proto.SessionActive());
    fx.finish(UploadAction::Revert, true);
    CHECK(fx.replyCmd() == 0x45 && fx.status() == 0);
}

static void testForeignAndMalformed() {
    Fixture fx;
    Bytes foreign{0x7E, 0x43, 0x53, 0x01};
    fx.send(foreign);
    CHECK(fx.proto.ReplyLength() == 0);
    Bytes tooShort{0x7D, 0x43, 0x53};
    fx.send(tooShort);
    CHECK(fx.proto.ReplyLength() == 0);

    fx.send(frame(0x10));
    CHECK(fx.replyCmd() == 0x50 && fx.status() == (int)UploadStatus::BadFrame);

    Bytes infoWithBody = frame(0x01); infoWithBody.push_back(0);
    fx.send(infoWithBody);
    CHECK(fx.status() == (int)UploadStatus::BadFrame);

    // An oversized DATA chunk is a bad frame and ends the session.
    fx.begin(1000, 0);
    fx.data(0, makeText(PresetProtocol::kChunkBytes + 1));
    CHECK(fx.replyCmd() == 0x43 && fx.status() == (int)UploadStatus::BadFrame);
    CHECK(!fx.proto.SessionActive());
}

static void testAssemblerEdges() {
    SysExAssembler a;
    const uint8_t lone[] = {0xF7, 0x01, 0x02};  // F7 and data without an F0
    a.Feed(lone, sizeof lone);
    CHECK(!a.Ready());

    const uint8_t empty[] = {0xF0, 0xF7};  // empty frame: ready, and ignored by Handle
    a.Feed(empty, sizeof empty);
    CHECK(a.Ready() && a.Length() == 0);
    Fixture fx;
    fx.proto.Handle(a.Frame(), a.Length(), 0);
    CHECK(fx.proto.ReplyLength() == 0);
    a.Release();

    const uint8_t restart[] = {0xF0, 0x11, 0x22, 0xF0, 0x33, 0x44, 0xF7};  // a new F0 restarts
    a.Feed(restart, sizeof restart);
    CHECK(a.Ready() && a.Length() == 2 && a.Frame()[0] == 0x33 && a.Frame()[1] == 0x44);
    const uint8_t clock[] = {0xF8, 0xFE};  // real-time bytes while busy leave the frame intact
    a.Feed(clock, sizeof clock);
    CHECK(a.Ready() && a.Length() == 2 && a.Frame()[0] == 0x33);
    a.Release();
}

static void testHashHighBits() {
    // A text whose hash uses bits 28-31, so the fifth u32 byte matters.
    std::string text;
    for (int i = 0; ; ++i) {
        text = "high-bit text " + std::to_string(i);
        if (Fnv1a32(text.data(), (uint32_t)text.size()) >= 0xF0000000u) break;
    }
    const uint32_t hash = Fnv1a32(text.data(), (uint32_t)text.size());
    Fixture fx;
    Bytes f = frame(0x02);
    appendU21(f, (uint32_t)text.size());
    appendU32(f, hash);
    f.back() |= 0x70;  // bits above the fifth byte's low nibble are ignored
    fx.send(f);
    CHECK(fx.status() == 0);
    fx.data(0, text);
    CHECK(fx.send(frame(0x04)) == UploadAction::Commit);
    CHECK(fx.proto.ReceivedHash() == hash);

    Fixture info(defaultActiveText(), stubValidator, 0xFEDCBA98u, true);
    info.send(frame(0x01));
    CHECK(readU32(info.proto.Reply() + 16) == 0xFEDCBA98u);
}

static void testMaxSizeUpload() {
    const std::string text = makeText(PresetProtocol::kMaxTextBytes);  // 409 x 240 + 144
    Fixture fx;
    CHECK(fx.upload(text) == UploadAction::Commit);
    CHECK(fx.proto.ReceivedLength() == PresetProtocol::kMaxTextBytes);
    CHECK(memcmp(fx.proto.ReceivedText(), text.data(), text.size()) == 0);

    // A full 240-byte final chunk would run 96 bytes past the declared maximum.
    Fixture over;
    over.begin(PresetProtocol::kMaxTextBytes, 0);
    uint32_t seq = 0;
    for (; seq < 409; ++seq) over.data(seq, text.substr(seq * 240, 240));
    CHECK(over.status() == 0 && over.proto.SessionActive());
    over.data(seq, makeText(240));
    CHECK(over.status() == (int)UploadStatus::BadLength && !over.proto.SessionActive());
    CHECK(over.proto.ReceivedLength() == 409u * 240u);
}

static void testBeginRestartsSession() {
    const std::string a = makeText(600), b = std::string(300, 'b');
    Fixture fx;
    fx.begin(600, Fnv1a32(a.data(), 600));
    fx.data(0, a.substr(0, 240));
    fx.data(1, a.substr(240, 240));
    CHECK(fx.upload(b) == UploadAction::Commit);  // BEGIN mid-session discards the first
    CHECK(fx.proto.ReceivedLength() == 300);
    CHECK(memcmp(fx.proto.ReceivedText(), b.data(), 300) == 0);
}

static void testSeqEdges() {
    Fixture fx;
    fx.begin(500, 0, 0);
    fx.data(0x3FFF, "abc");  // "one before seq 0" is not a duplicate of anything
    CHECK(fx.status() == (int)UploadStatus::BadSeq && !fx.proto.SessionActive());

    // A re-sent chunk keeps the session alive like a new one.
    fx.begin(500, 0, 0);
    fx.data(0, "abc", 0);
    fx.data(0, "abc", 4000);
    CHECK(fx.status() == 0 && fx.proto.ReceivedLength() == 3);
    fx.proto.Tick(8999);
    CHECK(fx.proto.SessionActive());
    fx.proto.Tick(9000);
    CHECK(!fx.proto.SessionActive());
}

static bool highByteValidator(const char*, uint32_t, char* err, int errLen) {
    snprintf(err, errLen, "bad \xE9 byte");
    return false;
}

static void testSevenBitReplies() {
    // Bytes >= 0x80 in the active text or in a parser message never break the SysEx.
    Fixture fx(std::string("\xC3\xA9 caf\xC3\xA9"), highByteValidator);
    Bytes f = frame(0x06); appendU14(f, 0);
    fx.send(f);
    CHECK(fx.status() == 0 && fx.proto.Reply()[11] == 0x43 && fx.proto.Reply()[12] == 0x29);
    fx.upload("x");
    CHECK(fx.status() == (int)UploadStatus::ParseError);
    CHECK(memcmp(fx.proto.Reply() + 6, "bad i byte", 10) == 0);  // 0xE9 & 0x7F == 'i'
}

static void testFlashFailures() {
    Fixture fx;
    CHECK(fx.upload("abc") == UploadAction::Commit);
    fx.finish(UploadAction::Commit, false);
    CHECK(fx.replyCmd() == 0x44 && fx.status() == (int)UploadStatus::FlashError);
    CHECK(!fx.proto.SessionActive());
    CHECK(fx.send(frame(0x05)) == UploadAction::Revert);
    fx.finish(UploadAction::Revert, false);
    CHECK(fx.replyCmd() == 0x45 && fx.status() == (int)UploadStatus::FlashError);
}

// End to end through the real parser, with every host frame delivered the way the
// pedal receives it: packed into USB-MIDI packets, unwrapped per code index
// (libdaisy/src/hid/usb_midi.cpp:163-195) and fed to the assembler one packet at a time.
static PresetBank gCheckBank;
static bool realValidator(const char* t, uint32_t n, char* err, int errLen) {
    return ParsePresetBankText(t, n, gCheckBank, err, errLen, malloc, free);
}

static void deliver(Fixture& fx, SysExAssembler& as, const Bytes& body) {
    Bytes msg(1, 0xF0);
    msg.insert(msg.end(), body.begin(), body.end());
    msg.push_back(0xF7);
    uint8_t packets[512];
    const size_t n = PackSysExUsbMidi(msg.data(), msg.size(), packets, sizeof packets);
    CHECK(n > 0);
    static const uint8_t kCinSize[16] = {3, 3, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
    for (size_t i = 0; i < n; i += 4)
        as.Feed(packets + i + 1, kCinSize[packets[i] & 0x0F]);
    CHECK(as.Ready());
    if (!as.Ready()) return;
    fx.proto.Handle(as.Frame(), as.Length(), 0);
    checkReplyWellFormed(fx.proto);
    as.Release();
}

static UploadAction uploadOverUsb(Fixture& fx, const std::string& text) {
    SysExAssembler as;
    Bytes f = frame(0x02);
    appendU21(f, (uint32_t)text.size());
    appendU32(f, Fnv1a32(text.data(), (uint32_t)text.size()));
    deliver(fx, as, f);
    CHECK(fx.status() == 0);
    uint32_t seq = 0;
    for (size_t off = 0; off < text.size(); off += PresetProtocol::kChunkBytes, ++seq) {
        f = frame(0x03);
        appendU14(f, seq);
        const std::string chunk = text.substr(off, PresetProtocol::kChunkBytes);
        f.insert(f.end(), chunk.begin(), chunk.end());
        deliver(fx, as, f);
        CHECK(fx.status() == 0);
    }
    f = frame(0x04);
    const UploadAction a = fx.proto.Handle(f.data(), f.size(), 0);
    checkReplyWellFormed(fx.proto);
    return a;
}

static std::string readFile(const char* path) {
    std::string s;
    FILE* fp = fopen(path, "rb");
    if (!fp) return s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) s.append(buf, n);
    fclose(fp);
    return s;
}

static void testEndToEnd(const char* fixturePath) {
    const std::string good = readFile(fixturePath);
    CHECK(!good.empty());

    Fixture fx(defaultActiveText(), realValidator);
    CHECK(uploadOverUsb(fx, good) == UploadAction::Commit);
    CHECK(gCheckBank.count == 2);
    CHECK(fx.proto.ReceivedLength() == good.size());
    CHECK(memcmp(fx.proto.ReceivedText(), good.data(), good.size()) == 0);

    // A bank the parser rejects: COMMIT carries the parser's own message.
    std::string bad = good;
    const size_t at = bad.find("\nblinks = 1\n");
    CHECK(at != std::string::npos);
    bad.replace(at, 12, "\nblinks = 99\n");
    char expected[128];
    PresetBank scratch;
    CHECK(!ParsePresetBankText(bad.data(), (uint32_t)bad.size(), scratch, expected,
                               sizeof expected, malloc, free));
    CHECK(uploadOverUsb(fx, bad) == UploadAction::None);
    CHECK(fx.replyCmd() == 0x44 && fx.status() == (int)UploadStatus::ParseError);
    const size_t len = strlen(expected);
    CHECK(fx.proto.ReplyLength() == 6 + len + 1);
    CHECK(memcmp(fx.proto.Reply() + 6, expected, len) == 0);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s tests/fixtures/two_presets.toml\n", argv[0]);
        return 2;
    }
    CHECK(Fnv1a32("", 0) == 2166136261u);
    CHECK(Fnv1a32("a", 1) == 0xE40C292Cu);  // published FNV-1a test vector
    testAssembler();
    testAssemblerEdges();
    testPacker();
    testInfo();
    testUpload();
    testUploadErrors();
    testRead();
    testSessionLifetime();
    testForeignAndMalformed();
    testHashHighBits();
    testMaxSizeUpload();
    testBeginRestartsSession();
    testSeqEdges();
    testSevenBitReplies();
    testFlashFailures();
    testEndToEnd(argv[1]);
    return CheckSummary("preset_protocol_test");
}
