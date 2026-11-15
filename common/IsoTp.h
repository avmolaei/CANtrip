// ISO-TP (ISO 15765-2): carries messages of up to 4095 bytes over 8-byte CAN
// frames. It is what UDS diagnostics ride on.
//
// One IsoTpChannel is one direction pair: it sends on txId and listens on
// rxId. A tester and an ECU each hold one, with the two IDs swapped. The
// class owns no clock, no thread and no hardware: the owner feeds it the
// frames it receives and the current time, and collects the frames it wants
// transmitted. That is what lets the tester in the window, the simulated ECU
// in the synthetic source and the unit tests all share it.
//
// Classic CAN with normal addressing only. CAN FD frames are ignored.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "AVlabsCanBackend.h"

namespace cantrip {

struct IsoTpConfig {
    uint32_t txId = 0x7E0;
    uint32_t rxId = 0x7E8;
    bool extended = false; // 29-bit IDs
    // Frames are filled up to 8 bytes with padByte, as most ECUs expect.
    bool padding = true;
    uint8_t padByte = 0xCC;
    // What this side asks of the sender in its flow control frames: how many
    // consecutive frames before the next flow control (0 = all of them), and
    // the gap between them in milliseconds.
    uint8_t blockSize = 0;
    uint8_t stMinMs = 0;
    // How long to wait for the other side's next frame before giving up.
    uint32_t timeoutMs = 1000;
};

class IsoTpChannel {
public:
    static constexpr size_t kMaxPayload = 4095;

    explicit IsoTpChannel(const IsoTpConfig& config = {});

    // Changing the configuration drops any transfer in progress.
    void setConfig(const IsoTpConfig& config);
    const IsoTpConfig& config() const { return config_; }
    void reset();

    // Starts sending a message. False if one is still being sent, or if the
    // payload is empty or longer than kMaxPayload.
    bool send(const std::vector<uint8_t>& payload, uint64_t nowMs);
    // A frame seen on the bus. Frames for other IDs are ignored.
    void onFrame(const CanFrame& frame, uint64_t nowMs);
    // Sends what is due and notices timeouts. Call it every few milliseconds.
    void poll(uint64_t nowMs);

    bool sending() const { return txState_ != TxState::Idle; }
    bool receiving() const { return rxState_ != RxState::Idle; }

    // Each returns what has piled up since its last call.
    std::vector<CanFrame> takeFrames();                  // to transmit, in order
    std::vector<std::vector<uint8_t>> takeMessages();    // received complete
    std::vector<std::string> takeErrors();               // transfers given up on

private:
    enum class TxState { Idle, WaitFlowControl, SendingConsecutive };
    enum class RxState { Idle, Receiving };

    void emitFrame(const uint8_t* bytes, size_t count);
    void emitFlowControl();
    void pumpTx(uint64_t nowMs);
    void failTx(const std::string& why);
    void failRx(const std::string& why);

    IsoTpConfig config_;

    TxState txState_ = TxState::Idle;
    std::vector<uint8_t> txPayload_;
    size_t txOffset_ = 0;
    uint8_t txSequence_ = 0;
    uint8_t txBlockSize_ = 0;
    uint8_t txBlockSent_ = 0;
    uint8_t txStMinMs_ = 0;
    int txWaits_ = 0;
    uint64_t txNextAtMs_ = 0;
    uint64_t txDeadlineMs_ = 0;

    RxState rxState_ = RxState::Idle;
    std::vector<uint8_t> rxPayload_;
    size_t rxTotal_ = 0;
    uint8_t rxSequence_ = 0;
    uint8_t rxBlockCount_ = 0;
    uint64_t rxDeadlineMs_ = 0;

    std::vector<CanFrame> outFrames_;
    std::vector<std::vector<uint8_t>> messages_;
    std::vector<std::string> errors_;
};

} // namespace cantrip
