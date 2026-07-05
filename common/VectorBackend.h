// AVlabs CAN backend implementation for Vector Informatik CAN hardware
// (VN-series interfaces), using the classic XL Driver Library API. See
// AVlabsCanBackend.h for the vendor-neutral interface and PeakBackend for
// the reference backend.
//
// CAN FD is NOT yet supported here (classic CAN only) - Vector's FD API
// (xlCanFdSetConfiguration, XLcanRxEvent) is a separate, larger surface not
// yet vendored/verified. initialize() rejects CanBitrateConfig::fd requests.
#pragma once

#include <unordered_map>

#include "AVlabsCanBackend.h"
#include "../third_party/vector-xl/vxlapi.h"

namespace cantrip {

class VectorBackend : public ICanBackend {
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

    // Each initialized channel gets its own xlOpenPort() call scoped to just
    // that channel's access mask, since CANtrip's ICanBackend interface
    // manages channels independently - Vector's own model would let one
    // port span several channels at once, but we don't need that here.
    std::unordered_map<XLaccess, XLportHandle> portByChannel_;
};

} // namespace cantrip
