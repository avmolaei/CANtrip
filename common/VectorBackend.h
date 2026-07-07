// AVlabs CAN backend implementation for Vector Informatik CAN hardware
// (VN-series interfaces), using the XL Driver Library API. See
// AVlabsCanBackend.h for the vendor-neutral interface and PeakBackend for
// the reference backend.
//
// Supports both classic CAN (xlReceive/xlCanTransmit, the older event API)
// and CAN FD (xlCanReceive/xlCanTransmitEx, a separate newer event API with
// a different event struct - Vector didn't extend the classic one since its
// 32-byte payload can't hold FD's up to 64 data bytes).
#pragma once

#include <unordered_map>

#include "AVlabsCanBackend.h"
#include "../third_party/vector-xl/vxlapi.h"

namespace cantrip {

class VectorBackend : public IAvlabsCanBackend {
public:
    // Loads vxlapi64.dll and calls xlOpenDriver(). Returns nullptr (with
    // `error` set) if the DLL isn't found on the standard search path or in
    // a couple of known Vector install locations - meaning no Vector
    // software/driver is installed - so probeAvailableBackends() can
    // silently skip this backend.
    static std::unique_ptr<VectorBackend> load(std::string* error);
    ~VectorBackend() override;

    std::string id() const override { return "vector"; }
    std::string displayName() const override { return "Vector XL Driver Library"; }

    std::vector<CanChannelInfo> enumerateChannels() const override;

    bool initialize(uint64_t channelId, const CanBitrateConfig& config, std::string* error) override;
    void uninitialize(uint64_t channelId) override;

    bool readFrame(uint64_t channelId, CanFrame* out, std::string* error) override;
    bool writeFrame(uint64_t channelId, const CanFrame& frame, std::string* error) override;

private:
    explicit VectorBackend(HMODULE module);
    std::string describeStatus(XLstatus status) const;
    bool readClassic(XLportHandle port, CanFrame* out, std::string* error) const;
    bool readFd(XLportHandle port, CanFrame* out, std::string* error) const;
    bool writeClassic(XLportHandle port, XLaccess accessMask, const CanFrame& frame, std::string* error) const;
    bool writeFd(XLportHandle port, XLaccess accessMask, const CanFrame& frame, std::string* error) const;

    HMODULE module_ = nullptr;
    xlOpenDriver_t pOpenDriver_ = nullptr;
    xlCloseDriver_t pCloseDriver_ = nullptr;
    xlGetDriverConfig_t pGetDriverConfig_ = nullptr;
    xlOpenPort_t pOpenPort_ = nullptr;
    xlClosePort_t pClosePort_ = nullptr;
    xlActivateChannel_t pActivateChannel_ = nullptr;
    xlDeactivateChannel_t pDeactivateChannel_ = nullptr;
    xlCanSetChannelBitrate_t pCanSetChannelBitrate_ = nullptr;
    xlReceive_t pReceive_ = nullptr;
    xlCanTransmit_t pCanTransmit_ = nullptr;
    xlGetErrorString_t pGetErrorString_ = nullptr;
    xlCanFdSetConfiguration_t pCanFdSetConfiguration_ = nullptr;
    xlCanReceive_t pCanReceive_ = nullptr;
    xlCanTransmitEx_t pCanTransmitEx_ = nullptr;

    // Each initialized channel gets its own xlOpenPort() call scoped to just
    // that channel's access mask, since CANtrip's IAvlabsCanBackend interface
    // manages channels independently - Vector's own model would let one
    // port span several channels at once, but we don't need that here.
    std::unordered_map<XLaccess, XLportHandle> portByChannel_;
    // Tracks whether each channel was opened in FD mode, since Vector
    // requires calling xlReceive/xlCanTransmit vs xlCanReceive/
    // xlCanTransmitEx accordingly - readFrame/writeFrame don't carry that
    // distinction themselves, so it's cached from initialize() by channel.
    std::unordered_map<XLaccess, bool> fdByChannel_;
};

} // namespace cantrip
