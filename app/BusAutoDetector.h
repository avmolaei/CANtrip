// Tries a fixed set of classic CAN bitrates against a given interface by
// briefly running can2pcap.exe directly (bypassing tshark entirely - the
// same isolation technique used throughout this project to diagnose real
// hardware behavior), and inspecting the raw SocketCAN pcap output for
// error frames. Classic CAN only for now - CAN FD's extra nominal/data/
// sample-point dimensions make that a separate, harder scan.
#pragma once

#include <cstdint>
#include <optional>

#include <QString>

namespace cantrip {

class BusAutoDetector {
public:
    // Runs synchronously - blocks the calling thread for roughly
    // (candidate count * probeMs). Deliberately kept simple/blocking rather
    // than an async state machine: this is a short (a second or two total),
    // bounded, user-initiated one-shot action, not the kind of unbounded
    // background work this project has had real UI-freeze bugs from before
    // (see TsharkCapture's display-rate throttle) - there's no risk of it
    // running away, so the extra complexity of making it async isn't
    // justified here.
    static std::optional<uint32_t> detectClassicBitrate(
        const QString& can2pcapPath, const QString& interfaceId);

private:
    // Returns true if this bitrate produced at least one real data record
    // with zero error records within probeMs.
    static bool probeBitrate(const QString& can2pcapPath, const QString& interfaceId,
                              uint32_t bitrateBps, int probeMs);
};

} // namespace cantrip
