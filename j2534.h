/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 MeatPi Electronics
 *
 * This file is part of the WiCAN J2534 PassThru driver, free software
 * under the GNU General Public License, version 3: see LICENSE.
 * It comes with NO WARRANTY and is used AT YOUR OWN RISK. The warranty
 * and liability terms in DISCLAIMER.txt are part of the licence
 * (additional terms, GPL section 7).
 */

/*
 * j2534.h — SAE J2534-1 (04.04) PassThru API surface (subset WiCAN
 * implements). Standard types/prototypes a J2534 application links against.
 */
#ifndef J2534_H
#define J2534_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* protocol IDs */
#define J1850VPW       1
#define CAN            5
#define ISO15765       6
#define TP2_0_PS       0x800E   /* SAE J2534-2 TP2.0 */

/* connect flags */
#define CAN_29BIT_ID           0x00000100

/* TxFlags / RxStatus */
#define ISO15765_FRAME_PAD     0x00000040
#define TX_MSG_TYPE            0x00000001
#define START_OF_MESSAGE       0x00000002
#define ISO15765_ADDR_TYPE     0x00000080
#define CONNECTION_ESTABLISHED 0x00010000   /* RxStatus: TP2.0 link is up */
#define CONNECTION_LOST        0x00020000   /* RxStatus: TP2.0 link ended */
#define TP2_0_BROADCAST_MSG    0x00010000   /* TxFlags */

/* filter types */
#define PASS_FILTER            0x00000001
#define BLOCK_FILTER           0x00000002
#define FLOW_CONTROL_FILTER    0x00000003

/* ioctl IDs */
#define GET_CONFIG             0x01
#define SET_CONFIG             0x02
#define READ_VBATT             0x03
#define CLEAR_TX_BUFFER        0x07
#define CLEAR_RX_BUFFER        0x08
#define CLEAR_PERIODIC_MSGS    0x09
#define CLEAR_MSG_FILTERS      0x0A
#define REQUEST_CONNECTION     0x800A  /* input: SBYTE_ARRAY, 11 bytes */
#define TEARDOWN_CONNECTION    0x800B  /* input: SBYTE_ARRAY, 4-byte RX id */
#define GET_DEVICE_INFO        0x800C  /* input: NULL; output: SPARAM_LIST */

/* config parameter IDs */
#define DATA_RATE              0x01
#define LOOPBACK               0x03
#define ISO15765_BS            0x1E
#define ISO15765_STMIN         0x1F
#define J1962_PINS             0x8001
#define TP2_0_T_BR_INT         0x8044
#define TP2_0_T_E              0x8045
#define TP2_0_MNTC             0x8046
#define TP2_0_T_CTA            0x8047
#define TP2_0_MNCT             0x8048
#define TP2_0_MNTB             0x8049
#define TP2_0_MNT              0x804A
#define TP2_0_T_WAIT           0x804B
#define TP2_0_T1               0x804C
#define TP2_0_T3               0x804D
#define TP2_0_IDENTIFER        0x804E  /* spelling as published for this id */
#define TP2_0_RXIDPASSIVE      0x804F

/* GET_DEVICE_INFO parameters (SAE J2534-2 table 121), the ones the WiCAN
 * answers; any other comes back not supported */
#define J1850VPW_SUPPORTED     0x03
#define CAN_SUPPORTED          0x06
#define ISO15765_SUPPORTED     0x07
#define TP2_0_SUPPORTED        0x13  /* value 0x00000100: one TP2_0_PS channel */
#define TP2_0_PS_J1962         0x28  /* value in: the pin bitmap asked about */
#define J1850VPW_SIMULTANEOUS  0x36
#define CAN_SIMULTANEOUS       0x39
#define ISO15765_SIMULTANEOUS  0x3A
#define TP2_0_SIMULTANEOUS     0x46

/* return codes */
#define STATUS_NOERROR              0x00
#define ERR_NOT_SUPPORTED           0x01
#define ERR_INVALID_CHANNEL_ID      0x02
#define ERR_INVALID_PROTOCOL_ID     0x03
#define ERR_NULL_PARAMETER          0x04
#define ERR_INVALID_IOCTL_VALUE     0x05
#define ERR_INVALID_FLAGS           0x06
#define ERR_FAILED                  0x07
#define ERR_DEVICE_NOT_CONNECTED    0x08
#define ERR_TIMEOUT                 0x09
#define ERR_INVALID_MSG             0x0A
#define ERR_BUFFER_EMPTY            0x10
#define ERR_BUFFER_FULL             0x11
#define ERR_INVALID_FILTER_ID       0x15
#define ERR_NO_FLOW_CONTROL         0x16
#define ERR_NOT_UNIQUE              0x17
#define ERR_INVALID_BAUDRATE        0x18
#define ERR_INVALID_DEVICE_ID       0x19
#define ERR_DEVICE_IN_USE           0x1A
#define ERR_NO_CONNECTION_ESTABLISHED 0x00010001   /* SAE J2534-2 table 123 */

typedef struct {
    unsigned long ProtocolID;
    unsigned long RxStatus;
    unsigned long TxFlags;
    unsigned long Timestamp;   /* microseconds */
    unsigned long DataSize;
    unsigned long ExtraDataIndex;
    unsigned char Data[4128];
} PASSTHRU_MSG;

typedef struct {
    unsigned long Parameter;
    unsigned long Value;
} SCONFIG;

typedef struct {
    unsigned long NumOfParams;
    SCONFIG *ConfigPtr;
} SCONFIG_LIST;

typedef struct {
    unsigned long NumOfBytes;
    unsigned char *BytePtr;
} SBYTE_ARRAY;

/* GET_DEVICE_INFO (SAE J2534-2 section 25) */
typedef struct {
    unsigned long Parameter;   /* in */
    unsigned long Value;       /* out, or in for the pin questions */
    unsigned long Supported;   /* out: 1 or 0 */
} SPARAM;

typedef struct {
    unsigned long NumOfParams;
    SPARAM *ParamPtr;
} SPARAM_LIST;

#ifdef _WIN32
#define J2534_API __declspec(dllexport) long __stdcall
#else
#define J2534_API long
#endif

J2534_API PassThruOpen(const void *pName, unsigned long *pDeviceID);
J2534_API PassThruClose(unsigned long DeviceID);
J2534_API PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID,
                          unsigned long Flags, unsigned long BaudRate,
                          unsigned long *pChannelID);
J2534_API PassThruDisconnect(unsigned long ChannelID);
J2534_API PassThruReadMsgs(unsigned long ChannelID, PASSTHRU_MSG *pMsg,
                           unsigned long *pNumMsgs, unsigned long Timeout);
J2534_API PassThruWriteMsgs(unsigned long ChannelID, PASSTHRU_MSG *pMsg,
                            unsigned long *pNumMsgs, unsigned long Timeout);
J2534_API PassThruStartPeriodicMsg(unsigned long ChannelID,
                                   const PASSTHRU_MSG *pMsg,
                                   unsigned long *pMsgID,
                                   unsigned long TimeInterval);
J2534_API PassThruStopPeriodicMsg(unsigned long ChannelID,
                                  unsigned long MsgID);
J2534_API PassThruStartMsgFilter(unsigned long ChannelID,
                                 unsigned long FilterType,
                                 const PASSTHRU_MSG *pMaskMsg,
                                 const PASSTHRU_MSG *pPatternMsg,
                                 const PASSTHRU_MSG *pFlowControlMsg,
                                 unsigned long *pFilterID);
J2534_API PassThruStopMsgFilter(unsigned long ChannelID,
                                unsigned long FilterID);
J2534_API PassThruSetProgrammingVoltage(unsigned long DeviceID,
                                        unsigned long Pin,
                                        unsigned long Voltage);
J2534_API PassThruReadVersion(unsigned long DeviceID, char *pFirmware,
                              char *pDll, char *pApi);
J2534_API PassThruGetLastError(char *pErrorDescription);
J2534_API PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID,
                        const void *pInput, void *pOutput);

#ifdef __cplusplus
}
#endif
#endif /* J2534_H */
