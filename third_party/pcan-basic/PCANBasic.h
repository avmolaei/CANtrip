// PCANBasic.h - minimal vendored subset of PEAK-System's PCAN-Basic API
//
// This is NOT the full official header. It declares only the types,
// constants, and functions CANtrip actually calls (initialize, read,
// write, uninitialize, channel enumeration, CAN-FD variants).
//
// IMPORTANT: replace this file with the official PCANBasic.h shipped in
// the PCAN-Basic SDK (installed alongside the PCAN-Basic driver package
// from https://www.peak-system.com/PCAN-Basic.239.0.html) before relying
// on this for real hardware bring-up. Constant values below (baud rates,
// parameter IDs, channel handles) are transcribed from PEAK's public API
// documentation and are believed correct for PCAN-Basic 4.x, but the
// vendor header is the source of truth.
#pragma once

#include <windows.h>

extern "C" {

typedef WORD TPCANHandle;
typedef DWORD TPCANStatus;
typedef BYTE TPCANDevice;
typedef WORD TPCANParameter;
typedef BYTE TPCANMessageType;
typedef BYTE TPCANType;
typedef WORD TPCANMode;
typedef DWORD TPCANBaudrate;

// --- Channel handles ---
#define PCAN_NONEBUS      0x00U
#define PCAN_USBBUS1      0x51U
#define PCAN_USBBUS2      0x52U
#define PCAN_USBBUS3      0x53U
#define PCAN_USBBUS4      0x54U
#define PCAN_USBBUS5      0x55U
#define PCAN_USBBUS6      0x56U
#define PCAN_USBBUS7      0x57U
#define PCAN_USBBUS8      0x58U
#define PCAN_USBBUS9      0x509U
#define PCAN_USBBUS10     0x50AU
#define PCAN_USBBUS11     0x50BU
#define PCAN_USBBUS12     0x50CU
#define PCAN_USBBUS13     0x50DU
#define PCAN_USBBUS14     0x50EU
#define PCAN_USBBUS15     0x50FU
#define PCAN_USBBUS16     0x510U
#define PCAN_LANBUS1      0x801U
#define PCAN_LANBUS2      0x802U

// --- Classic CAN baud rates (BTR0/BTR1 encoded) ---
#define PCAN_BAUD_1M      0x0014U
#define PCAN_BAUD_800K    0x0016U
#define PCAN_BAUD_500K    0x001CU
#define PCAN_BAUD_250K    0x011CU
#define PCAN_BAUD_125K    0x031CU
#define PCAN_BAUD_100K    0x432FU
#define PCAN_BAUD_95K     0x532FU
#define PCAN_BAUD_83K     0x852BU
#define PCAN_BAUD_50K     0x472FU
#define PCAN_BAUD_47K     0x1414U
#define PCAN_BAUD_33K     0x8B2FU
#define PCAN_BAUD_20K     0x532FU
#define PCAN_BAUD_10K     0x672FU
#define PCAN_BAUD_5K      0x7F7FU

// --- Message types (TPCANMsg.MSGTYPE bit flags) ---
#define PCAN_MESSAGE_STANDARD  0x00U
#define PCAN_MESSAGE_RTR       0x01U
#define PCAN_MESSAGE_EXTENDED  0x02U
#define PCAN_MESSAGE_FD        0x04U
#define PCAN_MESSAGE_BRS       0x08U
#define PCAN_MESSAGE_ESI       0x10U
#define PCAN_MESSAGE_ECHO      0x20U
#define PCAN_MESSAGE_ERRFRAME  0x40U
#define PCAN_MESSAGE_STATUS    0x80U

// --- Parameters for CAN_GetValue / CAN_SetValue ---
#define PCAN_CHANNEL_CONDITION      0x07U
#define PCAN_CHANNEL_IDENTIFYING    0x08U
#define PCAN_CHANNEL_FEATURES       0x09U
#define PCAN_BITRATE_ADAPTING       0x0AU
#define PCAN_BITRATE_INFO           0x0BU
#define PCAN_BITRATE_INFO_FD        0x0CU
#define PCAN_ATTACHED_CHANNELS_COUNT 0x2FU
#define PCAN_ATTACHED_CHANNELS       0x30U

// PCAN_CHANNEL_CONDITION values
#define PCAN_CHANNEL_UNAVAILABLE  0x00U
#define PCAN_CHANNEL_AVAILABLE    0x01U
#define PCAN_CHANNEL_OCCUPIED     0x02U
#define PCAN_CHANNEL_PCANVIEW     0x03U

// --- Status/error codes (TPCANStatus bit flags) ---
#define PCAN_ERROR_OK          0x00000U
#define PCAN_ERROR_XMTFULL     0x00001U
#define PCAN_ERROR_OVERRUN     0x00002U
#define PCAN_ERROR_BUSLIGHT    0x00004U
#define PCAN_ERROR_BUSHEAVY    0x00008U
#define PCAN_ERROR_BUSWARNING  PCAN_ERROR_BUSHEAVY
#define PCAN_ERROR_BUSOFF      0x00010U
#define PCAN_ERROR_QRCVEMPTY   0x00020U
#define PCAN_ERROR_QOVERRUN    0x00040U
#define PCAN_ERROR_QXMTFULL    0x00080U
#define PCAN_ERROR_REGTEST     0x00100U
#define PCAN_ERROR_NODRIVER    0x00200U
#define PCAN_ERROR_HWINUSE     0x00400U
#define PCAN_ERROR_NETINUSE    0x00800U
#define PCAN_ERROR_ILLHW       0x01400U
#define PCAN_ERROR_ILLNET      0x01800U
#define PCAN_ERROR_ILLCLIENT   0x01C00U
#define PCAN_ERROR_ILLHANDLE   (PCAN_ERROR_ILLHW | PCAN_ERROR_ILLNET | PCAN_ERROR_ILLCLIENT)
#define PCAN_ERROR_RESOURCE    0x02000U
#define PCAN_ERROR_ILLPARAMTYPE 0x04000U
#define PCAN_ERROR_ILLPARAMVAL 0x08000U
#define PCAN_ERROR_UNKNOWN     0x10000U
#define PCAN_ERROR_ILLDATA     0x20000U
#define PCAN_ERROR_BUSPASSIVE  0x40000U
#define PCAN_ERROR_ILLMODE     0x80000U

#define MAX_LENGTH_HARDWARE_NAME 33
#define MAX_LENGTH_VERSION_STRING 256

#pragma pack(push, 1)

typedef struct {
    DWORD ID;
    TPCANMessageType MSGTYPE;
    BYTE LEN;
    BYTE DATA[8];
} TPCANMsg;

typedef struct {
    DWORD ID;
    TPCANMessageType MSGTYPE;
    BYTE DLC;
    BYTE DATA[64];
} TPCANMsgFD;

typedef struct {
    DWORD millis;
    WORD millis_overflow;
    WORD micros;
} TPCANTimestamp;

typedef struct {
    TPCANHandle channel_handle;
    TPCANDevice device_type;
    BYTE controller_number;
    DWORD device_features;
    char device_name[MAX_LENGTH_HARDWARE_NAME];
    DWORD device_id;
    DWORD channel_condition;
} TPCANChannelInformation;

#pragma pack(pop)

typedef TPCANStatus (__stdcall *CAN_Initialize_t)(TPCANHandle, TPCANBaudrate, TPCANType, DWORD, WORD);
typedef TPCANStatus (__stdcall *CAN_InitializeFD_t)(TPCANHandle, LPSTR);
typedef TPCANStatus (__stdcall *CAN_Uninitialize_t)(TPCANHandle);
typedef TPCANStatus (__stdcall *CAN_Read_t)(TPCANHandle, TPCANMsg*, TPCANTimestamp*);
typedef TPCANStatus (__stdcall *CAN_ReadFD_t)(TPCANHandle, TPCANMsgFD*, TPCANTimestamp*);
typedef TPCANStatus (__stdcall *CAN_Write_t)(TPCANHandle, TPCANMsg*);
typedef TPCANStatus (__stdcall *CAN_WriteFD_t)(TPCANHandle, TPCANMsgFD*);
typedef TPCANStatus (__stdcall *CAN_GetValue_t)(TPCANHandle, TPCANParameter, void*, DWORD);
typedef TPCANStatus (__stdcall *CAN_SetValue_t)(TPCANHandle, TPCANParameter, void*, DWORD);
typedef TPCANStatus (__stdcall *CAN_GetErrorText_t)(TPCANStatus, WORD, LPSTR);

} // extern "C"
