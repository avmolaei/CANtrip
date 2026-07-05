// pcan2pcap - Wireshark extcap bridge for CANtrip's CAN hardware backends.
//
// Despite the filename (kept for continuity with the PEAK-only prototype),
// this bridges *any* ICanBackend CANtrip has probed as available - see
// ../common/CanBackend.h for the vendor-neutral interface and
// ../common/CanBackendRegistry.cpp for the list of backends it tries.
//
// Implements the four extcap CLI contracts Wireshark/tshark invoke:
//   --extcap-interfaces           list capture interfaces
//   --extcap-dlts                 list link-layer types per interface
//   --extcap-config               list configurable capture options
//   --capture --fifo <path>       stream frames as classic pcap records
//
// Frames are written using libpcap's LINKTYPE_CAN_SOCKETCAN (227) layout,
// which Wireshark's built-in SocketCAN dissector already understands:
// each record is a `struct can_frame` (16 bytes) or `struct canfd_frame`
// (72 bytes) with the entire struct in network (big-endian) byte order.
// This gets CAN ID/DLC/data/FD-flag decoding for free from Wireshark;
// CANtrip's own DBC layer adds signal-level decode on top later.
#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>
#include <chrono>
#include <thread>

#include "../common/AVlabsCanBackend.h"

namespace {

using cantrip::CanBitrateConfig;
using cantrip::CanChannelInfo;
using cantrip::CanFrame;
using cantrip::ICanBackend;

constexpr uint16_t kLinktypeCanSocketcan = 227;
constexpr const char* kTestInterfaceId = "cantrip_test";

std::string toLowerId(const std::string& name) {
    // "PCAN_USBBUS1" -> "pcan_usbbus1"
    std::string out = name;
    for (char& c : out) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return out;
}

// Interfaces are namespaced by backend so identically-named channels from
// two different vendors (unlikely, but not impossible) can't collide, e.g.
// "peak_pcan_usbbus1".
std::string interfaceIdFor(const ICanBackend& backend, const CanChannelInfo& channel) {
    return backend.id() + "_" + toLowerId(channel.name);
}

std::vector<std::string> collectArgs(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return args;
}

bool hasFlag(const std::vector<std::string>& args, const std::string& flag) {
    for (auto& a : args) if (a == flag) return true;
    return false;
}

std::string getOption(const std::vector<std::string>& args, const std::string& name, const std::string& def = "") {
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == name && i + 1 < args.size()) return args[i + 1];
    }
    return def;
}

void printExtcapInterfaces() {
    std::printf("extcap {version=1.0}{help=https://github.com/}{display=CANtrip CAN bridge}\n");

    for (auto& backend : cantrip::probeAvailableBackends()) {
        for (const CanChannelInfo& ch : backend->enumerateChannels()) {
            std::string id = interfaceIdFor(*backend, ch);
            std::printf("interface {value=%s}{display=%s %s}\n",
                        id.c_str(), backend->displayName().c_str(), ch.name.c_str());
        }
    }
    // Always offer the synthetic test interface so the pipeline can be
    // verified end-to-end without any CAN hardware or vendor drivers
    // installed.
    std::printf("interface {value=%s}{display=CANtrip synthetic test source (no hardware needed)}\n",
                 kTestInterfaceId);
}

void printExtcapDlts(const std::string& /*interfaceId*/) {
    std::printf("dlt {number=%u}{name=CAN_SOCKETCAN}{display=SocketCAN}\n", kLinktypeCanSocketcan);
}

void printExtcapConfig(const std::string& /*interfaceId*/) {
    // "Usual" bitrate presets as a selector; "Expert" raw FD init string as
    // a free-text field. The Qt app talks to pcan2pcap the same way
    // (invoking with --bitrate/--fd/--data-bitrate/--expert-string), this
    // config listing is what makes the tool also usable stand-alone from
    // within Wireshark's own capture-options dialog. The expert string's
    // exact format is backend-specific (see CanBitrateConfig::expertInitString).
    std::printf("arg {number=0}{call=--bitrate}{display=Bitrate}{type=selector}"
                "{tooltip=Classic CAN nominal bitrate}\n");
    std::printf("value {arg=0}{value=125000}{display=125 kbit/s}{default=false}\n");
    std::printf("value {arg=0}{value=250000}{display=250 kbit/s}{default=false}\n");
    std::printf("value {arg=0}{value=500000}{display=500 kbit/s}{default=true}\n");
    std::printf("value {arg=0}{value=1000000}{display=1 Mbit/s}{default=false}\n");

    std::printf("arg {number=1}{call=--fd}{display=Enable CAN FD}{type=boolflag}{default=false}\n");

    std::printf("arg {number=2}{call=--data-bitrate}{display=FD data bitrate}{type=selector}"
                "{tooltip=Only used when CAN FD is enabled}\n");
    std::printf("value {arg=2}{value=2000000}{display=2 Mbit/s}{default=true}\n");
    std::printf("value {arg=2}{value=5000000}{display=5 Mbit/s}{default=false}\n");

    std::printf("arg {number=3}{call=--expert-string}{display=Expert FD init string}{type=string}"
                "{tooltip=Raw backend-specific FD init string; overrides bitrate/data-bitrate "
                "when non-empty. For the PEAK backend, example: f_clock_mhz=80,nom_brp=2,"
                "nom_tseg1=63,nom_tseg2=16,nom_sjw=16,data_brp=2,data_tseg1=15,data_tseg2=4,data_sjw=4}\n");

    std::printf("arg {number=4}{call=--test-mode}{display=Synthetic test source}{type=boolflag}"
                "{default=false}{tooltip=Generate fake frames instead of reading real hardware}\n");
}

#pragma pack(push, 1)
struct PcapGlobalHeader {
    uint32_t magic = 0xa1b2c3d4;
    uint16_t versionMajor = 2;
    uint16_t versionMinor = 4;
    int32_t thiszone = 0;
    uint32_t sigfigs = 0;
    uint32_t snaplen = 72;
    uint32_t network = kLinktypeCanSocketcan;
};
struct PcapRecordHeader {
    uint32_t tsSec;
    uint32_t tsUsec;
    uint32_t inclLen;
    uint32_t origLen;
};
#pragma pack(pop)

uint32_t hostToNetwork32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8)
         | ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

constexpr uint32_t kCanEffFlag = 0x80000000u;
constexpr uint32_t kCanRtrFlag = 0x40000000u;
constexpr uint32_t kCanErrFlag = 0x20000000u;
constexpr uint32_t kCanSffMask = 0x000007FFu;
constexpr uint32_t kCanEffMask = 0x1FFFFFFFu;
constexpr uint32_t kCanErrMask = 0x1FFFFFFFu;

// Serializes one frame into the SocketCAN on-wire layout (network byte
// order), returning the byte length (16 for classic, 72 for FD).
size_t serializeFrame(const CanFrame& f, uint8_t* buf) {
    uint32_t canId;
    if (f.error) {
        // Error frames don't have a normal address - `f.id` is already an
        // error-class bitmask (CAN_ERR_* from linux/can/error.h), so it's
        // used as-is rather than treated as an 11/29-bit identifier.
        canId = kCanErrFlag | (f.id & kCanErrMask);
    } else {
        canId = (f.extended ? (f.id & kCanEffMask) : (f.id & kCanSffMask));
        if (f.extended) canId |= kCanEffFlag;
        if (f.rtr) canId |= kCanRtrFlag;
    }
    uint32_t canIdNet = hostToNetwork32(canId);
    std::memcpy(buf, &canIdNet, 4);

    if (!f.fd) {
        buf[4] = f.dlc;       // can_dlc
        buf[5] = 0;           // __pad
        buf[6] = 0;           // __res0
        buf[7] = 0;           // __res1
        std::memcpy(buf + 8, f.data, 8);
        return 16;
    }
    buf[4] = f.dlc;           // len
    buf[5] = (f.brs ? 0x01 : 0) | (f.esi ? 0x02 : 0); // flags
    buf[6] = 0;               // __res0
    buf[7] = 0;               // __res1
    std::memcpy(buf + 8, f.data, 64);
    return 72;
}

void writeRecord(FILE* out, const CanFrame& f) {
    uint8_t payload[72];
    size_t len = serializeFrame(f, payload);

    PcapRecordHeader rh{};
    rh.tsSec = static_cast<uint32_t>(f.timestampUs / 1000000ull);
    rh.tsUsec = static_cast<uint32_t>(f.timestampUs % 1000000ull);
    rh.inclLen = static_cast<uint32_t>(len);
    rh.origLen = static_cast<uint32_t>(len);

    std::fwrite(&rh, sizeof(rh), 1, out);
    std::fwrite(payload, len, 1, out);
    std::fflush(out);
}

CanFrame makeSyntheticFrame(uint64_t& tUs, uint32_t& counter) {
    static const uint32_t ids[] = {0x100, 0x200, 0x300, 0x7E8};
    CanFrame f;
    f.id = ids[counter % 4];
    f.extended = false;
    f.dlc = 8;
    for (int i = 0; i < 8; ++i) f.data[i] = static_cast<uint8_t>((counter + i) & 0xFF);
    f.timestampUs = tUs;
    ++counter;
    tUs += 100000; // 100ms between synthetic frames
    return f;
}

// CAN_ERR_PROT (bit 3 of the error class) and its data[2] protocol-violation
// sub-flags, straight from linux/can/error.h - this is a long-stable public
// kernel uAPI (also what Wireshark's own dissector decodes), not a vendor
// format, so unlike a proprietary DLL struct it's safe to hardcode from
// memory. Verified regardless: a hand-crafted frame using these exact bit
// values decoded as "can_can_err_prot_type_stuff" etc. under a real tshark.
constexpr uint32_t kCanErrProt = 0x00000008u;
constexpr uint8_t kErrProtBit = 0x01;
constexpr uint8_t kErrProtForm = 0x02;
constexpr uint8_t kErrProtStuff = 0x04;
constexpr uint8_t kErrProtOverload = 0x20;

// Cycles through a few protocol-violation error frames so the synthetic
// source can demonstrate CANtrip's bus-error display without needing real
// hardware to actually misbehave on the bus.
CanFrame makeSyntheticErrorFrame(uint64_t& tUs, uint32_t errorIndex) {
    static const uint8_t protTypes[] = {kErrProtBit, kErrProtForm, kErrProtStuff, kErrProtOverload};
    CanFrame f;
    f.error = true;
    f.id = kCanErrProt;
    f.dlc = 8;
    f.data[2] = protTypes[errorIndex % 4];
    f.timestampUs = tUs;
    return f;
}

// Finds which backend+channel an extcap interface ID refers to by
// re-enumerating every available backend and matching the same
// backend-namespaced ID scheme used in printExtcapInterfaces(). Returns the
// owning backend (so it stays alive for the capture loop) and sets
// *channelId; backend is null if not found.
std::unique_ptr<ICanBackend> resolveInterface(const std::string& interfaceId, uint64_t* channelId) {
    for (auto& backend : cantrip::probeAvailableBackends()) {
        for (const CanChannelInfo& ch : backend->enumerateChannels()) {
            if (interfaceIdFor(*backend, ch) == interfaceId) {
                *channelId = ch.channelId;
                return std::move(backend);
            }
        }
    }
    return nullptr;
}

int runCapture(const std::vector<std::string>& args, const std::string& interfaceId) {
    std::string fifoPath = getOption(args, "--fifo");
    if (fifoPath.empty()) {
        std::fprintf(stderr, "pcan2pcap: --capture requires --fifo <path>\n");
        return 1;
    }

    bool testMode = hasFlag(args, "--test-mode") || interfaceId == kTestInterfaceId;

    FILE* fifo = std::fopen(fifoPath.c_str(), "wb");
    if (!fifo) {
        std::fprintf(stderr, "pcan2pcap: failed to open fifo '%s'\n", fifoPath.c_str());
        return 1;
    }

    PcapGlobalHeader gh{};
    std::fwrite(&gh, sizeof(gh), 1, fifo);
    std::fflush(fifo);

    if (testMode) {
        uint64_t tUs = 0;
        uint32_t counter = 0;
        while (true) {
            writeRecord(fifo, makeSyntheticFrame(tUs, counter));
            // Every 8th tick, also inject a synthetic bus error frame so
            // CANtrip's error display can be exercised without needing real
            // hardware to actually fault on the bus.
            if (counter % 8 == 0) {
                writeRecord(fifo, makeSyntheticErrorFrame(tUs, counter / 8));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    CanBitrateConfig config;
    config.fd = hasFlag(args, "--fd");
    config.nominalBitrateBps = static_cast<uint32_t>(
        std::strtol(getOption(args, "--bitrate", "500000").c_str(), nullptr, 10));
    config.dataBitrateBps = static_cast<uint32_t>(
        std::strtol(getOption(args, "--data-bitrate", "2000000").c_str(), nullptr, 10));
    config.expertInitString = getOption(args, "--expert-string");

    uint64_t channelId = 0;
    std::unique_ptr<ICanBackend> backend = resolveInterface(interfaceId, &channelId);
    if (!backend) {
        std::fprintf(stderr, "pcan2pcap: unknown or unavailable interface '%s'\n", interfaceId.c_str());
        std::fclose(fifo);
        return 1;
    }

    std::string err;
    if (!backend->initialize(channelId, config, &err)) {
        std::fprintf(stderr, "pcan2pcap: initialize failed: %s\n", err.c_str());
        std::fclose(fifo);
        return 1;
    }

    while (true) {
        CanFrame frame;
        bool got = backend->readFrame(channelId, &frame, &err);
        if (got) {
            writeRecord(fifo, frame);
        } else if (!err.empty()) {
            std::fprintf(stderr, "pcan2pcap: read error: %s\n", err.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args = collectArgs(argc, argv);
    std::string interfaceId = getOption(args, "--extcap-interface");

    if (hasFlag(args, "--extcap-interfaces")) {
        printExtcapInterfaces();
        return 0;
    }
    if (hasFlag(args, "--extcap-dlts")) {
        printExtcapDlts(interfaceId);
        return 0;
    }
    if (hasFlag(args, "--extcap-config") && !hasFlag(args, "--capture")) {
        printExtcapConfig(interfaceId);
        return 0;
    }
    if (hasFlag(args, "--capture")) {
        return runCapture(args, interfaceId);
    }

    std::fprintf(stderr,
        "pcan2pcap: Wireshark extcap bridge for CANtrip's CAN hardware backends.\n"
        "This program is meant to be invoked by Wireshark/tshark/dumpcap,\n"
        "not run directly. See --extcap-interfaces.\n");
    return 1;
}
