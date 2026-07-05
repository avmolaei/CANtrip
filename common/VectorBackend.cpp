#include "VectorBackend.h"

#include <cstring>
#include <memory>

namespace cantrip {

namespace {

template <typename FnPtr>
bool bindProc(HMODULE mod, const char* name, FnPtr* out) {
    *out = reinterpret_cast<FnPtr>(GetProcAddress(mod, name));
    return *out != nullptr;
}

// vxlapi64.dll isn't always on the standard DLL search path even when
// Vector software is installed - confirmed on a real dev machine with
// Vector CANalyzer + Vector Platform Manager installed, where the DLL only
// exists under Platform Manager's own private driver folder. The dedicated
// "Vector XL Driver Library" redistributable does register it system-wide,
// so the bare-name load is tried first and should succeed on most setups;
// this is a fallback for the specific case observed here, not a guarantee
// it covers every possible Vector install layout.
HMODULE loadVxlApi() {
    if (HMODULE mod = LoadLibraryA("vxlapi64.dll")) return mod;
    return LoadLibraryA(
        "C:\\Program Files (x86)\\Vector Platform Manager\\VtpDrivers\\Common\\vxlapi64.dll");
}

} // namespace

VectorBackend::VectorBackend(HMODULE module) : module_(module) {}

VectorBackend::~VectorBackend() {
    for (auto& [channel, port] : portByChannel_) {
        pDeactivateChannel_(port, channel);
        pClosePort_(port);
    }
    if (pCloseDriver_) pCloseDriver_();
}

std::unique_ptr<VectorBackend> VectorBackend::load(std::string* error) {
    HMODULE mod = loadVxlApi();
    if (!mod) {
        if (error) *error = "vxlapi64.dll not found. Install Vector's XL Driver "
                             "Library or a Vector driver package.";
        return nullptr;
    }

    auto backend = std::unique_ptr<VectorBackend>(new VectorBackend(mod));
    bool ok = true;
    ok &= bindProc(mod, "xlOpenDriver", &backend->pOpenDriver_);
    ok &= bindProc(mod, "xlCloseDriver", &backend->pCloseDriver_);
    ok &= bindProc(mod, "xlGetDriverConfig", &backend->pGetDriverConfig_);
    ok &= bindProc(mod, "xlOpenPort", &backend->pOpenPort_);
    ok &= bindProc(mod, "xlClosePort", &backend->pClosePort_);
    ok &= bindProc(mod, "xlActivateChannel", &backend->pActivateChannel_);
    ok &= bindProc(mod, "xlDeactivateChannel", &backend->pDeactivateChannel_);
    ok &= bindProc(mod, "xlCanSetChannelBitrate", &backend->pCanSetChannelBitrate_);
    ok &= bindProc(mod, "xlReceive", &backend->pReceive_);
    ok &= bindProc(mod, "xlCanTransmit", &backend->pCanTransmit_);
    ok &= bindProc(mod, "xlGetErrorString", &backend->pGetErrorString_);

    if (!ok) {
        if (error) *error = "vxlapi64.dll found but missing expected exports; "
                             "wrong DLL version?";
        FreeLibrary(mod);
        return nullptr;
    }

    XLstatus status = backend->pOpenDriver_();
    if (status != XL_SUCCESS) {
        if (error) *error = backend->describeStatus(status);
        FreeLibrary(mod);
        return nullptr;
    }

    return backend;
}

std::string VectorBackend::describeStatus(XLstatus status) const {
    if (pGetErrorString_) {
        if (XLstringType text = pGetErrorString_(status)) {
            return std::string(text);
        }
    }
    return "Vector XL error " + std::to_string(status);
}

std::vector<CanChannelInfo> VectorBackend::enumerateChannels() const {
    std::vector<CanChannelInfo> result;

    auto driverConfig = std::make_unique<XLdriverConfig>();
    XLstatus status = pGetDriverConfig_(driverConfig.get());
    if (status != XL_SUCCESS) return result;

    for (unsigned int i = 0; i < driverConfig->channelCount; ++i) {
        const XLchannelConfig& ch = driverConfig->channel[i];
        // channelBusCapabilities lists which bus types this channel could
        // be activated as; only offer channels that can run classic CAN.
        if (!(ch.channelBusCapabilities & XL_BUS_ACTIVE_CAP_CAN)) continue;
        std::string name(ch.name, strnlen(ch.name, sizeof(ch.name)));
        result.push_back(CanChannelInfo{ch.channelMask, name, true});
    }
    return result;
}

bool VectorBackend::initialize(uint64_t channelId, const CanBitrateConfig& config, std::string* error) {
    if (config.fd) {
        if (error) *error = "CAN FD is not yet supported by the Vector backend (classic CAN only).";
        return false;
    }

    auto accessMask = static_cast<XLaccess>(channelId);
    char appName[XL_MAX_APPNAME] = "CANtrip";
    XLportHandle portHandle = XL_INVALID_PORTHANDLE;
    XLaccess permissionMask = accessMask;

    XLstatus status = pOpenPort_(&portHandle, appName, accessMask, &permissionMask,
                                  8192, XL_INTERFACE_VERSION, XL_BUS_TYPE_CAN);
    if (status != XL_SUCCESS) {
        if (error) *error = describeStatus(status);
        return false;
    }

    status = pCanSetChannelBitrate_(portHandle, accessMask, config.nominalBitrateBps);
    if (status != XL_SUCCESS) {
        if (error) *error = describeStatus(status);
        pClosePort_(portHandle);
        return false;
    }

    status = pActivateChannel_(portHandle, accessMask, XL_BUS_TYPE_CAN, XL_ACTIVATE_NONE);
    if (status != XL_SUCCESS) {
        if (error) *error = describeStatus(status);
        pClosePort_(portHandle);
        return false;
    }

    portByChannel_[accessMask] = portHandle;
    return true;
}

void VectorBackend::uninitialize(uint64_t channelId) {
    auto accessMask = static_cast<XLaccess>(channelId);
    auto it = portByChannel_.find(accessMask);
    if (it == portByChannel_.end()) return;
    pDeactivateChannel_(it->second, accessMask);
    pClosePort_(it->second);
    portByChannel_.erase(it);
}

bool VectorBackend::readFrame(uint64_t channelId, CanFrame* out, std::string* error) {
    auto accessMask = static_cast<XLaccess>(channelId);
    auto it = portByChannel_.find(accessMask);
    if (it == portByChannel_.end()) {
        if (error) *error = "channel not initialized";
        return false;
    }

    XLevent event{};
    unsigned int eventCount = 1;
    XLstatus status = pReceive_(it->second, &eventCount, &event);
    if (status == XL_ERR_QUEUE_IS_EMPTY || eventCount == 0) {
        return false; // no frame available right now, not an error
    }
    if (status != XL_SUCCESS) {
        if (error) *error = describeStatus(status);
        return false;
    }
    if (event.tag != XL_RECEIVE_MSG) {
        return false; // chip-state/other non-data event; nothing to decode
    }

    const s_xl_can_msg& msg = event.tagData.msg;
    out->extended = (msg.id & XL_CAN_EXT_MSG_ID) != 0;
    out->id = msg.id & ~XL_CAN_EXT_MSG_ID;
    out->rtr = (msg.flags & XL_CAN_MSG_FLAG_REMOTE_FRAME) != 0;
    out->fd = false;
    out->brs = false;
    out->esi = false;
    out->dlc = static_cast<uint8_t>(msg.dlc);
    std::memcpy(out->data, msg.data, sizeof(msg.data));
    // XLevent timestamps are nanoseconds since driver start; CanFrame wants
    // microseconds.
    out->timestampUs = event.timeStamp / 1000ull;
    return true;
}

bool VectorBackend::writeFrame(uint64_t channelId, const CanFrame& frame, std::string* error) {
    auto accessMask = static_cast<XLaccess>(channelId);
    auto it = portByChannel_.find(accessMask);
    if (it == portByChannel_.end()) {
        if (error) *error = "channel not initialized";
        return false;
    }

    XLevent event{};
    event.tag = XL_TRANSMIT_MSG;
    event.tagData.msg.id = frame.extended ? (frame.id | XL_CAN_EXT_MSG_ID) : frame.id;
    event.tagData.msg.flags = frame.rtr ? XL_CAN_MSG_FLAG_REMOTE_FRAME : 0;
    event.tagData.msg.dlc = frame.dlc;
    std::memcpy(event.tagData.msg.data, frame.data, sizeof(event.tagData.msg.data));

    unsigned int eventCount = 1;
    XLstatus status = pCanTransmit_(it->second, accessMask, &eventCount, &event);
    if (status != XL_SUCCESS) {
        if (error) *error = describeStatus(status);
        return false;
    }
    return true;
}

} // namespace cantrip
