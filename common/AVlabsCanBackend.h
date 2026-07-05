// AVlabs CAN Backend(tm, not really) - the "any CAN adapter, we don't care
// whose" interface. CANtrip's extcap and app talk to hardware only through
// this interface, never directly to a vendor SDK, so adding support for a
// new CAN adapter vendor means writing one new ICanBackend implementation
// (see PeakBackend for the reference one) with no changes to the extcap's
// pcap serialization or the app's decode/UI code. One bus to sniff them all.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
    // pcan2pcap can serialize it as a normal `struct can_frame` and get
    // Wireshark's own error-class dissection for free, same as data frames.
    bool error = false;
};

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
// `expertInitString` is an escape hatch for vendor-specific raw timing
// strings (e.g. PCAN-Basic's CAN_InitializeFD "f_clock_mhz=..." format)
// since exact bit-timing register layouts are not standardized across
// vendors; when non-empty it overrides nominal/dataBitrateBps for FD init.
struct CanBitrateConfig {
    uint32_t nominalBitrateBps = 500000;
    uint32_t dataBitrateBps = 2000000; // used only when fd == true
    bool fd = false;
    std::string expertInitString;
};

// One CAN hardware vendor's SDK, wrapped behind a vendor-neutral interface.
// Every implementation dynamically loads its vendor DLL at runtime
// (LoadLibrary/GetProcAddress) rather than link-time, so CANtrip builds and
// runs fine with only some (or none) of the vendor SDKs installed - a
// backend whose DLL isn't found is simply omitted by probeAvailableBackends,
// not treated as a build or startup error.
class ICanBackend {
public:
    virtual ~ICanBackend() = default;

    // Short, stable, lowercase identifier used to namespace extcap
    // interface IDs across vendors, e.g. "peak", "vector", "kvaser".
    virtual std::string id() const = 0;
    // Human-readable vendor name for display, e.g. "PEAK-System PCAN-Basic".
    virtual std::string displayName() const = 0;

    virtual std::vector<CanChannelInfo> enumerateChannels() const = 0;

    virtual bool initialize(uint64_t channelId, const CanBitrateConfig& config, std::string* error) = 0;
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
// To add a new vendor: implement ICanBackend in a new CanBackend*.cpp/.h
// pair (following PeakBackend.h/.cpp), then append a load attempt here.
std::vector<std::unique_ptr<ICanBackend>> probeAvailableBackends();

} // namespace cantrip
