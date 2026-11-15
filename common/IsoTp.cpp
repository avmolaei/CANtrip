#include "IsoTp.h"

#include <algorithm>
#include <cstring>

namespace cantrip {

namespace {
// The frame type is the high nibble of the first byte.
constexpr uint8_t kSingle = 0x0;
constexpr uint8_t kFirst = 0x1;
constexpr uint8_t kConsecutive = 0x2;
constexpr uint8_t kFlowControl = 0x3;

// Flow status, the low nibble of a flow control frame's first byte.
constexpr uint8_t kContinue = 0x0;
constexpr uint8_t kWait = 0x1;
constexpr uint8_t kOverflow = 0x2;

// A sender may be told to wait this many times in a row before we give up.
constexpr int kMaxWaits = 10;

// STmin as sent on the wire: 0x00 to 0x7F are milliseconds, 0xF1 to 0xF9 are
// 100 to 900 microseconds (rounded up to 1 ms here, the finest step we
// have), and everything else is reserved and to be read as the maximum.
uint8_t stMinToMs(uint8_t raw) {
    if (raw <= 0x7F) return raw;
    if (raw >= 0xF1 && raw <= 0xF9) return 1;
    return 0x7F;
}
} // namespace

IsoTpChannel::IsoTpChannel(const IsoTpConfig& config) : config_(config) {}

void IsoTpChannel::setConfig(const IsoTpConfig& config) {
    config_ = config;
    reset();
}

void IsoTpChannel::reset() {
    txState_ = TxState::Idle;
    rxState_ = RxState::Idle;
    txPayload_.clear();
    rxPayload_.clear();
    outFrames_.clear();
    messages_.clear();
    errors_.clear();
}

void IsoTpChannel::emitFrame(const uint8_t* bytes, size_t count) {
    CanFrame frame;
    frame.id = config_.txId;
    frame.extended = config_.extended;
    std::memset(frame.data, config_.padByte, 8);
    std::memcpy(frame.data, bytes, count);
    frame.dlc = static_cast<uint8_t>(config_.padding ? 8 : count);
    outFrames_.push_back(frame);
}

void IsoTpChannel::emitFlowControl() {
    // Sent raw: 0x00 to 0x7F already means milliseconds.
    const uint8_t fc[3] = {static_cast<uint8_t>(kFlowControl << 4 | kContinue), config_.blockSize,
                           static_cast<uint8_t>(std::min<int>(config_.stMinMs, 0x7F))};
    emitFrame(fc, 3);
}

void IsoTpChannel::failTx(const std::string& why) {
    txState_ = TxState::Idle;
    txPayload_.clear();
    errors_.push_back(why);
}

void IsoTpChannel::failRx(const std::string& why) {
    rxState_ = RxState::Idle;
    rxPayload_.clear();
    errors_.push_back(why);
}

bool IsoTpChannel::send(const std::vector<uint8_t>& payload, uint64_t nowMs) {
    if (sending() || payload.empty() || payload.size() > kMaxPayload) return false;

    if (payload.size() <= 7) {
        uint8_t bytes[8];
        bytes[0] = static_cast<uint8_t>(kSingle << 4 | payload.size());
        std::memcpy(bytes + 1, payload.data(), payload.size());
        emitFrame(bytes, payload.size() + 1);
        return true;
    }

    txPayload_ = payload;
    uint8_t bytes[8];
    bytes[0] = static_cast<uint8_t>(kFirst << 4 | (payload.size() >> 8));
    bytes[1] = static_cast<uint8_t>(payload.size() & 0xFF);
    std::memcpy(bytes + 2, payload.data(), 6);
    emitFrame(bytes, 8);
    txOffset_ = 6;
    txSequence_ = 1;
    txWaits_ = 0;
    txState_ = TxState::WaitFlowControl;
    txDeadlineMs_ = nowMs + config_.timeoutMs;
    return true;
}

void IsoTpChannel::pumpTx(uint64_t nowMs) {
    while (txState_ == TxState::SendingConsecutive && nowMs >= txNextAtMs_) {
        const size_t count = std::min<size_t>(7, txPayload_.size() - txOffset_);
        uint8_t bytes[8];
        bytes[0] = static_cast<uint8_t>(kConsecutive << 4 | (txSequence_ & 0x0F));
        std::memcpy(bytes + 1, txPayload_.data() + txOffset_, count);
        emitFrame(bytes, count + 1);
        txOffset_ += count;
        txSequence_ = static_cast<uint8_t>((txSequence_ + 1) & 0x0F);

        if (txOffset_ >= txPayload_.size()) {
            txState_ = TxState::Idle;
            txPayload_.clear();
            return;
        }
        if (txBlockSize_ != 0 && ++txBlockSent_ >= txBlockSize_) {
            txState_ = TxState::WaitFlowControl;
            txDeadlineMs_ = nowMs + config_.timeoutMs;
            return;
        }
        if (txStMinMs_ > 0) {
            txNextAtMs_ = nowMs + txStMinMs_;
            return;
        }
    }
}

void IsoTpChannel::onFrame(const CanFrame& frame, uint64_t nowMs) {
    if (frame.error || frame.rtr || frame.fd) return;
    if (frame.id != config_.rxId || frame.extended != config_.extended) return;
    const size_t length = std::min<size_t>(frame.dlc, 8);
    if (length < 1) return;
    const uint8_t* d = frame.data;

    switch (d[0] >> 4) {
    case kSingle: {
        const size_t count = d[0] & 0x0F;
        if (count == 0 || count > 7 || count > length - 1) return; // malformed, not ours to act on
        if (rxState_ == RxState::Receiving) failRx("a new message arrived before the previous one was complete");
        messages_.emplace_back(d + 1, d + 1 + count);
        break;
    }
    case kFirst: {
        const size_t total = (static_cast<size_t>(d[0] & 0x0F) << 8) | d[1];
        if (length < 8 || total < 8) return;
        if (rxState_ == RxState::Receiving) failRx("a new message arrived before the previous one was complete");
        rxPayload_.assign(d + 2, d + 8);
        rxTotal_ = total;
        rxSequence_ = 1;
        rxBlockCount_ = 0;
        rxState_ = RxState::Receiving;
        rxDeadlineMs_ = nowMs + config_.timeoutMs;
        emitFlowControl();
        break;
    }
    case kConsecutive: {
        if (rxState_ != RxState::Receiving) return;
        if ((d[0] & 0x0F) != rxSequence_) {
            failRx("a consecutive frame arrived out of order");
            return;
        }
        const size_t wanted = std::min<size_t>(7, rxTotal_ - rxPayload_.size());
        if (length - 1 < wanted) {
            failRx("a consecutive frame was too short");
            return;
        }
        rxPayload_.insert(rxPayload_.end(), d + 1, d + 1 + wanted);
        rxSequence_ = static_cast<uint8_t>((rxSequence_ + 1) & 0x0F);
        if (rxPayload_.size() >= rxTotal_) {
            messages_.push_back(std::move(rxPayload_));
            rxPayload_.clear();
            rxState_ = RxState::Idle;
            break;
        }
        rxDeadlineMs_ = nowMs + config_.timeoutMs;
        if (config_.blockSize != 0 && ++rxBlockCount_ >= config_.blockSize) {
            rxBlockCount_ = 0;
            emitFlowControl();
        }
        break;
    }
    case kFlowControl: {
        if (txState_ != TxState::WaitFlowControl) return;
        const uint8_t status = d[0] & 0x0F;
        if (status == kContinue) {
            txBlockSize_ = length >= 2 ? d[1] : 0;
            txStMinMs_ = length >= 3 ? stMinToMs(d[2]) : 0;
            txBlockSent_ = 0;
            txWaits_ = 0;
            txNextAtMs_ = nowMs;
            txState_ = TxState::SendingConsecutive;
            pumpTx(nowMs);
        } else if (status == kWait) {
            if (++txWaits_ > kMaxWaits) failTx("the other side kept asking to wait");
            else txDeadlineMs_ = nowMs + config_.timeoutMs;
        } else if (status == kOverflow) {
            failTx("the other side has no room for a message this long");
        } else {
            failTx("the other side sent an invalid flow control frame");
        }
        break;
    }
    default:
        break;
    }
}

void IsoTpChannel::poll(uint64_t nowMs) {
    pumpTx(nowMs);
    if (txState_ == TxState::WaitFlowControl && nowMs >= txDeadlineMs_) {
        failTx("no flow control frame came back");
    }
    if (rxState_ == RxState::Receiving && nowMs >= rxDeadlineMs_) {
        failRx("the message stopped arriving partway through");
    }
}

std::vector<CanFrame> IsoTpChannel::takeFrames() {
    std::vector<CanFrame> out;
    out.swap(outFrames_);
    return out;
}

std::vector<std::vector<uint8_t>> IsoTpChannel::takeMessages() {
    std::vector<std::vector<uint8_t>> out;
    out.swap(messages_);
    return out;
}

std::vector<std::string> IsoTpChannel::takeErrors() {
    std::vector<std::string> out;
    out.swap(errors_);
    return out;
}

} // namespace cantrip
