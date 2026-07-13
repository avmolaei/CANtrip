// can2pcap - Wireshark extcap bridge for CANtrip's CAN hardware backends.
// Bridges *any* IAvlabsCanBackend CANtrip has probed as available - see
// ../common/AVlabsCanBackend.h for the vendor-neutral interface and
// ../common/CanBackendRegistry.cpp for the list of backends it tries.
// (Named pcan2pcap prior to CANtrip 1.1 - a leftover from a PEAK-only
// prototype, before the vendor-neutral backend abstraction existed.)
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

#include <cctype>
#include <cmath>
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
using cantrip::IAvlabsCanBackend;

constexpr uint16_t kLinktypeCanSocketcan = 227;
constexpr const char* kTestInterfaceId = "cantrip_test";

// "PCAN_USBBUS1" -> "pcan_usbbus1", "PCAN-USB FD" -> "pcan_usb_fd". Every
// non-alphanumeric character collapses to a single underscore - required
// because the resulting ID is used both as a tshark -i argument AND as a
// preference NAME in `-o extcap.<id>.<key>:<value>` (see runCapture()).
// tshark's preference parser breaks on a raw space in that name (a real
// device, PEAK's "PCAN-USB FD", triggered this: "-o" was rejected with
// "specifies unknown preference" once the space reached tshark unescaped).
std::string sanitizeId(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(tolower(static_cast<unsigned char>(c)));
        } else if (!out.empty() && out.back() != '_') {
            out += '_';
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

// Interfaces are namespaced by backend so identically-named channels from
// two different vendors (unlikely, but not impossible) can't collide, e.g.
// "peak_pcan_usbbus1".
std::string interfaceIdFor(const IAvlabsCanBackend& backend, const CanChannelInfo& channel) {
    return backend.id() + "_" + sanitizeId(channel.name);
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
    // "Usual" bitrate presets as a selector; FD bit-timing (tick values, not
    // a free-text expert string) as plain integer fields. The Qt app talks
    // to can2pcap the same way (invoking with --bitrate/--fd/--data-bitrate/
    // --nom-*/--data-*), this config listing is what makes the tool also
    // usable stand-alone from within Wireshark's own capture-options dialog.
    // Values come from CANtrip's bit-timing calculator (see CanBitTiming.h);
    // manually-entered "expert" values work the same way from either side.
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

    std::printf("arg {number=3}{call=--nom-brp}{display=Nominal BRP}{type=integer}{default=2}"
                "{tooltip=FD nominal phase bitrate prescaler}\n");
    std::printf("arg {number=4}{call=--nom-tseg1}{display=Nominal TSEG1}{type=integer}{default=63}"
                "{tooltip=FD nominal phase time segment 1}\n");
    std::printf("arg {number=5}{call=--nom-tseg2}{display=Nominal TSEG2}{type=integer}{default=16}"
                "{tooltip=FD nominal phase time segment 2}\n");
    std::printf("arg {number=6}{call=--nom-sjw}{display=Nominal SJW}{type=integer}{default=16}"
                "{tooltip=FD nominal phase sync jump width}\n");
    std::printf("arg {number=7}{call=--data-brp}{display=Data BRP}{type=integer}{default=2}"
                "{tooltip=FD data phase bitrate prescaler}\n");
    std::printf("arg {number=8}{call=--data-tseg1}{display=Data TSEG1}{type=integer}{default=15}"
                "{tooltip=FD data phase time segment 1}\n");
    std::printf("arg {number=9}{call=--data-tseg2}{display=Data TSEG2}{type=integer}{default=4}"
                "{tooltip=FD data phase time segment 2}\n");
    std::printf("arg {number=10}{call=--data-sjw}{display=Data SJW}{type=integer}{default=4}"
                "{tooltip=FD data phase sync jump width}\n");

    std::printf("arg {number=11}{call=--test-mode}{display=Synthetic test source}{type=boolflag}"
                "{default=false}{tooltip=Generate fake frames instead of reading real hardware}\n");

    std::printf("arg {number=12}{call=--listen-only}{display=Listen-only (no bus configuration)}"
                "{type=boolflag}{default=false}{tooltip=Join this channel without requesting "
                "exclusive configuration rights, for use alongside another app that already "
                "configured the bus}\n");
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

// CAN FD's DLC is a 4-bit *code*, not a byte count - codes 9-15 map
// non-linearly to 12/16/20/24/32/48/64 bytes (ISO 11898-1). CanFrame::dlc
// deliberately stores the raw code straight from the vendor driver
// (matching real hardware/spec terminology - see its doc comment in
// AVlabsCanBackend.h), but the SocketCAN wire format's `len` byte - which
// Linux's kernel and Wireshark's own dissector both define as an actual byte
// count, 0-64 - needs the translated value. Confirmed against a real,
// actively-transmitting CAN FD bus (Vector VN7640): the untranslated code
// silently truncated the majority of real frames (most real ECUs use DLC
// codes 9-15 routinely, not just 0-8), with the true remaining bytes
// discarded into Wireshark's own "can_can_padding" field, which CANtrip
// never reads. Converted once here (the single serialization chokepoint)
// rather than duplicated in every vendor backend.
uint8_t fdDlcCodeToLength(uint8_t code) {
    static const uint8_t kLengths[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
    return code < 16 ? kLengths[code] : 64;
}

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
    buf[4] = fdDlcCodeToLength(f.dlc); // len (real byte count, not the raw DLC code)
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

// PEAK-only transmit path for Send Message (app/MessageSender.cpp is the
// client side): PCAN-Basic has no equivalent to Vector XL's permission-mask
// concept, so a second process/handle can't join a channel this process
// already initialized (confirmed for real - CAN_Initialize on an
// already-active channel returns PCAN_ERROR_INITIALIZE). The only handle
// that can transmit is the one this process already holds, so instead
// CANtrip's app process sends frames here over a named pipe, and this
// process writes them out on its own already-initialized handle.
//
// PIPE_NOWAIT (non-blocking ConnectNamedPipe/ReadFile) rather than
// overlapped I/O - keeps this in the same simple synchronous polling style
// as the rest of this file's capture loop, for what's a low-frequency,
// internal-only control channel; not Microsoft's modern-recommended
// approach, but sufficient here and far simpler to integrate.
HANDLE createTxPipe(const std::string& interfaceId) {
    const std::string pipeName = "\\\\.\\pipe\\cantrip_tx_" + interfaceId;
    HANDLE pipe = CreateNamedPipeA(
        pipeName.c_str(),
        PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
        1,                  // max instances
        0,                  // out buffer size (inbound-only, unused)
        sizeof(CanFrame) * 4, // in buffer size, room for a few queued frames
        0,                  // default timeout
        nullptr);
    return pipe; // INVALID_HANDLE_VALUE on failure - non-fatal, checked by caller
}

// Non-blocking: accepts a client connection if one is pending (no-op if
// already connected or nothing waiting), then checks for a complete
// CanFrame's worth of bytes and reads it if so. Returns true and fills
// *frame only when a full frame was actually read this call.
bool tryReadTxPipe(HANDLE pipe, CanFrame* frame) {
    if (pipe == INVALID_HANDLE_VALUE) return false;

    // PIPE_NOWAIT makes this return immediately regardless of outcome -
    // ERROR_PIPE_LISTENING (no client yet) and ERROR_NO_DATA are both
    // expected, not real failures, so their return value is intentionally
    // ignored here.
    ConnectNamedPipe(pipe, nullptr);

    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
    if (available < sizeof(CanFrame)) return false;

    DWORD bytesRead = 0;
    if (!ReadFile(pipe, frame, sizeof(CanFrame), &bytesRead, nullptr)) return false;
    return bytesRead == sizeof(CanFrame);
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

// Smoothly time-varying signals (sine/sawtooth/triangle) for exercising the
// Graph view without real hardware - matches test/sample.dbc's TestWaveforms
// message (0x400 = 1024). Unlike makeSyntheticFrame's incrementing-byte
// payload (fine for the trace view, useless for a chart demo), these are
// real waveforms on distinguishable scales.
CanFrame makeSyntheticWaveformFrame(uint64_t tUs) {
    const double t = static_cast<double>(tUs) / 1e6; // seconds
    constexpr double kPi = 3.14159265358979323846;

    // Sine: amplitude 50 about 0, period 10s -> fits the signal's [-100,100]
    // range with headroom.
    const double sine = 50.0 * std::sin(2.0 * kPi * 0.1 * t);
    // Sawtooth: ramps 0 -> 100 every 5s, then resets.
    const double sawtooth = 100.0 * std::fmod(t * 0.2, 1.0);
    // Triangle: ramps 0 -> 100 -> 0 every ~6.7s.
    const double trianglePhase = std::fmod(t * 0.15, 1.0);
    const double triangle = 100.0 * (trianglePhase < 0.5 ? trianglePhase * 2.0 : 2.0 - trianglePhase * 2.0);

    auto toRawU16 = [](double phys, double offset, double scale) -> uint16_t {
        return static_cast<uint16_t>(std::lround((phys - offset) / scale));
    };
    const uint16_t sineRaw = toRawU16(sine, -100.0, 0.01);
    const uint16_t sawtoothRaw = toRawU16(sawtooth, 0.0, 0.01);
    const uint16_t triangleRaw = toRawU16(triangle, 0.0, 0.01);

    CanFrame f;
    f.id = 0x400; // TestWaveforms, see test/sample.dbc
    f.extended = false;
    f.dlc = 8;
    f.data[0] = static_cast<uint8_t>(sineRaw & 0xFF);
    f.data[1] = static_cast<uint8_t>((sineRaw >> 8) & 0xFF);
    f.data[2] = static_cast<uint8_t>(sawtoothRaw & 0xFF);
    f.data[3] = static_cast<uint8_t>((sawtoothRaw >> 8) & 0xFF);
    f.data[4] = static_cast<uint8_t>(triangleRaw & 0xFF);
    f.data[5] = static_cast<uint8_t>((triangleRaw >> 8) & 0xFF);
    f.timestampUs = tUs;
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
std::unique_ptr<IAvlabsCanBackend> resolveInterface(const std::string& interfaceId, uint64_t* channelId) {
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
        std::fprintf(stderr, "can2pcap: --capture requires --fifo <path>\n");
        return 1;
    }

    bool testMode = hasFlag(args, "--test-mode") || interfaceId == kTestInterfaceId;

    FILE* fifo = std::fopen(fifoPath.c_str(), "wb");
    if (!fifo) {
        std::fprintf(stderr, "can2pcap: failed to open fifo '%s'\n", fifoPath.c_str());
        return 1;
    }

    PcapGlobalHeader gh{};
    std::fwrite(&gh, sizeof(gh), 1, fifo);
    std::fflush(fifo);

    if (testMode) {
        uint64_t tUs = 0;
        uint32_t counter = 0;
        while (true) {
            const uint64_t tickUs = tUs;
            writeRecord(fifo, makeSyntheticFrame(tUs, counter));
            writeRecord(fifo, makeSyntheticWaveformFrame(tickUs));
            // Every 8th tick, also inject a synthetic bus error frame so
            // CANtrip's error display can be exercised without needing real
            // hardware to actually fault on the bus.
            if (counter % 8 == 0) {
                writeRecord(fifo, makeSyntheticErrorFrame(tUs, counter / 8));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    auto getUint = [&](const char* name, const char* def) {
        return static_cast<uint32_t>(std::strtol(getOption(args, name, def).c_str(), nullptr, 10));
    };

    // Listen-only: join this channel without requesting exclusive
    // bus-configuration rights, assuming another app has already configured
    // it (see IAvlabsCanBackend::initialize's requestOwnership parameter).
    // Threaded from MainWindow's CAN Controller dialog "Request bus
    // configuration" checkbox down through TsharkCapture's launch args.
    const bool requestOwnership = !hasFlag(args, "--listen-only");

    CanBitrateConfig config;
    config.fd = hasFlag(args, "--fd");
    config.nominalBitrateBps = getUint("--bitrate", "500000");
    config.dataBitrateBps = getUint("--data-bitrate", "2000000");
    config.nominalTiming.brp = getUint("--nom-brp", "2");
    config.nominalTiming.tseg1 = getUint("--nom-tseg1", "63");
    config.nominalTiming.tseg2 = getUint("--nom-tseg2", "16");
    config.nominalTiming.sjw = getUint("--nom-sjw", "16");
    config.dataTiming.brp = getUint("--data-brp", "2");
    config.dataTiming.tseg1 = getUint("--data-tseg1", "15");
    config.dataTiming.tseg2 = getUint("--data-tseg2", "4");
    config.dataTiming.sjw = getUint("--data-sjw", "4");

    uint64_t channelId = 0;
    std::unique_ptr<IAvlabsCanBackend> backend = resolveInterface(interfaceId, &channelId);
    if (!backend) {
        std::fprintf(stderr, "can2pcap: unknown or unavailable interface '%s'\n", interfaceId.c_str());
        std::fclose(fifo);
        return 1;
    }

    std::string err;
    if (!backend->initialize(channelId, config, requestOwnership, &err)) {
        std::fprintf(stderr, "can2pcap: initialize failed: %s\n", err.c_str());
        std::fclose(fifo);
        return 1;
    }

    // Send Message's PEAK transmit path - see createTxPipe()'s comment for
    // why PEAK needs this and Vector doesn't. Failure is non-fatal: capture
    // continues normally, Send Message just won't be able to connect.
    HANDLE txPipe = INVALID_HANDLE_VALUE;
    if (backend->id() == "peak") {
        txPipe = createTxPipe(interfaceId);
        if (txPipe == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "can2pcap: could not create transmit pipe (Send Message unavailable this session)\n");
        }
    }

    while (true) {
        CanFrame frame;
        bool got = backend->readFrame(channelId, &frame, &err);
        if (got) {
            writeRecord(fifo, frame);
        } else if (!err.empty()) {
            std::fprintf(stderr, "can2pcap: read error: %s\n", err.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        CanFrame txFrame;
        if (tryReadTxPipe(txPipe, &txFrame)) {
            std::string txErr;
            if (!backend->writeFrame(channelId, txFrame, &txErr)) {
                std::fprintf(stderr, "can2pcap: transmit error: %s\n", txErr.c_str());
            }
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
        "can2pcap: Wireshark extcap bridge for CANtrip's CAN hardware backends.\n"
        "This program is meant to be invoked by Wireshark/tshark/dumpcap,\n"
        "not run directly. See --extcap-interfaces.\n");
    return 1;
}
