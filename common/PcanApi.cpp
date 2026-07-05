#include "PcanApi.h"

#include <array>
#include <cstring>

namespace cantrip {

namespace {

// Channels PCAN_ATTACHED_CHANNELS can report on; we probe these handles
// directly with PCAN_CHANNEL_CONDITION as a fallback for PCAN-Basic
// builds too old to support PCAN_ATTACHED_CHANNELS.
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

} // namespace

PcanApi::PcanApi(HMODULE module) : module_(module) {}

std::optional<PcanApi> PcanApi::load(std::string* error) {
    HMODULE mod = LoadLibraryA("PCANBasic.dll");
    if (!mod) {
        if (error) *error = "PCANBasic.dll not found. Install the PEAK-System "
                             "PCAN-Basic driver package.";
        return std::nullopt;
    }

    PcanApi api(mod);
    bool ok = true;
    ok &= bindProc(mod, "CAN_Initialize", &api.pInitialize_);
    ok &= bindProc(mod, "CAN_InitializeFD", &api.pInitializeFD_);
    ok &= bindProc(mod, "CAN_Uninitialize", &api.pUninitialize_);
    ok &= bindProc(mod, "CAN_Read", &api.pRead_);
    ok &= bindProc(mod, "CAN_ReadFD", &api.pReadFD_);
    ok &= bindProc(mod, "CAN_Write", &api.pWrite_);
    ok &= bindProc(mod, "CAN_WriteFD", &api.pWriteFD_);
    ok &= bindProc(mod, "CAN_GetValue", &api.pGetValue_);
    ok &= bindProc(mod, "CAN_SetValue", &api.pSetValue_);
    ok &= bindProc(mod, "CAN_GetErrorText", &api.pGetErrorText_);

    if (!ok) {
        if (error) *error = "PCANBasic.dll found but missing expected exports; "
                             "wrong DLL version?";
        FreeLibrary(mod);
        return std::nullopt;
    }
    return api;
}

std::string PcanApi::describeStatus(TPCANStatus status) const {
    char buf[256] = {};
    if (pGetErrorText_ && pGetErrorText_(status, 0, buf) == PCAN_ERROR_OK) {
        return std::string(buf);
    }
    return "PCAN error 0x" + std::to_string(status);
}

std::vector<PcanChannel> PcanApi::enumerateChannels() const {
    std::vector<PcanChannel> result;
    for (TPCANHandle h : kProbeHandles) {
        DWORD condition = PCAN_CHANNEL_UNAVAILABLE;
        TPCANStatus status = pGetValue_(h, PCAN_CHANNEL_CONDITION, &condition, sizeof(condition));
        if (status != PCAN_ERROR_OK) continue;
        if (condition == PCAN_CHANNEL_UNAVAILABLE) continue;
        result.push_back(PcanChannel{h, handleName(h), condition == PCAN_CHANNEL_AVAILABLE});
    }
    return result;
}

bool PcanApi::initializeClassic(TPCANHandle channel, TPCANBaudrate baud, std::string* error) const {
    TPCANStatus status = pInitialize_(channel, baud, /*HWTYPE*/0, /*IOPort*/0, /*Interrupt*/0);
    if (status != PCAN_ERROR_OK) {
        if (error) *error = describeStatus(status);
        return false;
    }
    return true;
}

bool PcanApi::initializeFd(TPCANHandle channel, const std::string& bitrateString, std::string* error) const {
    std::string mutableCopy = bitrateString;
    TPCANStatus status = pInitializeFD_(channel, mutableCopy.data());
    if (status != PCAN_ERROR_OK) {
        if (error) *error = describeStatus(status);
        return false;
    }
    return true;
}

void PcanApi::uninitialize(TPCANHandle channel) const {
    pUninitialize_(channel);
}

bool PcanApi::readClassic(TPCANHandle channel, PcanFrame* out, std::string* error) const {
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

bool PcanApi::readFd(TPCANHandle channel, PcanFrame* out, std::string* error) const {
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

bool PcanApi::writeClassic(TPCANHandle channel, const PcanFrame& frame, std::string* error) const {
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

bool PcanApi::writeFd(TPCANHandle channel, const PcanFrame& frame, std::string* error) const {
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
