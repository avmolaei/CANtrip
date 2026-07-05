// Thin dynamic-loading wrapper around PCANBasic.dll, shared by the
// pcan2pcap extcap program and the CANtrip app so PCAN-Basic is only
// wired up once. Loads the DLL at runtime (LoadLibrary) rather than
// link-time so building CANtrip doesn't require the PEAK-System SDK's
// import library to be present.
#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

#include "../third_party/pcan-basic/PCANBasic.h"

namespace cantrip {

struct PcanChannel {
    TPCANHandle handle;
    std::string name;      // e.g. "PCAN_USBBUS1"
    bool available;
};

// Frame as read from PCAN-Basic, normalized for classic or FD.
struct PcanFrame {
    uint32_t id = 0;
    bool extended = false;
    bool rtr = false;
    bool fd = false;
    bool brs = false;      // bit rate switch (FD only)
    bool esi = false;      // error state indicator (FD only)
    uint8_t dlc = 0;       // data length code (0-8 classic, 0-15 FD)
    uint8_t data[64] = {};
    uint64_t timestampUs = 0; // microseconds since CAN_Initialize
};

// Loads PCANBasic.dll from the system search path. Returns false (with
// `error` set) if the DLL isn't found — meaning the PCAN-Basic driver
// package isn't installed.
class PcanApi {
public:
    static std::optional<PcanApi> load(std::string* error);

    // Enumerates channels PCAN-Basic reports as physically present.
    std::vector<PcanChannel> enumerateChannels() const;

    // classicBaud: one of the PCAN_BAUD_* constants from PCANBasic.h.
    bool initializeClassic(TPCANHandle channel, TPCANBaudrate classicBaud, std::string* error) const;

    // bitrateString: PCAN-Basic FD init string, e.g.
    // "f_clock_mhz=80,nom_brp=2,nom_tseg1=63,nom_tseg2=16,nom_sjw=16,"
    // "data_brp=2,data_tseg1=15,data_tseg2=4,data_sjw=4"
    bool initializeFd(TPCANHandle channel, const std::string& bitrateString, std::string* error) const;

    void uninitialize(TPCANHandle channel) const;

    // Blocking-ish single read; returns false on PCAN_ERROR_QRCVEMPTY
    // (no frame available) as well as on real errors — caller checks
    // `error` to distinguish (empty error string = just no data yet).
    bool readClassic(TPCANHandle channel, PcanFrame* out, std::string* error) const;
    bool readFd(TPCANHandle channel, PcanFrame* out, std::string* error) const;

    bool writeClassic(TPCANHandle channel, const PcanFrame& frame, std::string* error) const;
    bool writeFd(TPCANHandle channel, const PcanFrame& frame, std::string* error) const;

private:
    explicit PcanApi(HMODULE module);

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

    std::string describeStatus(TPCANStatus status) const;
};

} // namespace cantrip
