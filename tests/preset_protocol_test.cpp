// Host test for the USB-MIDI preset protocol: SysEx framing from the USB stream,
// USB-MIDI packing of replies, and the upload/read/revert state machine.
#include <string.h>
#include <string>
#include <vector>

#include "check.h"
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

struct Fixture {
    PresetProtocol proto;
    std::vector<char> rx = std::vector<char>(PresetProtocol::kMaxTextBytes);
    std::string activeText;
    Fixture() {
        for (int i = 0; i < 1000; ++i) activeText.push_back(static_cast<char>('a' + i % 26));
        ActiveBank active = {activeText.c_str(), (uint32_t)activeText.size(),
                             Fnv1a32(activeText.data(), (uint32_t)activeText.size()), true, 7};
        proto.Init(rx.data(), stubValidator, active);
    }
    UploadAction send(const Bytes& f, uint32_t now = 0) { return proto.Handle(f.data(), f.size(), now); }
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
    fx.proto.Finish(UploadAction::Commit, true);
    CHECK(fx.replyCmd() == 0x44 && fx.status() == 0 && fx.proto.ReplyLength() == 7);
    fx.proto.Finish(UploadAction::Commit, false);
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
    fx.proto.Finish(UploadAction::Revert, true);
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

int main() {
    CHECK(Fnv1a32("", 0) == 2166136261u);
    CHECK(Fnv1a32("a", 1) == 0xE40C292Cu);  // published FNV-1a test vector
    testAssembler();
    testPacker();
    testInfo();
    testUpload();
    testUploadErrors();
    testRead();
    testSessionLifetime();
    testForeignAndMalformed();
    return CheckSummary("preset_protocol_test");
}
