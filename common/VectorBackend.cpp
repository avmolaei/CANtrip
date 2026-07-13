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
    ok &= bindProc(mod, "xlCanFdSetConfiguration", &backend->pCanFdSetConfiguration_);
    ok &= bindProc(mod, "xlCanReceive", &backend->pCanReceive_);
    ok &= bindProc(mod, "xlCanTransmitEx", &backend->pCanTransmitEx_);

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

bool VectorBackend::initialize(uint64_t channelId, const CanBitrateConfig& config,
                                bool requestOwnership, std::string* error) {
    auto accessMask = static_cast<XLaccess>(channelId);
    char appName[XL_MAX_APPNAME] = "CANtrip";
    XLportHandle portHandle = XL_INVALID_PORTHANDLE;
    // permissionMask is both an input (what we're asking for) and an output
    // (what the driver actually granted - reduced or zeroed if another port
    // already holds init/configuration rights on this channel). Requesting
    // 0 instead of the full accessMask is how a port asks to join a channel
    // in listen-only mode without contesting whoever already configured it -
    // this is the exact mechanism real CANalyzer's own "Init Access" toggle
    // uses. Root cause of the original channel-sharing failure: this used to
    // unconditionally request the full mask and then unconditionally try to
    // configure the bus below, which failed hard the moment another app (or
    // another CANtrip-owned port, see MessageSender) already owned it.
    XLaccess permissionMask = requestOwnership ? accessMask : 0;

    // FD-capable ports must be opened negotiating interface version V4, not
    // the V3 used for classic - confirmed against python-can's real Vector
    // backend (verified source, see VectorBackend.h). Without this,
    // xlCanReceive fails on every call with XL_ERR_INVALID_ACCESS even
    // though xlOpenPort/xlCanFdSetConfiguration/xlActivateChannel all
    // report success - a genuinely undocumented-in-the-header requirement
    // this project hit for real, not a hypothetical.
    const unsigned int interfaceVersion = config.fd ? XL_INTERFACE_VERSION_V4 : XL_INTERFACE_VERSION;
    XLstatus status = pOpenPort_(&portHandle, appName, accessMask, &permissionMask,
                                  8192, interfaceVersion, XL_BUS_TYPE_CAN);
    if (status != XL_SUCCESS) {
        if (error) *error = describeStatus(status);
        return false;
    }

    if (requestOwnership) {
        if (config.fd) {
            // XLcanFdConf takes bit-timing tick values (sjw/tseg1/tseg2), not a
            // prescaler - unlike PCAN-Basic's init string, Vector's struct has
            // no BRP field, so config.nominalTiming.brp/dataTiming.brp are
            // unused here; the driver derives its own prescaler from bitrate +
            // (1+tseg1+tseg2). Timing is computed by CanBitTiming (see
            // AVlabsCanBackend.h) rather than the fixed defaults this backend
            // used before real bit-timing support existed.
            XLcanFdConf fdConf{};
            fdConf.arbitrationBitRate = config.nominalBitrateBps;
            fdConf.sjwAbr = config.nominalTiming.sjw;
            fdConf.tseg1Abr = config.nominalTiming.tseg1;
            fdConf.tseg2Abr = config.nominalTiming.tseg2;
            fdConf.dataBitRate = config.dataBitrateBps;
            fdConf.sjwDbr = config.dataTiming.sjw;
            fdConf.tseg1Dbr = config.dataTiming.tseg1;
            fdConf.tseg2Dbr = config.dataTiming.tseg2;

            status = pCanFdSetConfiguration_(portHandle, accessMask, &fdConf);
        } else {
            status = pCanSetChannelBitrate_(portHandle, accessMask, config.nominalBitrateBps);
        }
        if (status != XL_SUCCESS) {
            if (error) *error = describeStatus(status);
            pClosePort_(portHandle);
            return false;
        }
    }
    // else: listen-only - skip bitrate/FD configuration entirely, on the
    // assumption another port has already configured this channel. fdByChannel_
    // is still populated below from the caller's own config.fd so writeFrame()'s
    // classic/FD dispatch works correctly even though no config call was made.

    status = pActivateChannel_(portHandle, accessMask, XL_BUS_TYPE_CAN, XL_ACTIVATE_NONE);
    if (status != XL_SUCCESS) {
        if (error) *error = describeStatus(status);
        pClosePort_(portHandle);
        return false;
    }

    portByChannel_[accessMask] = portHandle;
    fdByChannel_[accessMask] = config.fd;
    return true;
}

void VectorBackend::uninitialize(uint64_t channelId) {
    auto accessMask = static_cast<XLaccess>(channelId);
    auto it = portByChannel_.find(accessMask);
    if (it == portByChannel_.end()) return;
    pDeactivateChannel_(it->second, accessMask);
    pClosePort_(it->second);
    portByChannel_.erase(it);
    fdByChannel_.erase(accessMask);
}

bool VectorBackend::readFrame(uint64_t channelId, CanFrame* out, std::string* error) {
    auto accessMask = static_cast<XLaccess>(channelId);
    auto it = portByChannel_.find(accessMask);
    if (it == portByChannel_.end()) {
        if (error) *error = "channel not initialized";
        return false;
    }
    bool fd = fdByChannel_.count(accessMask) ? fdByChannel_[accessMask] : false;
    return fd ? readFd(it->second, out, error) : readClassic(it->second, out, error);
}

bool VectorBackend::writeFrame(uint64_t channelId, const CanFrame& frame, std::string* error) {
    auto accessMask = static_cast<XLaccess>(channelId);
    auto it = portByChannel_.find(accessMask);
    if (it == portByChannel_.end()) {
        if (error) *error = "channel not initialized";
        return false;
    }
    bool fd = fdByChannel_.count(accessMask) ? fdByChannel_[accessMask] : false;
    return fd ? writeFd(it->second, accessMask, frame, error) : writeClassic(it->second, accessMask, frame, error);
}

bool VectorBackend::readClassic(XLportHandle port, CanFrame* out, std::string* error) const {
    XLevent event{};
    unsigned int eventCount = 1;
    XLstatus status = pReceive_(port, &eventCount, &event);
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
    // Classic error frames arrive as a normal XL_RECEIVE_MSG event with this
    // flag set (confirmed in the real vxlapi.h - XL_CAN_MSG_FLAG_ERROR_FRAME),
    // not as a separate event type. No further detail is available at this
    // level (unlike the FD path below), so this reports an unspecified
    // protocol violation - still enough for auto-detect to tell "errors are
    // happening" from "clean bus".
    if (msg.flags & XL_CAN_MSG_FLAG_ERROR_FRAME) {
        out->error = true;
        out->id = CanErr::kProt;
        std::memset(out->data, 0, sizeof(out->data));
        out->timestampUs = event.timeStamp / 1000ull;
        return true;
    }
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

bool VectorBackend::readFd(XLportHandle port, CanFrame* out, std::string* error) const {
    XLcanRxEvent event{};
    XLstatus status = pCanReceive_(port, &event);
    if (status == XL_ERR_QUEUE_IS_EMPTY) {
        return false; // no frame available right now, not an error
    }
    if (status != XL_SUCCESS) {
        if (error) *error = describeStatus(status);
        return false;
    }
    // XL_CAN_EV_TAG_RX_ERROR carries real per-error detail (XL_CAN_EV_ERROR::
    // errorCode, confirmed in the real vxlapi.h) - map it to the closest
    // SocketCAN CAN_ERR_* bit rather than dropping it silently, the same
    // way this exact event was used to diagnose a real FD sample-point
    // mismatch on a live Vector VN7640 earlier this project. ACK/NACK are
    // their own top-level error class in the SocketCAN convention, not a
    // CAN_ERR_PROT sub-type; anything else without a specific mapped bit
    // still reports as an unspecified protocol violation rather than being
    // dropped, since "some kind of error happened" is still useful signal
    // (e.g. for auto-detect telling a clean bus from a mismatched one).
    if (event.tag == XL_CAN_EV_TAG_RX_ERROR) {
        const XL_CAN_EV_ERROR& err = event.tagData.canError;
        out->error = true;
        std::memset(out->data, 0, sizeof(out->data));
        switch (err.errorCode) {
            case XL_CAN_ERRC_ACK_ERROR:
            case XL_CAN_ERRC_NACK_ERROR:
                out->id = CanErr::kAck;
                break;
            case XL_CAN_ERRC_BIT_ERROR:
                out->id = CanErr::kProt;
                out->data[2] = CanErr::kProtBit;
                break;
            case XL_CAN_ERRC_FORM_ERROR:
                out->id = CanErr::kProt;
                out->data[2] = CanErr::kProtForm;
                break;
            case XL_CAN_ERRC_STUFF_ERROR:
                out->id = CanErr::kProt;
                out->data[2] = CanErr::kProtStuff;
                break;
            case XL_CAN_ERRC_OVLD_ERROR:
                out->id = CanErr::kProt;
                out->data[2] = CanErr::kProtOverload;
                break;
            default:
                out->id = CanErr::kProt;
                break;
        }
        out->timestampUs = event.timeStampSync / 1000ull;
        return true;
    }
    if (event.tag != XL_CAN_EV_TAG_RX_OK) {
        return false; // chip-state/other non-data event; nothing to decode
    }

    const XL_CAN_EV_RX_MSG& msg = event.tagData.canRxOkMsg;
    out->extended = (msg.canId & XL_CAN_EXT_MSG_ID) != 0;
    out->id = msg.canId & ~XL_CAN_EXT_MSG_ID;
    out->rtr = (msg.msgFlags & XL_CAN_RXMSG_FLAG_RTR) != 0;
    out->fd = (msg.msgFlags & XL_CAN_RXMSG_FLAG_EDL) != 0;
    out->brs = (msg.msgFlags & XL_CAN_RXMSG_FLAG_BRS) != 0;
    out->esi = (msg.msgFlags & XL_CAN_RXMSG_FLAG_ESI) != 0;
    out->dlc = msg.dlc;
    std::memcpy(out->data, msg.data, sizeof(msg.data));
    // timeStampSync is nanoseconds; CanFrame wants microseconds.
    out->timestampUs = event.timeStampSync / 1000ull;
    return true;
}

bool VectorBackend::writeClassic(XLportHandle port, XLaccess accessMask, const CanFrame& frame, std::string* error) const {
    XLevent event{};
    event.tag = XL_TRANSMIT_MSG;
    event.tagData.msg.id = frame.extended ? (frame.id | XL_CAN_EXT_MSG_ID) : frame.id;
    event.tagData.msg.flags = frame.rtr ? XL_CAN_MSG_FLAG_REMOTE_FRAME : 0;
    event.tagData.msg.dlc = frame.dlc;
    std::memcpy(event.tagData.msg.data, frame.data, sizeof(event.tagData.msg.data));

    unsigned int eventCount = 1;
    XLstatus status = pCanTransmit_(port, accessMask, &eventCount, &event);
    if (status != XL_SUCCESS) {
        if (error) *error = describeStatus(status);
        return false;
    }
    return true;
}

bool VectorBackend::writeFd(XLportHandle port, XLaccess accessMask, const CanFrame& frame, std::string* error) const {
    // This function handles every transmit on an FD-capable port (see
    // writeFrame()'s dispatch, keyed on the channel's own FD mode, not any
    // per-frame flag - a V4-opened port must use xlCanTransmitEx for all
    // its I/O). But an FD-capable bus can still carry plain classic frames
    // mixed in (see TransmitMessageDialog's "FD Frame" checkbox), which
    // this port-level function has to express by leaving XL_CAN_TXMSG_FLAG_EDL
    // unset for those - setting it unconditionally (as this did before)
    // marked every single frame as FD-formatted regardless of what the
    // caller actually asked for. Found via code review (2026-07-13) while
    // investigating a real bus-error flood reported on the user's Vector
    // hardware - plausible contributor, not yet confirmed as the specific
    // fix for that report (need the user's actual test config to be sure).
    XLcanTxEvent event{};
    event.tag = XL_CAN_EV_TAG_TX_MSG;
    event.tagData.canMsg.canId = frame.extended ? (frame.id | XL_CAN_EXT_MSG_ID) : frame.id;
    event.tagData.canMsg.msgFlags = (frame.fd ? XL_CAN_TXMSG_FLAG_EDL : 0)
        | (frame.brs ? XL_CAN_TXMSG_FLAG_BRS : 0)
        | (frame.rtr ? XL_CAN_TXMSG_FLAG_RTR : 0);
    event.tagData.canMsg.dlc = frame.dlc;
    std::memcpy(event.tagData.canMsg.data, frame.data, sizeof(event.tagData.canMsg.data));

    unsigned int sentCount = 0;
    XLstatus status = pCanTransmitEx_(port, accessMask, 1, &sentCount, &event);
    if (status != XL_SUCCESS) {
        if (error) *error = describeStatus(status);
        return false;
    }
    return true;
}

} // namespace cantrip
