// AVlabs CAN Backend(tm, not really) - the "any CAN adapter, we don't care
// whose" interface. CANtrip's extcap and app talk to hardware only through
// this interface, never directly to a vendor SDK, so adding support for a
// new CAN adapter vendor means writing one new IAvlabsCanBackend implementation
// (see PeakBackend for the reference one) with no changes to the extcap's
// pcap serialization or the app's decode/UI code. One bus to sniff them all.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "CanBitTiming.h"

namespace cantrip {

// Backend-agnostic frame representation, used for both reads and writes
// across every vendor backend.
struct CanFrame {
    uint32_t id = 0;
    bool extended = false;
    bool rtr = false;
    bool fd = false;
    bool brs = false;      // bit rate switch (FD only)
    bool esi = false;      // error state indicator (FD only)
    uint8_t dlc = 0;       // data length code (0-8 classic, 0-15 FD)
    uint8_t data[64] = {};
    uint64_t timestampUs = 0; // microseconds since the channel was initialized

    // When true, this isn't a data frame: `id` is instead a SocketCAN
    // error-class bitmask (CAN_ERR_* from linux/can/error.h) and `data`
    // holds the associated error detail bytes (data[2] = protocol violation
    // type - stuff/form/bit/ACK/etc). This mirrors the real SocketCAN error
    // frame ABI exactly (verified against a real Wireshark decode) so
    // can2pcap can serialize it as a normal `struct can_frame` and get
    // Wireshark's own error-class dissection for free, same as data frames.
    bool error = false;
};

// CAN_ERR_* bitmask values a backend's readFrame() should use for
// CanFrame::id when error == true, straight from linux/can/error.h (a
// long-stable public kernel uAPI, safe to hardcode - same convention
// already used by the synthetic test source in can2pcap.cpp, and decoded
// by Wireshark's own SocketCAN dissector on the other end). Only the
// subset backends actually populate is listed here, not the full uAPI.
namespace CanErr {
constexpr uint32_t kCtrl = 0x00000004u;    // controller problem (see kCtrl* below)
constexpr uint32_t kProt = 0x00000008u;    // protocol violation (see kProt* below)
constexpr uint32_t kAck = 0x00000020u;     // received no ACK on transmission
constexpr uint32_t kBusoff = 0x00000040u;  // bus off

// CAN_ERR_PROT sub-type, carried in data[2] when kProt is set.
constexpr uint8_t kProtBit = 0x01;
constexpr uint8_t kProtForm = 0x02;
constexpr uint8_t kProtStuff = 0x04;
constexpr uint8_t kProtOverload = 0x20;
}  // namespace CanErr

// A channel exposed by a backend. `channelId` is opaque outside the backend
// that produced it - each backend interprets its own IDs however its native
// SDK addresses channels (PEAK uses small integer handles, other vendors may
// not); callers just round-trip the value back into that same backend's
// calls and never need to know its underlying meaning.
struct CanChannelInfo {
    uint64_t channelId = 0;
    std::string name;      // e.g. "PCAN_USBBUS1"
    bool available = false;
};

// Bitrate/timing configuration common enough to express across vendors.
// `nominalTiming`/`dataTiming` (BRP/TSEG1/TSEG2/SJW) are used only when
// `fd` is true - every vendor's FD init ultimately needs these same four
// numbers per phase, whether it takes them as a formatted string (PCAN-Basic)
// or as struct fields (Vector XL), so callers compute them once (see
// CanBitTiming.h) rather than each backend reinventing bit-timing math.
struct CanBitrateConfig {
    uint32_t nominalBitrateBps = 500000;
    uint32_t dataBitrateBps = 2000000; // used only when fd == true
    bool fd = false;
    CanTimingValues nominalTiming;
    CanTimingValues dataTiming;
};

// One CAN hardware vendor's SDK, wrapped behind a vendor-neutral interface.
// Every implementation dynamically loads its vendor DLL at runtime
// (LoadLibrary/GetProcAddress) rather than link-time, so CANtrip builds and
// runs fine with only some (or none) of the vendor SDKs installed - a
// backend whose DLL isn't found is simply omitted by probeAvailableBackends,
// not treated as a build or startup error.
class IAvlabsCanBackend {
public:
    virtual ~IAvlabsCanBackend() = default;

    // Short, stable, lowercase identifier used to namespace extcap
    // interface IDs across vendors, e.g. "peak", "vector", "kvaser".
    virtual std::string id() const = 0;
    // Human-readable vendor name for display, e.g. "PEAK-System PCAN-Basic".
    virtual std::string displayName() const = 0;

    virtual std::vector<CanChannelInfo> enumerateChannels() const = 0;

    // requestOwnership: true (default) requests exclusive bus-configuration
    // rights and configures the bitrate/timing - today's existing behavior.
    // false opens the channel in listen-only mode instead: no configuration
    // permission is requested and no bitrate/timing call is made, on the
    // assumption another port (another app, or another CANtrip-owned port on
    // the same channel) has already configured the bus. Lets CANtrip coexist
    // on a channel another app already owns, and lets CANtrip itself open a
    // second port on a channel it's already capturing on (see MessageSender)
    // without re-requesting exclusive configuration rights.
    virtual bool initialize(uint64_t channelId, const CanBitrateConfig& config,
                             bool requestOwnership, std::string* error) = 0;
    virtual void uninitialize(uint64_t channelId) = 0;

    // Non-blocking single read; returns false with an empty *error when no
    // frame is available yet (not an error), and false with *error set on a
    // real failure.
    virtual bool readFrame(uint64_t channelId, CanFrame* out, std::string* error) = 0;
    virtual bool writeFrame(uint64_t channelId, const CanFrame& frame, std::string* error) = 0;
};

// Attempts to load every backend CANtrip knows about. A backend that can't
// find its vendor DLL is silently omitted rather than treated as an error,
// since most users will have only one vendor's driver installed.
//
// To add a new vendor: implement IAvlabsCanBackend in a new CanBackend*.cpp/.h
// pair (following PeakBackend.h/.cpp), then append a load attempt here.
std::vector<std::unique_ptr<IAvlabsCanBackend>> probeAvailableBackends();

} // namespace cantrip
