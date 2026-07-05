#include "PeakBackend.h"

#include <array>
#include <cstdlib>
#include <cstring>

namespace cantrip {

namespace {

// Channels PCAN_ATTACHED_CHANNELS can report on; we probe these handles
// directly with PCAN_CHANNEL_CONDITION as a fallback for PCAN-Basic builds
// too old to support PCAN_ATTACHED_CHANNELS.
constexpr std::array<TPCANHandle, 8> kProbeHandles = {
    PCAN_USBBUS1, PCAN_USBBUS2, PCAN_USBBUS3, PCAN_USBBUS4,
    PCAN_USBBUS5, PCAN_USBBUS6, PCAN_USBBUS7, PCAN_USBBUS8,
};

const char* handleName(TPCANHandle h) {
    switch (h) {
        case PCAN_USBBUS1: return "PCAN_USBBUS1";
        case PCAN_USBBUS2: return "PCAN_USBBUS2";
        case PCAN_USBBUS3: return "PCAN_USBBUS3";
        case PCAN_USBBUS4: return "PCAN_USBBUS4";
        case PCAN_USBBUS5: return "PCAN_USBBUS5";
        case PCAN_USBBUS6: return "PCAN_USBBUS6";
        case PCAN_USBBUS7: return "PCAN_USBBUS7";
        case PCAN_USBBUS8: return "PCAN_USBBUS8";
        default: return "PCAN_UNKNOWN";
    }
}

template <typename FnPtr>
bool bindProc(HMODULE mod, const char* name, FnPtr* out) {
    *out = reinterpret_cast<FnPtr>(GetProcAddress(mod, name));
    return *out != nullptr;
}

// PCAN-Basic classic init only accepts one of a fixed set of predefined
// baud rates, not an arbitrary bps value; map to the nearest one at or
// below the requested rate.
TPCANBaudrate nearestClassicBaud(uint32_t bps) {
    if (bps <= 5000) return PCAN_BAUD_5K;
    if (bps <= 10000) return PCAN_BAUD_10K;
    if (bps <= 20000) return PCAN_BAUD_20K;
    if (bps <= 33000) return PCAN_BAUD_33K;
    if (bps <= 47000) return PCAN_BAUD_47K;
    if (bps <= 50000) return PCAN_BAUD_50K;
    if (bps <= 83000) return PCAN_BAUD_83K;
    if (bps <= 95000) return PCAN_BAUD_95K;
    if (bps <= 100000) return PCAN_BAUD_100K;
    if (bps <= 125000) return PCAN_BAUD_125K;
    if (bps <= 250000) return PCAN_BAUD_250K;
    if (bps <= 500000) return PCAN_BAUD_500K;
    if (bps <= 800000) return PCAN_BAUD_800K;
    return PCAN_BAUD_1M;
}

} // namespace

PeakBackend::PeakBackend(HMODULE module) : module_(module) {}

std::unique_ptr<PeakBackend> PeakBackend::load(std::string* error) {
    HMODULE mod = LoadLibraryA("PCANBasic.dll");
    if (!mod) {
        if (error) *error = "PCANBasic.dll not found. Install the PEAK-System "
                             "PCAN-Basic driver package.";
        return nullptr;
    }

    auto backend = std::unique_ptr<PeakBackend>(new PeakBackend(mod));
    bool ok = true;
    ok &= bindProc(mod, "CAN_Initialize", &backend->pInitialize_);
    ok &= bindProc(mod, "CAN_InitializeFD", &backend->pInitializeFD_);
    ok &= bindProc(mod, "CAN_Uninitialize", &backend->pUninitialize_);
    ok &= bindProc(mod, "CAN_Read", &backend->pRead_);
    ok &= bindProc(mod, "CAN_ReadFD", &backend->pReadFD_);
    ok &= bindProc(mod, "CAN_Write", &backend->pWrite_);
    ok &= bindProc(mod, "CAN_WriteFD", &backend->pWriteFD_);
    ok &= bindProc(mod, "CAN_GetValue", &backend->pGetValue_);
    ok &= bindProc(mod, "CAN_SetValue", &backend->pSetValue_);
    ok &= bindProc(mod, "CAN_GetErrorText", &backend->pGetErrorText_);

    if (!ok) {
        if (error) *error = "PCANBasic.dll found but missing expected exports; "
                             "wrong DLL version?";
        FreeLibrary(mod);
        return nullptr;
    }
    return backend;
}

std::string PeakBackend::describeStatus(TPCANStatus status) const {
    char buf[256] = {};
    if (pGetErrorText_ && pGetErrorText_(status, 0, buf) == PCAN_ERROR_OK) {
        return std::string(buf);
    }
    return "PCAN error 0x" + std::to_string(status);
}

std::vector<CanChannelInfo> PeakBackend::enumerateChannels() const {
    std::vector<CanChannelInfo> result;
    for (TPCANHandle h : kProbeHandles) {
        DWORD condition = PCAN_CHANNEL_UNAVAILABLE;
        TPCANStatus status = pGetValue_(h, PCAN_CHANNEL_CONDITION, &condition, sizeof(condition));
        if (status != PCAN_ERROR_OK) continue;
        if (condition == PCAN_CHANNEL_UNAVAILABLE) continue;
        result.push_back(CanChannelInfo{
            static_cast<uint64_t>(h), handleName(h), condition == PCAN_CHANNEL_AVAILABLE});
    }
    return result;
}

bool PeakBackend::initialize(uint64_t channelId, const CanBitrateConfig& config, std::string* error) {
    auto handle = static_cast<TPCANHandle>(channelId);
    bool ok = config.fd
        ? initializeFd(handle, !config.expertInitString.empty() ? config.expertInitString
              : ("f_clock_mhz=80,nom_brp=2,nom_tseg1=63,nom_tseg2=16,nom_sjw=16,"
                 "data_brp=2,data_tseg1=15,data_tseg2=4,data_sjw=4"), error)
        : initializeClassic(handle, nearestClassicBaud(config.nominalBitrateBps), error);
    if (ok) fdByChannel_[handle] = config.fd;
    return ok;
}

void PeakBackend::uninitialize(uint64_t channelId) {
    auto handle = static_cast<TPCANHandle>(channelId);
    pUninitialize_(handle);
    fdByChannel_.erase(handle);
}

bool PeakBackend::readFrame(uint64_t channelId, CanFrame* out, std::string* error) {
    auto handle = static_cast<TPCANHandle>(channelId);
    bool fd = fdByChannel_.count(handle) ? fdByChannel_[handle] : false;
    return fd ? readFd(handle, out, error) : readClassic(handle, out, error);
}

bool PeakBackend::writeFrame(uint64_t channelId, const CanFrame& frame, std::string* error) {
    auto handle = static_cast<TPCANHandle>(channelId);
    bool fd = fdByChannel_.count(handle) ? fdByChannel_[handle] : false;
    return fd ? writeFd(handle, frame, error) : writeClassic(handle, frame, error);
}

bool PeakBackend::initializeClassic(TPCANHandle channel, TPCANBaudrate baud, std::string* error) const {
    TPCANStatus status = pInitialize_(channel, baud, /*HWTYPE*/0, /*IOPort*/0, /*Interrupt*/0);
    if (status != PCAN_ERROR_OK) {
        if (error) *error = describeStatus(status);
        return false;
    }
    return true;
}

bool PeakBackend::initializeFd(TPCANHandle channel, const std::string& bitrateString, std::string* error) const {
    std::string mutableCopy = bitrateString;
    TPCANStatus status = pInitializeFD_(channel, mutableCopy.data());
    if (status != PCAN_ERROR_OK) {
        if (error) *error = describeStatus(status);
        return false;
    }
    return true;
}

bool PeakBackend::readClassic(TPCANHandle channel, CanFrame* out, std::string* error) const {
    TPCANMsg msg{};
    TPCANTimestamp ts{};
    TPCANStatus status = pRead_(channel, &msg, &ts);
    if (status == PCAN_ERROR_QRCVEMPTY) {
        return false; // no frame available right now, not an error
    }
    if (status != PCAN_ERROR_OK) {
        if (error) *error = describeStatus(status);
        return false;
    }
    out->id = msg.ID;
    out->extended = msg.MSGTYPE & PCAN_MESSAGE_EXTENDED;
    out->rtr = msg.MSGTYPE & PCAN_MESSAGE_RTR;
    out->fd = false;
    out->brs = false;
    out->esi = false;
    out->dlc = msg.LEN;
    std::memcpy(out->data, msg.DATA, msg.LEN);
    out->timestampUs = static_cast<uint64_t>(ts.millis) * 1000ull + ts.micros
        + (static_cast<uint64_t>(ts.millis_overflow) << 32) * 1000ull;
    return true;
}

bool PeakBackend::readFd(TPCANHandle channel, CanFrame* out, std::string* error) const {
    TPCANMsgFD msg{};
    TPCANTimestamp ts{};
    TPCANStatus status = pReadFD_(channel, &msg, &ts);
    if (status == PCAN_ERROR_QRCVEMPTY) {
        return false;
    }
    if (status != PCAN_ERROR_OK) {
        if (error) *error = describeStatus(status);
        return false;
    }
    out->id = msg.ID;
    out->extended = msg.MSGTYPE & PCAN_MESSAGE_EXTENDED;
    out->rtr = msg.MSGTYPE & PCAN_MESSAGE_RTR;
    out->fd = msg.MSGTYPE & PCAN_MESSAGE_FD;
    out->brs = msg.MSGTYPE & PCAN_MESSAGE_BRS;
    out->esi = msg.MSGTYPE & PCAN_MESSAGE_ESI;
    out->dlc = msg.DLC;
    std::memcpy(out->data, msg.DATA, sizeof(out->data));
    out->timestampUs = static_cast<uint64_t>(ts.millis) * 1000ull + ts.micros
        + (static_cast<uint64_t>(ts.millis_overflow) << 32) * 1000ull;
    return true;
}

bool PeakBackend::writeClassic(TPCANHandle channel, const CanFrame& frame, std::string* error) const {
    TPCANMsg msg{};
    msg.ID = frame.id;
    msg.MSGTYPE = (frame.extended ? PCAN_MESSAGE_EXTENDED : PCAN_MESSAGE_STANDARD)
        | (frame.rtr ? PCAN_MESSAGE_RTR : 0);
    msg.LEN = frame.dlc;
    std::memcpy(msg.DATA, frame.data, frame.dlc);
    TPCANStatus status = pWrite_(channel, &msg);
    if (status != PCAN_ERROR_OK) {
        if (error) *error = describeStatus(status);
        return false;
    }
    return true;
}

bool PeakBackend::writeFd(TPCANHandle channel, const CanFrame& frame, std::string* error) const {
    TPCANMsgFD msg{};
    msg.ID = frame.id;
    msg.MSGTYPE = (frame.extended ? PCAN_MESSAGE_EXTENDED : PCAN_MESSAGE_STANDARD)
        | (frame.fd ? PCAN_MESSAGE_FD : 0)
        | (frame.brs ? PCAN_MESSAGE_BRS : 0);
    msg.DLC = frame.dlc;
    std::memcpy(msg.DATA, frame.data, sizeof(msg.DATA));
    TPCANStatus status = pWriteFD_(channel, &msg);
    if (status != PCAN_ERROR_OK) {
        if (error) *error = describeStatus(status);
        return false;
    }
    return true;
}

} // namespace cantrip
