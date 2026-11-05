/*
 * wican_wire.h: the WiCAN J2534 wire protocol (mirrors the firmware's
 * components/j2534_server/include/j2534_proto.h). Keep the two in sync.
 */
#ifndef WICAN_WIRE_H
#define WICAN_WIRE_H

#include <stdint.h>

#define WW_MAGIC        0x4A35u   /* "J5" */
#define WW_VERSION      1
#define WW_HDR_SIZE     12
#define WW_MAX_DATA     4128
/* the device's request payload cap (J2534_RX_CAP): one WRITE_MSGS with
 * its count word and every message that fits (1.6 batches a call's
 * messages into one request) */
#define WW_REQ_CAP      (WW_MAX_DATA + 256)

/* message types */
#define WW_HELLO          0x01
#define WW_OPEN           0x02
#define WW_CLOSE          0x03
#define WW_CONNECT        0x04
#define WW_DISCONNECT     0x05
#define WW_WRITE_MSGS     0x10
#define WW_START_FILTER   0x11
#define WW_STOP_FILTER    0x12
#define WW_START_PERIODIC 0x13
#define WW_STOP_PERIODIC  0x14
#define WW_IOCTL          0x15
#define WW_ACK            0x80
#define WW_RX_MSG         0x81
#define WW_EVENT          0x82

/* 12-byte header, little-endian: magic u16, ver u8, type u8, seq u16,
 * channel u16, length u32. PASSTHRU_MSG on the wire: protocol u32,
 * rx_status u32, tx_flags u32, timestamp u32, extra_data_index u32,
 * data_size u32, data[data_size]. */

#endif /* WICAN_WIRE_H */
