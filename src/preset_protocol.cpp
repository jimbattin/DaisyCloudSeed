#include "preset_protocol.h"

#include <atomic>
#include <string.h>

// C++14: odr-used static constexpr members need a namespace-scope definition.
constexpr size_t   SysExAssembler::kMaxFrame;
constexpr uint32_t PresetProtocol::kMaxTextBytes;
constexpr uint32_t PresetProtocol::kChunkBytes;
constexpr uint32_t PresetProtocol::kSessionTimeoutMs;
constexpr size_t   PresetProtocol::kMaxReply;

namespace {

constexpr uint8_t kCmdInfo = 0x01, kCmdBegin = 0x02, kCmdData = 0x03, kCmdCommit = 0x04,
                  kCmdRevert = 0x05, kCmdRead = 0x06, kCmdAbort = 0x07;
constexpr uint8_t kReplyFlag  = 0x40;
constexpr size_t  kHeaderLen  = 4;    // 7D 43 53 <cmd>
constexpr size_t  kMaxErrText = 200;  // ParseError body cap

uint32_t getU14(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 7; }
uint32_t getU21(const uint8_t* p) { return getU14(p) | (uint32_t)p[2] << 14; }
uint32_t getU32(const uint8_t* p) {
    return getU21(p) | (uint32_t)p[3] << 21 | (uint32_t)(p[4] & 0x0F) << 28;
}

}  // namespace

uint32_t Fnv1a32(const void* data, uint32_t length) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < length; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/*
 * SysExAssembler
 */

void SysExAssembler::Feed(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        const uint8_t b = data[i];
        if (ready_) continue;      // slot busy until the main loop releases it
        if (b >= 0xF8) continue;   // real-time bytes may interleave anywhere
        if (b == 0xF0) {
            inFrame_ = true; len_ = 0; overflow_ = false;
        } else if (b == 0xF7) {
            if (inFrame_ && !overflow_) {
                std::atomic_signal_fence(std::memory_order_seq_cst);
                ready_ = true;
            }
            inFrame_ = false;
        } else if (b & 0x80) {
            inFrame_ = false;      // any other status byte aborts the frame
        } else if (inFrame_) {
            if (len_ < kMaxFrame) buf_[len_++] = b;
            else overflow_ = true;
        }
    }
}

bool SysExAssembler::Ready() const {
    if (!ready_) return false;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    return true;
}

void SysExAssembler::Release() {
    std::atomic_signal_fence(std::memory_order_seq_cst);
    ready_ = false;
}

size_t PackSysExUsbMidi(const uint8_t* msg, size_t len, uint8_t* out, size_t outCap) {
    if (len < 2 || msg[0] != 0xF0 || msg[len - 1] != 0xF7) return 0;
    const size_t need = (len + 2) / 3 * 4;
    if (need > outCap) return 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        const size_t n = len - i;
        if (n > 3) {
            out[o++] = 0x04;  // SysEx starts or continues
            out[o++] = msg[i]; out[o++] = msg[i + 1]; out[o++] = msg[i + 2];
        } else {
            out[o++] = static_cast<uint8_t>(0x04 + n);  // 0x5/0x6/0x7: ends with 1/2/3 bytes
            out[o++] = msg[i];
            out[o++] = n > 1 ? msg[i + 1] : 0;
            out[o++] = n > 2 ? msg[i + 2] : 0;
        }
    }
    return o;
}

/*
 * PresetProtocol
 */

void PresetProtocol::Init(char* rxBuffer, BankValidator validate, const ActiveBank& active) {
    rx_       = rxBuffer;
    validate_ = validate;
    active_   = active;
    session_  = false;
    replyLen_ = 0;
}

void PresetProtocol::beginReply(uint8_t cmd, UploadStatus status) {
    replyLen_ = 0;
    put(0xF0); put(kSysExManufacturer); put(kSysExTag0); put(kSysExTag1);
    put(static_cast<uint8_t>(cmd | kReplyFlag));
    put(static_cast<uint8_t>(status));
}

void PresetProtocol::put(uint8_t b) { reply_[replyLen_++] = b; }
void PresetProtocol::putU14(uint32_t v) { put(v & 0x7F); put((v >> 7) & 0x7F); }
void PresetProtocol::putU21(uint32_t v) { putU14(v); put((v >> 14) & 0x7F); }
void PresetProtocol::putU32(uint32_t v) { putU21(v); put((v >> 21) & 0x7F); put((v >> 28) & 0x0F); }
void PresetProtocol::endReply() { put(0xF7); }

UploadAction PresetProtocol::Handle(const uint8_t* frame, size_t len, uint32_t nowMs) {
    replyLen_ = 0;
    if (len < kHeaderLen || frame[0] != kSysExManufacturer || frame[1] != kSysExTag0 ||
        frame[2] != kSysExTag1)
        return UploadAction::None;  // not ours: no reply

    const uint8_t  cmd  = frame[3];
    const uint8_t* body = frame + kHeaderLen;
    const size_t   n    = len - kHeaderLen;

    switch (cmd) {
    case kCmdInfo:
        if (n != 0) break;
        beginReply(cmd, UploadStatus::Ok);
        put(kProtocolVersion);
        putU21(kMaxTextBytes);
        putU14(kChunkBytes);
        put(active_.uploaded ? 1 : 0);
        putU21(active_.length);
        putU32(active_.hash);
        put(static_cast<uint8_t>(active_.presetCount & 0x7F));
        endReply();
        return UploadAction::None;

    case kCmdBegin: {
        if (n != 8) break;
        const uint32_t length = getU21(body);
        if (length == 0 || length > kMaxTextBytes) {
            reply(cmd, UploadStatus::BadLength);
            return UploadAction::None;
        }
        session_        = true;
        expectedLength_ = length;
        expectedHash_   = getU32(body + 3);
        received_       = 0;
        nextSeq_        = 0;
        lastActivityMs_ = nowMs;
        reply(cmd, UploadStatus::Ok);
        return UploadAction::None;
    }

    case kCmdData: {
        if (n < 3 || n > 2 + kChunkBytes) {
            session_ = false;
            break;
        }
        const uint32_t seq = getU14(body);
        UploadStatus status = UploadStatus::Ok;
        if (!session_) {
            status = UploadStatus::NoSession;
        } else if (nextSeq_ > 0 && seq == nextSeq_ - 1) {
            lastActivityMs_ = nowMs;  // duplicate after a lost ACK: re-ACK, append nothing
        } else if (seq != nextSeq_) {
            status = UploadStatus::BadSeq;
        } else if (n - 2 > expectedLength_ - received_) {
            status = UploadStatus::BadLength;
        } else {
            memcpy(rx_ + received_, body + 2, n - 2);
            received_ += static_cast<uint32_t>(n - 2);
            ++nextSeq_;
            lastActivityMs_ = nowMs;
        }
        if (status != UploadStatus::Ok) session_ = false;
        beginReply(cmd, status);
        putU14(seq);
        endReply();
        return UploadAction::None;
    }

    case kCmdCommit: {
        if (n != 0) {
            session_ = false;
            break;
        }
        const bool had = session_;
        session_ = false;  // every COMMIT ends the session
        if (!had) {
            reply(cmd, UploadStatus::NoSession);
        } else if (received_ != expectedLength_) {
            reply(cmd, UploadStatus::LengthMismatch);
        } else if (Fnv1a32(rx_, received_) != expectedHash_) {
            reply(cmd, UploadStatus::HashMismatch);
        } else {
            char err[128];
            err[0] = '\0';
            if (validate_(rx_, received_, err, sizeof err))
                return UploadAction::Commit;  // reply built by Finish()
            beginReply(cmd, UploadStatus::ParseError);
            for (size_t i = 0; i < kMaxErrText && err[i]; ++i)
                put(static_cast<uint8_t>(err[i] & 0x7F));
            endReply();
        }
        return UploadAction::None;
    }

    case kCmdRevert:
        if (n != 0) break;
        session_ = false;
        return UploadAction::Revert;  // reply built by Finish()

    case kCmdRead: {
        if (n != 2) break;
        const uint32_t index = getU14(body);
        const uint32_t start = index * kChunkBytes;
        if (start >= active_.length) {
            reply(cmd, UploadStatus::BadSeq);
            return UploadAction::None;
        }
        uint32_t count = active_.length - start;
        if (count > kChunkBytes) count = kChunkBytes;
        beginReply(cmd, UploadStatus::Ok);
        putU14(index);
        putU21(active_.length);
        for (uint32_t i = 0; i < count; ++i)
            put(static_cast<uint8_t>(active_.text[start + i] & 0x7F));
        endReply();
        return UploadAction::None;
    }

    case kCmdAbort:
        if (n != 0) break;
        session_ = false;
        reply(cmd, UploadStatus::Ok);
        return UploadAction::None;

    default:
        break;
    }
    reply(cmd, UploadStatus::BadFrame);  // unknown command, or wrong body length
    return UploadAction::None;
}

void PresetProtocol::Finish(UploadAction action, bool ok) {
    const uint8_t cmd = action == UploadAction::Commit ? kCmdCommit : kCmdRevert;
    reply(cmd, ok ? UploadStatus::Ok : UploadStatus::FlashError);
}

void PresetProtocol::Tick(uint32_t nowMs) {
    if (session_ && nowMs - lastActivityMs_ >= kSessionTimeoutMs) session_ = false;
}
