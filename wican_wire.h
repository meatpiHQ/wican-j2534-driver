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
 * wican_wire.h: the WiCAN J2534 wire protocol (mirrors the firmware's
 * components/j2534_server/include/j2534_proto.h). Keep the two in sync.
 */
#ifndef WICAN_WIRE_H
#define WICAN_WIRE_H

#include <stdint.h>

#define WW_MAGIC        0x4A35u   /* "J5" */
#define WW_VERSION      1         /* the header's version byte: the framing */
/* The wire version of a session, asked for in HELLO's payload and answered
 * in its ACK (1.10). Version 2: the status of an ACK is SAE J2534-1's own
 * number, and the device sends EVENT frames. A firmware before it ignores
 * the payload and answers 1: its statuses from 0x14 to 0x1A are the old
 * numbers, which the DLL turns into SAE's (status_from_device). */
#define WW_SESSION_VERSION 2
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

/* WW_EVENT payload (wire version 2): u32 event, u32 value; the header's
 * channel is the channel it is about. */
#define WW_EVENT_RX_LOST  1   /* value messages were lost in front of the
                                 channel's next WW_RX_MSG */

/* 12-byte header, little-endian: magic u16, ver u8, type u8, seq u16,
 * channel u16, length u32. PASSTHRU_MSG on the wire: protocol u32,
 * rx_status u32, tx_flags u32, timestamp u32, extra_data_index u32,
 * data_size u32, data[data_size].
 *
 * WW_IOCTL payload: u32 ioctl_id, u32 body_len, body[body_len]. An IOCTL
 *   without parameters is the id alone (4 bytes), as before TP2.0.
 *   SET_CONFIG / GET_CONFIG body: u32 count, then count pairs of
 *     u32 parameter, u32 value. GET_CONFIG's ACK is the status word,
 *     then that same count and the pairs with values filled.
 *   REQUEST_CONNECTION body is u32 11, then the 11 setup bytes
 *     (body_len 15). TEARDOWN_CONNECTION body is u32 4, then the RX id
 *     (body_len 8).
 *   GET_DEVICE_INFO body is u32 count (at most 16), then count pairs of
 *     u32 parameter, u32 value. Its ACK is the status word, the count, then
 *     count triples of u32 parameter, u32 value, u32 supported (the SPARAM
 *     list of SAE J2534-2 section 25). */

#endif /* WICAN_WIRE_H */
