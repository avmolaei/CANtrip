// ICanBackend implementation for PEAK-System PCAN-Basic hardware. This is
// the reference backend - see CanBackend.h for the vendor-neutral interface
// that a Vector, Kvaser, or ETAS backend would implement the same way.
#pragma once

#include <unordered_map>

#include "AVlabsCanBackend.h"
#include "../third_party/pcan-basic/PCANBasic.h"

namespace cantrip {

class PeakBackend : public ICanBackend {
public:
    // Loads PCANBasic.dll from the system search path. Returns nullptr
    // (with `error` set) if the DLL isn't found - meaning the PCAN-Basic
    // driver package isn't installed - so probeAvailableBackends() can
    // silently skip this backend rather than fail startup.
    static std::unique_ptr<PeakBackend> load(std::string* error);

    std::string id() const override { return "peak"; }
    std::string displayName() const override { return "PEAK-System PCAN-Basic"; }

    std::vector<CanChannelInfo> enumerateChannels() const override;

    bool initialize(uint64_t channelId, const CanBitrateConfig& config, std::string* error) override;
    void uninitialize(uint64_t channelId) override;

    bool readFrame(uint64_t channelId, CanFrame* out, std::string* error) override;
    bool writeFrame(uint64_t channelId, const CanFrame& frame, std::string* error) override;

private:
    explicit PeakBackend(HMODULE module);

    bool initializeClassic(TPCANHandle channel, TPCANBaudrate baud, std::string* error) const;
    bool initializeFd(TPCANHandle channel, const std::string& bitrateString, std::string* error) const;
    bool readClassic(TPCANHandle channel, CanFrame* out, std::string* error) const;
    bool readFd(TPCANHandle channel, CanFrame* out, std::string* error) const;
    bool writeClassic(TPCANHandle channel, const CanFrame& frame, std::string* error) const;
    bool writeFd(TPCANHandle channel, const CanFrame& frame, std::string* error) const;
    std::string describeStatus(TPCANStatus status) const;

    HMODULE module_ = nullptr;
    CAN_Initialize_t pInitialize_ = nullptr;
    CAN_InitializeFD_t pInitializeFD_ = nullptr;
    CAN_Uninitialize_t pUninitialize_ = nullptr;
    CAN_Read_t pRead_ = nullptr;
    CAN_ReadFD_t pReadFD_ = nullptr;
    CAN_Write_t pWrite_ = nullptr;
    CAN_WriteFD_t pWriteFD_ = nullptr;
    CAN_GetValue_t pGetValue_ = nullptr;
    CAN_SetValue_t pSetValue_ = nullptr;
    CAN_GetErrorText_t pGetErrorText_ = nullptr;

    // Tracks whether each initialized channel was opened in FD mode, since
    // PCAN-Basic requires calling CAN_Read/CAN_Write vs CAN_ReadFD/CAN_WriteFD
    // accordingly - the vendor-neutral readFrame/writeFrame calls don't carry
    // that distinction themselves, so it's cached from initialize() by handle.
    std::unordered_map<TPCANHandle, bool> fdByChannel_;
};

} // namespace cantrip
