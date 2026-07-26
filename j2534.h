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
#define CAN            5
#define ISO15765       6

/* connect flags */
#define CAN_29BIT_ID           0x00000100

/* TxFlags / RxStatus */
#define ISO15765_FRAME_PAD     0x00000040
#define TX_MSG_TYPE            0x00000001
#define START_OF_MESSAGE       0x00000002
#define ISO15765_ADDR_TYPE     0x00000080

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

/* config parameter IDs */
#define DATA_RATE              0x01
#define LOOPBACK               0x03
#define ISO15765_BS            0x1E
#define ISO15765_STMIN         0x1F

/* return codes */
#define STATUS_NOERROR              0x00
#define ERR_NOT_SUPPORTED           0x01
#define ERR_INVALID_CHANNEL_ID      0x02
#define ERR_INVALID_PROTOCOL_ID     0x03
#define ERR_NULL_PARAMETER          0x04
#define ERR_INVALID_FLAGS           0x06
#define ERR_FAILED                  0x07
#define ERR_DEVICE_NOT_CONNECTED    0x08
#define ERR_TIMEOUT                 0x09
#define ERR_INVALID_MSG             0x0A
#define ERR_BUFFER_EMPTY            0x10
#define ERR_BUFFER_FULL             0x11
#define ERR_INVALID_FILTER_ID       0x15
#define ERR_NO_FLOW_CONTROL         0x16
#define ERR_INVALID_BAUDRATE        0x18
#define ERR_INVALID_DEVICE_ID       0x19
#define ERR_DEVICE_IN_USE           0x1A

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
