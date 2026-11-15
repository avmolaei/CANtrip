// A small pretend ECU that answers UDS requests, so the Diagnostics tab can
// be tried and tested with no hardware. It lives in the synthetic test
// source: requests transmitted on that bus reach it, and its answers come
// back as received frames.
//
// It listens on 0x7E1 and answers on 0x7E9, the second ECU of the usual OBD
// numbering. Not 0x7E0/0x7E8: the synthetic source already sends a made-up
// periodic frame on 0x7E8 (OBDResponse in test/sample.dbc), which would cut
// into every multi-frame answer. What it knows:
//   10  sessions: default (01), programming (02), extended (03). It falls
//       back to the default session after 5 s without a request, which is
//       what Tester Present is for.
//   11  reset: back to the default session, locked, trouble codes restored.
//   3E  tester present.
//   22  read: F190 VIN, F187 part number, F18C serial number, F186 active
//       session, 0100 engine speed (changes over time), 0101 coolant
//       temperature.
//   2E  write F190 (17 bytes): extended session and unlocked only.
//   27  security: 27 01 gives a 4-byte seed, 27 02 wants seed XOR CAFEBABE.
//       Not available in the default session.
//   19  trouble codes: 19 02 <mask> lists them, 19 01 <mask> counts them.
//   14  clear trouble codes (FF FF FF).
//   31  routine FF00: answers "response pending" first, then finishes
//       400 ms later.
// Anything else gets the matching negative response.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "IsoTp.h"

namespace cantrip {

class SimulatedEcu {
public:
    static constexpr uint32_t kRequestId = 0x7E1;
    static constexpr uint32_t kResponseId = 0x7E9;
    static constexpr uint32_t kKeyMask = 0xCAFEBABEu;

    SimulatedEcu();

    // A frame transmitted on the ECU's bus.
    void onFrame(const CanFrame& frame, uint64_t nowMs);
    // Call every few milliseconds: sends what is due.
    void poll(uint64_t nowMs);
    // The frames the ECU puts on the bus, in order.
    std::vector<CanFrame> takeFrames();

    // The service layer on its own: the answer to one request, empty when
    // the ECU stays silent. onFrame() and poll() go through this.
    std::vector<uint8_t> handle(const std::vector<uint8_t>& request, uint64_t nowMs);

private:
    struct Dtc {
        uint8_t high, middle, low, status;
    };

    void restoreDtcs();
    void expireSession(uint64_t nowMs);

    IsoTpChannel transport_;
    uint8_t session_ = 0x01;
    bool unlocked_ = false;
    bool seedGiven_ = false;
    uint32_t seed_ = 0;
    std::array<uint8_t, 17> vin_;
    std::vector<Dtc> dtcs_;
    uint64_t lastRequestMs_ = 0;
    // A slow routine's final answer, sent once its time has come.
    std::vector<uint8_t> delayedResponse_;
    uint64_t delayedAtMs_ = 0;
};

} // namespace cantrip
