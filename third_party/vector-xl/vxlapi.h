// vxlapi.h - minimal vendored subset of Vector Informatik's XL Driver Library
// API, adapted for CANtrip's dynamic-loading (LoadLibrary/GetProcAddress)
// pattern rather than link-time import.
//
// Given a real, hand-verified bug in an earlier vendored PEAK header (wrong
// parameter IDs AND wrong struct packing - see third_party/pcan-basic's
// history), the structs below are copied VERBATIM (field-for-field, same
// order, same types) from a verified real copy of Vector's actual vxlapi.h
// (github.com/rbei-etas/busmaster, BUSMASTER's vendored copy, copyright
// Vector Informatik GmbH) rather than retyped from documentation/memory -
// deliberately, since a wrong field offset here is a silent
// memory-corruption bug, not a compile error, exactly like the PEAK case.
// Cross-checked against a real installed vxlapi64.dll v11.6.12.0.
//
// Only the classic CAN subset is included (LIN/FlexRay/MOST/Ethernet/A429/
// DAIO fields and functions are omitted - CANtrip doesn't use them). CAN FD
// support (xlCanFdSetConfiguration, XLcanRxEvent) is not yet implemented;
// see VectorBackend.
#pragma once

#include <windows.h>

extern "C" {

// ---- Core types (verbatim from vxlapi.h) ----
typedef unsigned __int64 XLuint64;
typedef XLuint64 XLaccess;
typedef HANDLE XLhandle;
typedef short XLstatus;
typedef long XLportHandle, *pXLportHandle;
typedef char* XLstringType;

// ---- Status codes ----
#define XL_SUCCESS 0
#define XL_ERR_QUEUE_IS_EMPTY 10

// ---- Bus types / activation flags ----
#define XL_BUS_TYPE_CAN 0x00000001U
#define XL_BUS_COMPATIBLE_CAN XL_BUS_TYPE_CAN
#define XL_BUS_ACTIVE_CAP_CAN (XL_BUS_COMPATIBLE_CAN << 16)
#define XL_ACTIVATE_NONE 0
#define XL_ACTIVATE_RESET_CLOCK 8
#define XL_INTERFACE_VERSION_V3 3
#define XL_INTERFACE_VERSION_V4 4
#define XL_INTERFACE_VERSION XL_INTERFACE_VERSION_V3

// ---- Config limits ----
#define XL_MAX_APPNAME 32
#define XL_MAX_LENGTH 31
#define XL_CONFIG_MAX_CHANNELS 64
#define XL_INVALID_PORTHANDLE (-1)

// ---- Channel mask helper ----
#define XL_CHANNEL_MASK(x) (1I64 << (x))

// ---- CAN message event tags/flags ----
#ifndef MAX_MSG_LEN
#define MAX_MSG_LEN 8
#endif
#define XL_RECEIVE_MSG ((unsigned short)0x0001)
#define XL_TRANSMIT_MSG ((unsigned short)0x000A)
#define XL_CAN_EXT_MSG_ID 0x80000000U
#define XL_CAN_MSG_FLAG_ERROR_FRAME 0x01
#define XL_CAN_MSG_FLAG_OVERRUN 0x02
#define XL_CAN_MSG_FLAG_REMOTE_FRAME 0x10
#define XL_CAN_MSG_FLAG_TX_COMPLETED 0x40
#define XL_CAN_MSG_FLAG_TX_REQUEST 0x80

#include <pshpack1.h>

// ---- Verbatim from vxlapi.h: struct s_xl_can_msg (32 bytes) ----
struct s_xl_can_msg {
    unsigned long id;
    unsigned short flags;
    unsigned short dlc;
    XLuint64 res1;
    unsigned char data[MAX_MSG_LEN];
    XLuint64 res2;
};

// ---- Verbatim from vxlapi.h: struct s_xl_event (XLevent, 48 bytes) ----
// The real struct's tagData is a union of several 32-byte payload types
// (CAN msg, chip state, LIN, sync pulse, DAIO...); s_xl_can_msg is exactly
// 32 bytes (confirmed against the real header's own "/* 32 Bytes */"
// annotations on every union member), so using it directly in place of the
// union reproduces the same total size without needing to also vendor the
// LIN/DAIO/sync-pulse struct definitions CANtrip never reads.
typedef unsigned char XLeventTag;
struct s_xl_event {
    XLeventTag tag;
    unsigned char chanIndex;
    unsigned short transId;
    unsigned short portHandle;
    unsigned char flags;
    unsigned char reserved;
    XLuint64 timeStamp;
    union {
        struct s_xl_can_msg msg;
        unsigned char raw[32];
    } tagData;
};
typedef struct s_xl_event XLevent;

// ---- Verbatim from vxlapi.h: XLbusParams (32 bytes) ----
// Only the `can`/`canFD` union members are meaningful to CANtrip; the
// other bus types' sub-structs are kept so the union's total size (and
// therefore every field's offset in XL_CHANNEL_CONFIG below) matches the
// real header exactly.
typedef struct {
    unsigned int busType;
    union {
        struct {
            unsigned int bitRate;
            unsigned char sjw;
            unsigned char tseg1;
            unsigned char tseg2;
            unsigned char sam;
            unsigned char outputMode;
            unsigned char reserved[7];
            unsigned char canOpMode;
        } can;
        struct {
            unsigned int arbitrationBitRate;
            unsigned char sjwAbr;
            unsigned char tseg1Abr;
            unsigned char tseg2Abr;
            unsigned char samAbr;
            unsigned char outputMode;
            unsigned char sjwDbr;
            unsigned char tseg1Dbr;
            unsigned char tseg2Dbr;
            unsigned int dataBitRate;
            unsigned char canOpMode;
        } canFD;
        struct {
            unsigned short channelDirection;
            unsigned short res1;
            union {
                struct { unsigned int bitrate; unsigned int parity; unsigned int minGap; } tx;
                struct { unsigned int bitrate; unsigned int minBitrate; unsigned int maxBitrate; unsigned int parity; unsigned int minGap; unsigned int autoBaudrate; } rx;
                unsigned char raw[24];
            } dir;
        } a429;
        unsigned char raw[28];
    } data;
} XLbusParams;

// ---- Verbatim from vxlapi.h: XL_CHANNEL_CONFIG / XL_DRIVER_CONFIG ----
// (used only for enumeration - CANtrip reads name/channelMask/
// channelBusCapabilities and ignores the rest, but every field must stay
// in place so those offsets are correct.)
typedef struct s_xl_channel_config {
    char name[XL_MAX_LENGTH + 1];
    unsigned char hwType;
    unsigned char hwIndex;
    unsigned char hwChannel;
    unsigned short transceiverType;
    unsigned short transceiverState;
    unsigned short configError;
    unsigned char channelIndex;
    XLuint64 channelMask;
    unsigned int channelCapabilities;
    unsigned int channelBusCapabilities;
    unsigned char isOnBus;
    unsigned int connectedBusType;
    XLbusParams busParams;
    unsigned int _doNotUse;
    unsigned int driverVersion;
    unsigned int interfaceVersion;
    unsigned int raw_data[10];
    unsigned int serialNumber;
    unsigned int articleNumber;
    char transceiverName[XL_MAX_LENGTH + 1];
    unsigned int specialCabFlags;
    unsigned int dominantTimeout;
    unsigned char dominantRecessiveDelay;
    unsigned char recessiveDominantDelay;
    unsigned char connectionInfo;
    unsigned char currentlyAvailableTimestamps;
    unsigned short minimalSupplyVoltage;
    unsigned short maximalSupplyVoltage;
    unsigned int maximalBaudrate;
    unsigned char fpgaCoreCapabilities;
    unsigned char specialDeviceStatus;
    unsigned short channelBusActiveCapabilities;
    unsigned short breakOffset;
    unsigned short delimiterOffset;
    unsigned int reserved[3];
} XL_CHANNEL_CONFIG;
typedef XL_CHANNEL_CONFIG XLchannelConfig;

typedef struct s_xl_driver_config {
    unsigned int dllVersion;
    unsigned int channelCount;
    unsigned int reserved[10];
    XLchannelConfig channel[XL_CONFIG_MAX_CHANNELS];
} XL_DRIVER_CONFIG;
typedef XL_DRIVER_CONFIG XLdriverConfig;

// ---- Verbatim from vxlapi.h: XLcanFdConf (bit timing for CAN FD) ----
// All-`unsigned int` fields, so pack(1) vs natural alignment makes no
// actual difference here, but kept in the same pack(1) region as the real
// header for consistency.
typedef struct {
    unsigned int arbitrationBitRate;
    unsigned int sjwAbr;
    unsigned int tseg1Abr;
    unsigned int tseg2Abr;
    unsigned int dataBitRate;
    unsigned int sjwDbr;
    unsigned int tseg1Dbr;
    unsigned int tseg2Dbr;
    unsigned int reserved[2];
} XLcanFdConf;

#include <poppack.h>

// ---- Verbatim from vxlapi.h: CAN FD tx/rx event definitions ----
// The real header packs these at 8-byte alignment (pshpack8), NOT the
// pack(1) used for the classic XLevent/XL_CHANNEL_CONFIG structs above -
// a different packing pragma for a different part of the same file, so
// this is reproduced exactly rather than assumed to match the rest.
#include <pshpack8.h>

#define XL_CAN_MAX_DATA_LEN 64
#define XL_CANFD_RX_EVENT_HEADER_SIZE 32
#define XL_CANFD_MAX_EVENT_SIZE 128

// Real byte count from a CAN FD DLC code (0-15) - DLCs 9-15 map to
// non-linear lengths (12/16/20/24/32/48/64), not 9-15 bytes.
#define CANFD_GET_NUM_DATABYTES(dlc, edl, rtr) \
    ((rtr) ? 0 : (dlc) < 9 ? (dlc) : !(edl) ? 8 : (dlc) == 9 ? 12 : (dlc) == 10 ? 16 : (dlc) == 11 ? 20 : (dlc) == 12 ? 24 : (dlc) == 13 ? 32 : (dlc) == 14 ? 48 : 64)

#define XL_CAN_TXMSG_FLAG_EDL 0x0001
#define XL_CAN_TXMSG_FLAG_BRS 0x0002
#define XL_CAN_TXMSG_FLAG_RTR 0x0010

#define XL_CAN_RXMSG_FLAG_EDL 0x0001
#define XL_CAN_RXMSG_FLAG_BRS 0x0002
#define XL_CAN_RXMSG_FLAG_ESI 0x0004
#define XL_CAN_RXMSG_FLAG_RTR 0x0010

#define XL_CAN_EV_TAG_RX_OK ((unsigned short)0x0400)
#define XL_CAN_EV_TAG_RX_ERROR ((unsigned short)0x0401)
#define XL_CAN_EV_TAG_TX_ERROR ((unsigned short)0x0402)
#define XL_CAN_EV_TAG_TX_REQUEST ((unsigned short)0x0403)
#define XL_CAN_EV_TAG_TX_OK ((unsigned short)0x0404)
#define XL_CAN_EV_TAG_CHIP_STATE ((unsigned short)0x0409)
#define XL_CAN_EV_TAG_TX_MSG ((unsigned short)0x0440)

typedef struct {
    unsigned int canId;
    unsigned int msgFlags;
    unsigned char dlc;
    unsigned char reserved[7];
    unsigned char data[XL_CAN_MAX_DATA_LEN];
} XL_CAN_TX_MSG;

typedef struct {
    unsigned short tag;
    unsigned short transId;
    unsigned char channelIndex;
    unsigned char reserved[3];
    union {
        XL_CAN_TX_MSG canMsg;
    } tagData;
} XLcanTxEvent;

// used with XL_CAN_EV_TAG_RX_OK, XL_CAN_EV_TAG_TX_OK
typedef struct {
    unsigned int canId;
    unsigned int msgFlags;
    unsigned int crc;
    unsigned char reserved1[12];
    unsigned short totalBitCnt;
    unsigned char dlc;
    unsigned char reserved[5];
    unsigned char data[XL_CAN_MAX_DATA_LEN];
} XL_CAN_EV_RX_MSG;

typedef struct {
    unsigned int canId;
    unsigned int msgFlags;
    unsigned char dlc;
    unsigned char reserved1;
    unsigned short reserved;
    unsigned char data[XL_CAN_MAX_DATA_LEN];
} XL_CAN_EV_TX_REQUEST;

typedef struct {
    unsigned char busStatus;
    unsigned char txErrorCounter;
    unsigned char rxErrorCounter;
    unsigned char reserved;
    unsigned int reserved0;
} XL_CAN_EV_CHIP_STATE;

#define XL_CAN_ERRC_BIT_ERROR 1
#define XL_CAN_ERRC_FORM_ERROR 2
#define XL_CAN_ERRC_STUFF_ERROR 3
#define XL_CAN_ERRC_OTHER_ERROR 4
#define XL_CAN_ERRC_CRC_ERROR 5
#define XL_CAN_ERRC_ACK_ERROR 6
#define XL_CAN_ERRC_NACK_ERROR 7
#define XL_CAN_ERRC_OVLD_ERROR 8
#define XL_CAN_ERRC_EXCPT_ERROR 9

typedef struct {
    unsigned char errorCode;
    unsigned char reserved[95];
} XL_CAN_EV_ERROR;

#define XL_CAN_QUEUE_OVERFLOW 0x100

// General RX event. The real union also has a canSyncPulse member; omitted
// here since raw[96] already fixes the union's size correctly (CANtrip
// never reads sync pulse events) - see XLbusParams above for the same
// "only vendor the members we use, but every size must still match" rule.
typedef struct {
    unsigned int size;
    unsigned short tag;
    unsigned short channelIndex;
    unsigned int userHandle;
    unsigned short flagsChip;
    unsigned short reserved0;
    XLuint64 reserved1;
    XLuint64 timeStampSync;
    union {
        unsigned char raw[XL_CANFD_MAX_EVENT_SIZE - XL_CANFD_RX_EVENT_HEADER_SIZE];
        XL_CAN_EV_RX_MSG canRxOkMsg;
        XL_CAN_EV_RX_MSG canTxOkMsg;
        XL_CAN_EV_TX_REQUEST canTxRequest;
        XL_CAN_EV_ERROR canError;
        XL_CAN_EV_CHIP_STATE canChipState;
    } tagData;
} XLcanRxEvent;

#include <poppack.h>

// ---- Function pointer typedefs for dynamic loading ----
typedef XLstatus(__stdcall* xlOpenDriver_t)(void);
typedef XLstatus(__stdcall* xlCloseDriver_t)(void);
typedef XLstatus(__stdcall* xlGetDriverConfig_t)(XLdriverConfig*);
typedef XLstatus(__stdcall* xlOpenPort_t)(XLportHandle*, char*, XLaccess, XLaccess*, unsigned int, unsigned int, unsigned int);
typedef XLstatus(__stdcall* xlClosePort_t)(XLportHandle);
typedef XLstatus(__stdcall* xlActivateChannel_t)(XLportHandle, XLaccess, unsigned int, unsigned int);
typedef XLstatus(__stdcall* xlDeactivateChannel_t)(XLportHandle, XLaccess);
typedef XLstatus(__stdcall* xlCanSetChannelBitrate_t)(XLportHandle, XLaccess, unsigned long);
typedef XLstatus(__stdcall* xlReceive_t)(XLportHandle, unsigned int*, XLevent*);
typedef XLstatus(__stdcall* xlCanTransmit_t)(XLportHandle, XLaccess, unsigned int*, void*);
typedef XLstringType(__stdcall* xlGetErrorString_t)(XLstatus);
typedef XLstatus(__stdcall* xlCanFdSetConfiguration_t)(XLportHandle, XLaccess, XLcanFdConf*);
typedef XLstatus(__stdcall* xlCanReceive_t)(XLportHandle, XLcanRxEvent*);
typedef XLstatus(__stdcall* xlCanTransmitEx_t)(XLportHandle, XLaccess, unsigned int, unsigned int*, XLcanTxEvent*);

} // extern "C"
