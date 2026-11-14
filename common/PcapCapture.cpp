#include "PcapCapture.h"

#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>

namespace cantrip {

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

#pragma pack(push, 1)
struct PcapGlobalHeader {
    uint32_t magic = 0xa1b2c3d4;
    uint16_t versionMajor = 2;
    uint16_t versionMinor = 4;
    int32_t thiszone = 0;
    uint32_t sigfigs = 0;
    uint32_t snaplen = 72;
    uint32_t network = kPcapLinktypeCanSocketcan;
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

bool writePcapRecord(FILE* out, const CanFrame& f) {
    uint8_t payload[72];
    size_t len = serializeFrame(f, payload);

    PcapRecordHeader rh{};
    rh.tsSec = static_cast<uint32_t>(f.timestampUs / 1000000ull);
    rh.tsUsec = static_cast<uint32_t>(f.timestampUs % 1000000ull);
    rh.inclLen = static_cast<uint32_t>(len);
    rh.origLen = static_cast<uint32_t>(len);

    return std::fwrite(&rh, sizeof(rh), 1, out) == 1 && std::fwrite(payload, len, 1, out) == 1 && std::fflush(out) == 0;
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
    for (auto& backend : probeAvailableBackends()) {
        for (const CanChannelInfo& ch : backend->enumerateChannels()) {
            if (interfaceIdFor(*backend, ch) == interfaceId) {
                *channelId = ch.channelId;
                return std::move(backend);
            }
        }
    }
    return nullptr;
}

bool writePcapHeader(FILE* out) {
    PcapGlobalHeader gh{};
    return std::fwrite(&gh, sizeof(gh), 1, out) == 1 && std::fflush(out) == 0;
}

void runSyntheticCapture(FILE* out, const std::atomic<bool>& stop) {
    uint64_t tUs = 0;
    uint32_t counter = 0;
    while (!stop) {
        const uint64_t tickUs = tUs;
        bool ok = writePcapRecord(out, makeSyntheticFrame(tUs, counter));
        ok = ok && writePcapRecord(out, makeSyntheticWaveformFrame(tickUs));
        // Every 8th tick, also inject a synthetic bus error frame so
        // CANtrip's error display can be exercised without needing real
        // hardware to actually fault on the bus.
        if (counter % 8 == 0) ok = ok && writePcapRecord(out, makeSyntheticErrorFrame(tUs, counter / 8));
        if (!ok) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool runBackendCapture(IAvlabsCanBackend& backend, uint64_t channelId, const std::string& interfaceId,
                       const CanBitrateConfig& config, bool requestOwnership, FILE* out,
                       const std::atomic<bool>& stop, const CaptureMessageCallback& onMessage, std::string* error) {
    const auto report = [&onMessage](const std::string& message) {
        if (onMessage) onMessage(message);
    };

    std::string err;
    if (!backend.initialize(channelId, config, requestOwnership, &err)) {
        if (error) *error = "initialize failed: " + err;
        return false;
    }

    // Send Message's PEAK transmit path - see createTxPipe()'s comment for
    // why PEAK needs this and Vector doesn't. Failure is non-fatal: capture
    // continues normally, Send Message just won't be able to connect.
    HANDLE txPipe = INVALID_HANDLE_VALUE;
    if (backend.id() == "peak") {
        txPipe = createTxPipe(interfaceId);
        if (txPipe == INVALID_HANDLE_VALUE) {
            report("could not create transmit pipe (Send Message unavailable this session)");
        }
    }

    while (!stop) {
        CanFrame frame;
        err.clear();
        const bool got = backend.readFrame(channelId, &frame, &err);
        if (got) {
            if (!writePcapRecord(out, frame)) break; // nothing is reading anymore
        } else if (!err.empty()) {
            report("read error: " + err);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        CanFrame txFrame;
        if (tryReadTxPipe(txPipe, &txFrame)) {
            std::string txErr;
            if (!backend.writeFrame(channelId, txFrame, &txErr)) report("transmit error: " + txErr);
        }
    }

    if (txPipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(txPipe);
        CloseHandle(txPipe);
    }
    // In its own process this loop used to end by being killed. Inside
    // CANtrip the channel has to be handed back, or the next capture on it
    // would fail to initialize.
    backend.uninitialize(channelId);
    return true;
}

PcapPipeFeeder::~PcapPipeFeeder() {
    requestStop();
    join();
}

bool PcapPipeFeeder::start(const Config& config, CaptureMessageCallback onMessage, std::string* error) {
    join();
    stop_ = false;

    // Unique per process and per capture: a previous capture's reader may
    // not have let go of the old name yet.
    static std::atomic<unsigned> counter{0};
    pipeName_ = "\\\\.\\pipe\\cantrip_capture_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(++counter);

    // PIPE_NOWAIT only for the wait for a reader, so that wait can be given
    // up when a stop is requested. run() switches to blocking writes after.
    HANDLE pipe = CreateNamedPipeA(pipeName_.c_str(), PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_NOWAIT,
                                   1, 64 * 1024, 0, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        if (error) *error = "could not create the capture pipe (error " + std::to_string(GetLastError()) + ")";
        return false;
    }

    thread_ = std::thread(&PcapPipeFeeder::run, this, config, std::move(onMessage), pipe);
    return true;
}

void PcapPipeFeeder::join() {
    if (thread_.joinable()) thread_.join();
}

void PcapPipeFeeder::run(Config config, CaptureMessageCallback onMessage, void* pipeHandle) {
    HANDLE pipe = static_cast<HANDLE>(pipeHandle);

    while (true) {
        if (stop_) {
            CloseHandle(pipe);
            return;
        }
        if (ConnectNamedPipe(pipe, nullptr) == 0 && GetLastError() == ERROR_PIPE_CONNECTED) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    // The C runtime owns the handle from here: fclose() below closes it.
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(pipe), _O_BINARY);
    FILE* out = fd >= 0 ? _fdopen(fd, "wb") : nullptr;
    if (!out) {
        if (fd >= 0) _close(fd);
        else CloseHandle(pipe);
        if (onMessage) onMessage("could not open the capture pipe for writing");
        return;
    }

    if (writePcapHeader(out)) {
        if (config.interfaceId == kSyntheticInterfaceId) {
            runSyntheticCapture(out, stop_);
        } else {
            uint64_t channelId = 0;
            std::unique_ptr<IAvlabsCanBackend> backend = resolveInterface(config.interfaceId, &channelId);
            std::string err;
            if (!backend) {
                if (onMessage) onMessage("unknown or unavailable interface '" + config.interfaceId + "'");
            } else if (!runBackendCapture(*backend, channelId, config.interfaceId, config.bitrate,
                                          config.requestOwnership, out, stop_, onMessage, &err)) {
                if (onMessage) onMessage(err);
            }
        }
    }
    // Closing the pipe is what tells tshark the capture is over.
    std::fclose(out);
}

} // namespace cantrip
