#include "SimulatedEcu.h"

#include <cstring>
#include <string>

#include "Uds.h"

namespace cantrip {

namespace {
constexpr uint64_t kSessionTimeoutMs = 5000;
constexpr uint64_t kRoutineDurationMs = 400;

constexpr uint8_t kDefaultSession = 0x01;
constexpr uint8_t kProgrammingSession = 0x02;
constexpr uint8_t kExtendedSession = 0x03;

std::vector<uint8_t> negative(uint8_t serviceId, uint8_t nrc) {
    return {Uds::kNegativeResponse, serviceId, nrc};
}

std::vector<uint8_t> withText(std::vector<uint8_t> head, const char* text) {
    head.insert(head.end(), text, text + std::strlen(text));
    return head;
}
} // namespace

SimulatedEcu::SimulatedEcu() {
    IsoTpConfig config;
    config.txId = kResponseId;
    config.rxId = kRequestId;
    transport_.setConfig(config);
    std::memcpy(vin_.data(), "CANTR1PSYNTH00001", 17);
    restoreDtcs();
}

void SimulatedEcu::restoreDtcs() {
    dtcs_ = {
        {0x03, 0x01, 0x00, 0x09}, // P0301-00, failed and confirmed
        {0xC1, 0x00, 0x87, 0x08}, // U0100-87, confirmed
        {0x40, 0x35, 0x11, 0x24}, // C0035-11, pending
    };
}

void SimulatedEcu::expireSession(uint64_t nowMs) {
    if (session_ != kDefaultSession && nowMs - lastRequestMs_ > kSessionTimeoutMs) {
        session_ = kDefaultSession;
        unlocked_ = false;
        seedGiven_ = false;
    }
}

std::vector<uint8_t> SimulatedEcu::handle(const std::vector<uint8_t>& request, uint64_t nowMs) {
    if (request.empty()) return {};
    expireSession(nowMs);
    lastRequestMs_ = nowMs;

    const uint8_t sid = request[0];
    const bool quiet = udsSuppressesPositiveResponse(request);
    const uint8_t sub = request.size() >= 2 ? static_cast<uint8_t>(request[1] & 0x7F) : 0;
    const auto positive = [quiet](std::vector<uint8_t> response) { return quiet ? std::vector<uint8_t>{} : response; };

    switch (sid) {
    case Uds::kSessionControl: {
        if (request.size() != 2) return negative(sid, 0x13);
        if (sub < kDefaultSession || sub > kExtendedSession) return negative(sid, 0x12);
        session_ = sub;
        unlocked_ = false;
        seedGiven_ = false;
        // P2 = 50 ms, P2* = 5000 ms, as the two timing words.
        return positive({0x50, sub, 0x00, 0x32, 0x01, 0xF4});
    }
    case Uds::kEcuReset: {
        if (request.size() != 2) return negative(sid, 0x13);
        if (sub != 0x01 && sub != 0x03) return negative(sid, 0x12);
        session_ = kDefaultSession;
        unlocked_ = false;
        seedGiven_ = false;
        restoreDtcs();
        return positive({0x51, sub});
    }
    case Uds::kTesterPresent: {
        if (request.size() != 2) return negative(sid, 0x13);
        if (sub != 0x00) return negative(sid, 0x12);
        return positive({0x7E, 0x00});
    }
    case Uds::kReadDataById: {
        if (request.size() != 3) return negative(sid, 0x13);
        const uint16_t did = static_cast<uint16_t>(request[1] << 8 | request[2]);
        std::vector<uint8_t> head = {0x62, request[1], request[2]};
        switch (did) {
        case 0xF190: head.insert(head.end(), vin_.begin(), vin_.end()); return head;
        case 0xF187: return withText(head, "CT-ECU-0001");
        case 0xF18C: return withText(head, "SN20261115");
        case 0xF186: head.push_back(session_); return head;
        case 0x0100: {
            // Engine speed in rpm, sweeping from 800 to 2800 every 20 s.
            const uint16_t rpm = static_cast<uint16_t>(800 + (nowMs / 10) % 2000);
            head.push_back(static_cast<uint8_t>(rpm >> 8));
            head.push_back(static_cast<uint8_t>(rpm & 0xFF));
            return head;
        }
        case 0x0101: head.push_back(0x7B); return head; // 83 degrees C with the usual -40 offset
        default: return negative(sid, 0x31);
        }
    }
    case Uds::kWriteDataById: {
        if (request.size() < 4) return negative(sid, 0x13);
        if (request[1] != 0xF1 || request[2] != 0x90) return negative(sid, 0x31);
        if (session_ != kExtendedSession) return negative(sid, 0x7F);
        if (!unlocked_) return negative(sid, 0x33);
        if (request.size() != 3 + vin_.size()) return negative(sid, 0x13);
        std::memcpy(vin_.data(), request.data() + 3, vin_.size());
        return {0x6E, 0xF1, 0x90};
    }
    case Uds::kSecurityAccess: {
        if (request.size() < 2) return negative(sid, 0x13);
        if (session_ == kDefaultSession) return negative(sid, 0x7F);
        if (request[1] == 0x01) {
            if (request.size() != 2) return negative(sid, 0x13);
            // A zero seed is how an ECU says it is unlocked already.
            seed_ = unlocked_ ? 0 : (static_cast<uint32_t>(nowMs * 2654435761u) | 1u);
            seedGiven_ = !unlocked_;
            return {0x67, 0x01, static_cast<uint8_t>(seed_ >> 24), static_cast<uint8_t>(seed_ >> 16),
                    static_cast<uint8_t>(seed_ >> 8), static_cast<uint8_t>(seed_)};
        }
        if (request[1] == 0x02) {
            if (request.size() != 6) return negative(sid, 0x13);
            if (!seedGiven_) return negative(sid, 0x24);
            const uint32_t key = static_cast<uint32_t>(request[2]) << 24 | static_cast<uint32_t>(request[3]) << 16 |
                                 static_cast<uint32_t>(request[4]) << 8 | request[5];
            seedGiven_ = false;
            if (key != (seed_ ^ kKeyMask)) return negative(sid, 0x35);
            unlocked_ = true;
            return {0x67, 0x02};
        }
        return negative(sid, 0x12);
    }
    case Uds::kReadDtc: {
        if (request.size() != 3) return negative(sid, request.size() < 2 ? 0x13 : (request[1] == 0x01 || request[1] == 0x02) ? 0x13 : 0x12);
        const uint8_t mask = request[2];
        if (request[1] == 0x02) {
            std::vector<uint8_t> response = {0x59, 0x02, 0xFF};
            for (const Dtc& dtc : dtcs_) {
                if (dtc.status & mask) response.insert(response.end(), {dtc.high, dtc.middle, dtc.low, dtc.status});
            }
            return response;
        }
        if (request[1] == 0x01) {
            uint16_t count = 0;
            for (const Dtc& dtc : dtcs_) count += (dtc.status & mask) ? 1 : 0;
            // 0x01 is the ISO 14229-1 trouble code format.
            return {0x59, 0x01, 0xFF, 0x01, static_cast<uint8_t>(count >> 8), static_cast<uint8_t>(count & 0xFF)};
        }
        return negative(sid, 0x12);
    }
    case Uds::kClearDtc: {
        if (request.size() != 4) return negative(sid, 0x13);
        if (request[1] != 0xFF || request[2] != 0xFF || request[3] != 0xFF) return negative(sid, 0x31);
        dtcs_.clear();
        return {0x54};
    }
    case Uds::kRoutineControl: {
        if (request.size() != 4) return negative(sid, 0x13);
        if (sub != 0x01) return negative(sid, 0x12);
        if (request[2] != 0xFF || request[3] != 0x00) return negative(sid, 0x31);
        if (!delayedResponse_.empty()) return negative(sid, 0x21);
        delayedResponse_ = {0x71, 0x01, 0xFF, 0x00};
        delayedAtMs_ = nowMs + kRoutineDurationMs;
        return negative(sid, Uds::kNrcResponsePending);
    }
    default:
        return negative(sid, 0x11);
    }
}

void SimulatedEcu::onFrame(const CanFrame& frame, uint64_t nowMs) {
    transport_.onFrame(frame, nowMs);
    for (const std::vector<uint8_t>& request : transport_.takeMessages()) {
        const std::vector<uint8_t> response = handle(request, nowMs);
        if (!response.empty()) transport_.send(response, nowMs);
    }
}

void SimulatedEcu::poll(uint64_t nowMs) {
    if (!delayedResponse_.empty() && nowMs >= delayedAtMs_ && !transport_.sending()) {
        transport_.send(delayedResponse_, nowMs);
        delayedResponse_.clear();
    }
    transport_.poll(nowMs);
    transport_.takeErrors(); // a tester that walks away mid-transfer is not the ECU's problem
}

std::vector<CanFrame> SimulatedEcu::takeFrames() {
    return transport_.takeFrames();
}

} // namespace cantrip
