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
 * wican_j2534.c: SAE J2534 PassThru DLL for WiCAN.
 *
 * Translates the J2534 API into the WiCAN wire protocol (wican_wire.h)
 * over one of two transports carrying the same framed protocol:
 *   - TCP: WiCAN reachable by IP, WiFi (STA or the device AP),
 *     USB-Ethernet, or USB device NCM/RNDIS.
 *   - Serial: WiCAN as a USB CDC-ACM virtual COM port
 *     (usb_host_manager.role=device, device_class=cdc; VID 16D0 PID 1263).
 *
 * Target resolution (priority):
 *   - env WICAN_J2534_COM = COMn | auto        -> serial
 *   - env WICAN_J2534_HOST / WICAN_J2534_PORT  -> TCP (COMn value -> serial)
 *   - registry HKLM\SOFTWARE\WiCAN\J2534 :
 *       Transport (REG_SZ "tcp"|"serial"), ComPort (REG_SZ, ""/"auto" =
 *       find by USB VID/PID), Host (REG_SZ; "COMn" -> serial), Port (DWORD),
 *       LogFile (REG_SZ, optional)
 *   - default 192.168.80.1 : 6809  (WiCAN SoftAP gateway)
 *
 * Driver log (on by default since 1.4): one line per API call, every
 * message's bytes in full both ways, at %PUBLIC%\Documents\WiCAN\
 * wican_j2534.log (16 MB, one rotation); env WICAN_J2534_LOG=<file> or the
 * registry LogFile move it, "off" disables it; every line also goes to
 * OutputDebugString (DebugView).
 *
 * RX model: WiCAN pushes RX_MSG frames; one reader thread owns the
 * transport read side and dispatches: ACK frames wake the pending
 * request (an event), RX_MSG frames go to a per-channel ring that
 * PassThruReadMsgs drains (networked-J2534 pattern; identical over serial).
 *
 * Messages are copied verbatim both ways: the ISO15765 layout (the 4-byte
 * CAN id first, then the payload, SAE J2534-1) is the device's business
 * (firmware of 2026-10-20 or later; older firmware took the bare payload
 * and every standard tool timed out).
 *
 * 1.2 (2026-10-19): event-driven ACK wait (the 10 ms sleep poll put a
 * 10 to 16 ms floor under every message), the debug log, GET_CONFIG
 * answers, requests serialized across threads. An empty read stays
 * ERR_BUFFER_EMPTY whatever the Timeout: udsoncan's released reader takes
 * ERR_TIMEOUT as data.
 *
 * 1.10 (2026-11-18): the return codes are SAE J2534-1's numbers (the
 * seven from ERR_MSG_PROTOCOL_ID on were one too low, ERR_DEVICE_IN_USE
 * was 0x1A), whatever the firmware: HELLO asks for wire version 2, where
 * the device answers SAE's numbers itself, and the numbers of a firmware
 * that answers 1 are turned into them here. PassThruReadMsgs returns
 * ERR_BUFFER_OVERFLOW when messages were lost in front of the first one
 * it hands over (the device's EVENT RX_LOST, or this DLL's own ring).
 *
 * Build: CMakeLists.txt (MSVC or MinGW). 32-bit is the safe default:
 * most OEM J2534 apps load a 32-bit DLL.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>   /* timeBeginPeriod: the write join window (1.6) */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "j2534.h"
#include "wican_wire.h"

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#endif

#define J2534_MAX_CH  8
#define RX_RING       256
#define DLL_VERSION   "WiCAN J2534 1.10"
/* longer than the device's longest WRITE (a multi-frame send at a slow
 * STmin is capped at 5.5 s on the device side) */
#define ACK_WAIT_MS   8000

/* USB ids of the WiCAN CDC-ACM function (WICAN_USB_DEV_PID_CDC): what
 * "auto" COM-port discovery matches in the Enum tree. Prefix, not exact:
 * CDC-ACM is a composite device, so the serial child enumerates under
 * VID_16D0&PID_1263&MI_00 (bench-hit 2026-07-18). */
#define WICAN_CDC_VIDPID "VID_16D0&PID_1263"

typedef struct {
    PASSTHRU_MSG msg[RX_RING];
    /* 1.10: messages were lost in front of msg[i] (SAE J2534-1 names the
     * first message received after a receive buffer overflow). Lost in the
     * device (its EVENT RX_LOST) or here (the ring was full). */
    unsigned char lost_before[RX_RING];
    unsigned long lost_pending;   /* lost since the last message pushed */
    unsigned long lost_total;     /* since PassThruOpen, for the log */
    volatile int head, tail;
    CRITICAL_SECTION cs;
    HANDLE ev;                /* auto-reset: a message was pushed (1.4) */
} rx_ring_t;

typedef struct {
    SOCKET sock;
    int    is_serial;
    HANDLE hcom;              /* CDC-ACM COM port (overlapped) */
    HANDLE rd_ev, wr_ev;      /* per-direction OVERLAPPED events */
    volatile int connected;
    /* 1.10: the wire version the device answered in HELLO. 1 = a firmware
     * whose statuses from 0x14 to 0x1A are the old numbers. */
    volatile unsigned wire_ver;
    unsigned seq;
    CRITICAL_SECTION tx_cs;   /* the write of one request */
    CRITICAL_SECTION req_cs;  /* one request/ACK cycle at a time */

    HANDLE reader;
    volatile int reader_run;

    /* ACK handoff from the reader thread to transact() */
    HANDLE                 ack_ev;      /* auto-reset: an ACK (or the link's end) */
    volatile unsigned      await_seq;
    volatile int           ack_ready;
    volatile unsigned long ack_status;
    volatile unsigned long ack_result;
    volatile int           ack_has_result;
    /* bytes after the ACK status word. GET_CONFIG's pairs live here.
     * Written by the reader before ack_ready, read by transact after it. */
    unsigned char          ack_body[1024];
    volatile unsigned      ack_body_len;

    /* 1.6: queued writes. Timeout 0, and a raw CAN frame at any Timeout,
     * return once the request is on the link (SAE J2534-1: "queue and
     * return"); the device's ACK comes in behind it on the reader thread,
     * and a refusal is kept for the next PassThruWriteMsgs call.
     * write_wait = every write waits for its ACK (the 1.5 behaviour;
     * registry WriteWait or env WICAN_J2534_WRITE_WAIT = 1). */
    int                    write_wait;
    volatile unsigned long deferred_status;   /* 0 = none pending */
    volatile unsigned      deferred_seq;

    /* 1.6: a queued write is held for about a millisecond so the writes
     * that follow it join the same request (a tool that sends a block
     * frame by frame, 15 calls in a row, costs the device ONE request
     * instead of 15; bench 2026-11-06: 11.2 min for a 2 MB write over
     * WiFi against 6.5 with the join). Any other call flushes first, so a
     * write followed by a read goes out at once. The flusher thread's
     * Sleep(1) is one timer period: timeBeginPeriod(1) while open. */
    CRITICAL_SECTION pend_cs;
    HANDLE           flush_ev;        /* auto-reset: something is pending */
    HANDLE           flusher;
    volatile int     flusher_run;
    int              timer_up;        /* timeBeginPeriod(1) is in effect */
    unsigned         pend_channel;
    unsigned         pend_count;
    unsigned         pend_n;          /* bytes used in pend_buf (4 + messages) */
    unsigned char    pend_buf[WW_REQ_CAP];

    rx_ring_t     ring[J2534_MAX_CH];
    unsigned long ch_proto[J2534_MAX_CH];
    unsigned long ch_baud[J2534_MAX_CH];
    int           ch_open[J2534_MAX_CH];
} device_t;

static device_t g_dev;
static int  g_open;
static char g_last_error[256] = "no error";

static void set_err(const char *m)
{ strncpy(g_last_error, m, sizeof(g_last_error) - 1); g_last_error[sizeof(g_last_error)-1] = 0; }
static const char *device_status_text(long st);   /* below; the reader thread uses it (1.6) */
static long flush_pending(void);                   /* below; transact() flushes a held write first (1.6) */

/* ---- the driver log ---------------------------------------------------- */
/*
 * On by default since 1.4 (2026-10-27): a user can always send it. One line
 * per API call, every message's bytes in full both ways (what went to the
 * ECU, what came back, with the device's microsecond timestamp on reads),
 * at a fixed place every user can write without admin rights:
 *   C:\Users\Public\Documents\WiCAN\wican_j2534.log   (%PUBLIC%\Documents)
 * Capped at LOG_MAX_BYTES, then rotated once to wican_j2534.log.1.
 * The registry `LogFile` or the env WICAN_J2534_LOG move it ("off" disables
 * it). Every line also goes to OutputDebugString (DebugView).
 */
#define LOG_MAX_BYTES (16ul * 1024ul * 1024ul)
#define LOG_LINE_CAP  (3 * WW_MAX_DATA + 256)   /* a full 4128-byte message as hex */

static FILE *g_log;                 /* NULL = file logging off */
static CRITICAL_SECTION g_log_cs;
static int g_log_cs_ready;
static char g_log_path[MAX_PATH];
static unsigned long g_log_bytes;
static char g_log_line[LOG_LINE_CAP];   /* under g_log_cs */

static void log_default_path(char *out, size_t n)
{
    const char *pub = getenv("PUBLIC");   /* C:\Users\Public on every Windows since Vista */
    char dir[MAX_PATH];
    _snprintf(dir, sizeof(dir), "%s\\Documents\\WiCAN", (pub && pub[0]) ? pub : "C:\\Users\\Public");
    dir[sizeof(dir) - 1] = 0;
    CreateDirectoryA(dir, NULL);          /* exists already: fine */
    _snprintf(out, n, "%s\\wican_j2534.log", dir);
    out[n - 1] = 0;
}

static void log_open(const char *path)
{
    if (!g_log_cs_ready) { InitializeCriticalSection(&g_log_cs); g_log_cs_ready = 1; }
    if (g_log) return;
    if (path && path[0] && _stricmp(path, "off") == 0) { g_log_path[0] = 0; return; }
    if (path && path[0]) { strncpy(g_log_path, path, sizeof(g_log_path) - 1); g_log_path[sizeof(g_log_path) - 1] = 0; }
    else log_default_path(g_log_path, sizeof(g_log_path));
    g_log = fopen(g_log_path, "a");
    if (!g_log) return;
    fseek(g_log, 0, SEEK_END);
    g_log_bytes = (unsigned long)ftell(g_log);
}

static void log_close(void)
{
    if (g_log) { fclose(g_log); g_log = NULL; }
}

/* under g_log_cs: the file passed the cap, keep one older copy */
static void log_rotate(void)
{
    char old[MAX_PATH + 4];
    fclose(g_log); g_log = NULL;
    _snprintf(old, sizeof(old), "%s.1", g_log_path); old[sizeof(old) - 1] = 0;
    DeleteFileA(old);
    MoveFileA(g_log_path, old);
    g_log = fopen(g_log_path, "w");
    g_log_bytes = 0;
}

static void log_line(const char *fmt, ...)
{
    SYSTEMTIME st;
    va_list ap;
    int n;

    if (!g_log_cs_ready) { InitializeCriticalSection(&g_log_cs); g_log_cs_ready = 1; }
    EnterCriticalSection(&g_log_cs);
    GetLocalTime(&st);
    n = _snprintf(g_log_line, sizeof(g_log_line), "%02d:%02d:%02d.%03d [%lu] ",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                  (unsigned long)GetCurrentThreadId());
    if (n < 0 || n >= (int)sizeof(g_log_line)) n = 0;
    va_start(ap, fmt);
    _vsnprintf(g_log_line + n, sizeof(g_log_line) - n - 2, fmt, ap);
    va_end(ap);
    g_log_line[sizeof(g_log_line) - 2] = 0;
    strcat(g_log_line, "\n");
    OutputDebugStringA(g_log_line);
    if (g_log) {
        fputs(g_log_line, g_log);
        fflush(g_log);
        g_log_bytes += (unsigned long)strlen(g_log_line);
        if (g_log_bytes > LOG_MAX_BYTES) log_rotate();
    }
    LeaveCriticalSection(&g_log_cs);
}

/* the first 16 bytes of a message as hex, "..." when there are more
 * (filter masks and patterns) */
static const char *hex16(const unsigned char *d, unsigned long n, char *out)
{
    unsigned long i, m = n < 16 ? n : 16;
    char *p = out;
    for (i = 0; i < m; i++) p += sprintf(p, "%s%02X", i ? " " : "", d[i]);
    if (n > m) strcpy(p, " ...");
    else if (n == 0) strcpy(p, "(none)");
    return out;
}

/* every byte of a message as hex (out holds 3 * WW_MAX_DATA + 8) */
static const char *hex_all(const unsigned char *d, unsigned long n, char *out)
{
    unsigned long i, m = n < WW_MAX_DATA ? n : WW_MAX_DATA;
    char *p = out;
    for (i = 0; i < m; i++) p += sprintf(p, "%s%02X", i ? " " : "", d[i]);
    if (n == 0) strcpy(p, "(none)");
    return out;
}

/* ---- LE helpers ------------------------------------------------------- */
static void put_u16(unsigned char *p, unsigned v){ p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); }
static void put_u32(unsigned char *p, unsigned long v){ p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24); }
static unsigned get_u16(const unsigned char *p){ return p[0] | (p[1]<<8); }
static unsigned long get_u32(const unsigned char *p){ return (unsigned long)p[0] | ((unsigned long)p[1]<<8) | ((unsigned long)p[2]<<16) | ((unsigned long)p[3]<<24); }

static int recv_all(SOCKET s, unsigned char *b, int n)
{ int g=0; while(g<n){ int r=recv(s,(char*)b+g,n-g,0); if(r<=0) return 0; g+=r; } return 1; }
static int send_all(SOCKET s, const unsigned char *b, int n)
{ int t=0; while(t<n){ int r=send(s,(const char*)b+t,n-t,0); if(r<=0) return 0; t+=r; } return 1; }

/* ---- serial (CDC-ACM COM port) transport ------------------------------ */

/* Open COMn overlapped: 8N1 (rate is cosmetic over CDC), DTR asserted
 * (the firmware gates the serial session on DTR), reads return within
 * 100 ms so the reader thread can poll reader_run. */
static HANDLE com_open(const char *name)
{
    char path[32];
    _snprintf(path, sizeof(path), "\\\\.\\%s", name);
    path[sizeof(path)-1] = 0;
    HANDLE h = CreateFileA(path, GENERIC_READ|GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) return h;

    DCB dcb; memset(&dcb, 0, sizeof(dcb)); dcb.DCBlength = sizeof(dcb);
    GetCommState(h, &dcb);
    dcb.BaudRate = CBR_115200; dcb.fBinary = TRUE;
    dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_ENABLE; dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutxCtsFlow = FALSE; dcb.fOutxDsrFlow = FALSE; dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE; dcb.fInX = FALSE; dcb.fNull = FALSE; dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) { CloseHandle(h); return INVALID_HANDLE_VALUE; }

    COMMTIMEOUTS to; memset(&to, 0, sizeof(to));
    to.ReadIntervalTimeout        = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant   = 100;
    to.WriteTotalTimeoutConstant  = 2000;
    SetCommTimeouts(h, &to);
    SetupComm(h, 16384, 16384);
    PurgeComm(h, PURGE_RXCLEAR|PURGE_TXCLEAR|PURGE_RXABORT|PURGE_TXABORT);
    return h;
}

/* "auto": walk HKLM\SYSTEM\...\Enum\USB\<key starting VID_16D0&PID_1263>
 * (prefix match: the CDC child sits under ...&MI_00) for PortName values
 * and return the first one that actually opens (Enum keys persist for
 * unplugged devices; a successful open is the presence test). The
 * SYSTEM hive is not WOW64-redirected, so this works from a 32-bit DLL. */
static HANDLE com_autodiscover(char *found, int flen)
{
    HKEY usb;
    HANDLE h = INVALID_HANDLE_VALUE;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Enum\\USB",
                      0, KEY_READ, &usb) != ERROR_SUCCESS)
        return INVALID_HANDLE_VALUE;
    for (DWORD d = 0; h == INVALID_HANDLE_VALUE; d++) {
        char dev[256]; DWORD dsz = sizeof(dev);
        if (RegEnumKeyExA(usb, d, dev, &dsz, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (_strnicmp(dev, WICAN_CDC_VIDPID, sizeof(WICAN_CDC_VIDPID)-1) != 0)
            continue;
        HKEY root;
        if (RegOpenKeyExA(usb, dev, 0, KEY_READ, &root) != ERROR_SUCCESS)
            continue;
        for (DWORD i = 0; h == INVALID_HANDLE_VALUE; i++) {
            char inst[256]; DWORD isz = sizeof(inst);
            if (RegEnumKeyExA(root, i, inst, &isz, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            char sub[300];
            _snprintf(sub, sizeof(sub), "%s\\Device Parameters", inst);
            sub[sizeof(sub)-1] = 0;
            HKEY dk;
            if (RegOpenKeyExA(root, sub, 0, KEY_READ, &dk) != ERROR_SUCCESS)
                continue;
            char port[16]; DWORD psz = sizeof(port), type;
            if (RegQueryValueExA(dk, "PortName", NULL, &type, (LPBYTE)port, &psz)
                    == ERROR_SUCCESS && type == REG_SZ) {
                port[sizeof(port)-1] = 0;
                h = com_open(port);
                if (h != INVALID_HANDLE_VALUE && found)
                    { strncpy(found, port, flen-1); found[flen-1] = 0; }
            }
            RegCloseKey(dk);
        }
        RegCloseKey(root);
    }
    RegCloseKey(usb);
    return h;
}

/* ---- transport-agnostic I/O (reader thread reads, tx_cs-holder writes) */

static int io_read_all(unsigned char *b, int n)
{
    if (!g_dev.is_serial) return recv_all(g_dev.sock, b, n);
    int got = 0;
    while (got < n) {
        if (!g_dev.reader_run) return 0;
        DWORD r = 0;
        OVERLAPPED ov; memset(&ov, 0, sizeof(ov)); ov.hEvent = g_dev.rd_ev;
        if (!ReadFile(g_dev.hcom, b + got, (DWORD)(n - got), &r, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) return 0;
            if (!GetOverlappedResult(g_dev.hcom, &ov, &r, TRUE)) return 0;
        }
        got += (int)r;   /* r==0 = 100 ms idle tick; loop re-checks reader_run */
    }
    return 1;
}

static int io_write_all(const unsigned char *b, int n)
{
    if (!g_dev.is_serial) return send_all(g_dev.sock, b, n);
    int put = 0;
    while (put < n) {
        DWORD w = 0;
        OVERLAPPED ov; memset(&ov, 0, sizeof(ov)); ov.hEvent = g_dev.wr_ev;
        if (!WriteFile(g_dev.hcom, b + put, (DWORD)(n - put), &w, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) return 0;
            if (!GetOverlappedResult(g_dev.hcom, &ov, &w, TRUE)) return 0;
        }
        if (w == 0) return 0;   /* write timeout */
        put += (int)w;
    }
    return 1;
}

static int encode_msg(const PASSTHRU_MSG *m, unsigned char *buf)
{
    put_u32(buf+0,m->ProtocolID); put_u32(buf+4,m->RxStatus);
    put_u32(buf+8,m->TxFlags);    put_u32(buf+12,m->Timestamp);
    put_u32(buf+16,m->ExtraDataIndex); put_u32(buf+20,m->DataSize);
    memcpy(buf+24, m->Data, m->DataSize);
    return 24 + (int)m->DataSize;
}

static void decode_msg(const unsigned char *buf, int len, PASSTHRU_MSG *m)
{
    memset(m,0,sizeof(*m));
    m->ProtocolID=get_u32(buf+0); m->RxStatus=get_u32(buf+4);
    m->TxFlags=get_u32(buf+8);    m->Timestamp=get_u32(buf+12);
    m->ExtraDataIndex=get_u32(buf+16); m->DataSize=get_u32(buf+20);
    if (m->DataSize > sizeof(m->Data)) m->DataSize = sizeof(m->Data);
    if (24 + (int)m->DataSize <= len) memcpy(m->Data, buf+24, m->DataSize);
}

/* ---- reader thread: single recv() owner ------------------------------ */

/* 1.10, SAE J2534-1 on a receive buffer overflow: new messages are
 * discarded while the buffer is full, and the first message received after
 * it tells the application that some are missing in front of it
 * (PassThruReadMsgs returns ERR_BUFFER_OVERFLOW for it). Until 1.9 a full
 * ring dropped the new message and nobody was told. Returns how many
 * messages this push found lost in front of it (0: none; -1: the ring is
 * full and this one is lost too). */
static long ring_push(rx_ring_t *r, const PASSTHRU_MSG *m)
{
    long lost = 0;
    EnterCriticalSection(&r->cs);
    int next = (r->head + 1) % RX_RING;
    if (next != r->tail) {
        lost = (long)r->lost_pending;
        r->lost_before[r->head] = r->lost_pending != 0;
        r->lost_pending = 0;
        r->msg[r->head] = *m; r->head = next;
    } else {
        r->lost_pending++; r->lost_total++;
        lost = -1;
    }
    LeaveCriticalSection(&r->cs);
    if (r->ev) SetEvent(r->ev);   /* wake a PassThruReadMsgs waiting on it */
    return lost;
}

/* the device lost n messages of the channel in front of its next one */
static void ring_lost(rx_ring_t *r, unsigned long n)
{
    EnterCriticalSection(&r->cs);
    r->lost_pending += n; r->lost_total += n;
    LeaveCriticalSection(&r->cs);
}

/* *lost: messages are missing in front of the one handed over */
static int ring_pop(rx_ring_t *r, PASSTHRU_MSG *out, int *lost)
{
    int got = 0;
    EnterCriticalSection(&r->cs);
    if (r->tail != r->head) {
        *out = r->msg[r->tail]; *lost = r->lost_before[r->tail];
        r->tail = (r->tail+1)%RX_RING; got = 1;
    }
    LeaveCriticalSection(&r->cs);
    return got;
}

/* the next message to hand over has messages missing in front of it */
static int ring_next_lost(rx_ring_t *r)
{
    int lost;
    EnterCriticalSection(&r->cs);
    lost = r->tail != r->head && r->lost_before[r->tail];
    LeaveCriticalSection(&r->cs);
    return lost;
}

/* a channel that closes takes its messages with it */
static void ring_clear(rx_ring_t *r)
{
    EnterCriticalSection(&r->cs);
    r->head = r->tail = 0; r->lost_pending = 0;
    LeaveCriticalSection(&r->cs);
}

/* 1.10: a status as the device sent it, in SAE J2534-1's numbers. A
 * firmware of wire version 1 numbers the seven codes from
 * ERR_MSG_PROTOCOL_ID on one too low (0x14 to 0x19) and sends
 * ERR_DEVICE_IN_USE as 0x1A; version 2 sends SAE's own. Before HELLO is
 * answered wire_ver is 1, and the one refusal a device sends there is
 * "in use": 0x1A from a version 1 firmware, 0x0E from a later one. */
static unsigned long status_from_device(unsigned long st)
{
    if (g_dev.wire_ver >= 2) return st;
    if (st >= 0x14 && st <= 0x19) return st + 1;
    if (st == 0x1A) return ERR_DEVICE_IN_USE;
    return st;
}

static DWORD WINAPI reader_thread(LPVOID arg)
{
    (void)arg;
    unsigned char hdr[WW_HDR_SIZE];
    static unsigned char payload[WW_MAX_DATA + 64];

    while (g_dev.reader_run) {
        if (!io_read_all(hdr, WW_HDR_SIZE)) break;
        if (get_u16(hdr) != WW_MAGIC) { log_line("reader: bad frame magic, link dropped"); break; }
        unsigned type = hdr[3], seq = get_u16(hdr+4), ch = get_u16(hdr+6);
        unsigned long len = get_u32(hdr+8);
        if (len > sizeof(payload)) { log_line("reader: frame too big (%lu), link dropped", len); break; }
        if (len && !io_read_all(payload, (int)len)) break;

        if (type == WW_ACK) {
            unsigned long st  = (len>=4) ? status_from_device(get_u32(payload)) : ERR_FAILED;
            unsigned long res = (len>=8) ? get_u32(payload+4) : 0;
            if (seq == g_dev.await_seq && !g_dev.ack_ready) {
                unsigned extra = (len > 4) ? (unsigned)(len - 4) : 0;
                if (extra > sizeof(g_dev.ack_body)) extra = (unsigned)sizeof(g_dev.ack_body);
                if (extra) memcpy(g_dev.ack_body, payload + 4, extra);
                g_dev.ack_body_len = extra;
                g_dev.ack_status = st; g_dev.ack_result = res;
                g_dev.ack_has_result = len >= 8;
                g_dev.ack_ready = 1; SetEvent(g_dev.ack_ev);
            } else if (st != STATUS_NOERROR && !g_dev.deferred_status) {
                /* a queued write's ACK (1.6): kept for the next write call */
                g_dev.deferred_seq = seq; g_dev.deferred_status = st;
                log_line("reader: the queued write seq %u was refused: status 0x%lX (%s)",
                         seq, st, device_status_text((long)st));
            }
        } else if (type == WW_RX_MSG && ch >= 1 && ch <= J2534_MAX_CH) {
            PASSTHRU_MSG m; decode_msg(payload, (int)len, &m);
            long lost = ring_push(&g_dev.ring[ch-1], &m);
            if (lost > 0)
                log_line("reader: channel %u: %ld message(s) were lost in front of this one (%lu since open): "
                         "PassThruReadMsgs will return ERR_BUFFER_OVERFLOW for it",
                         ch, lost, g_dev.ring[ch-1].lost_total);
            else if (lost < 0 && g_dev.ring[ch-1].lost_pending == 1)
                log_line("reader: channel %u: the application is not reading, %d messages wait: new ones are lost "
                         "until it does", ch, RX_RING - 1);
        } else if (type == WW_EVENT && len >= 8 && ch >= 1 && ch <= J2534_MAX_CH) {
            /* 1.10 (wire version 2): what the device has to say unasked */
            unsigned long ev = get_u32(payload), val = get_u32(payload + 4);
            if (ev == WW_EVENT_RX_LOST && val) {
                ring_lost(&g_dev.ring[ch-1], val);
                log_line("reader: channel %u: the WiCAN lost %lu received message(s) (its receive queue ran over)", ch, val);
            }
        }
    }
    g_dev.connected = 0;
    SetEvent(g_dev.ack_ev);   /* a waiting transact() learns of the end */
    for (int i = 0; i < J2534_MAX_CH; i++)
        if (g_dev.ring[i].ev) SetEvent(g_dev.ring[i].ev);   /* and so do the readers */
    log_line("reader: link closed");
    return 0;
}

/* 1.5: a device refusal in words for PassThruGetLastError. Until then a
 * status the WiCAN answered left "no error" behind, and a tool's dialog
 * said nothing useful (the field report of 2026-10-27). The codes are SAE
 * J2534-1's since 1.10: status_from_device() has already turned an older
 * firmware's numbers into them (J2534_WIRE_PROTOCOL.md section 5). */
static const char *device_status_text(long st)
{
    switch (st) {
    case 0x01: return "the WiCAN refused: ERR_NOT_SUPPORTED (0x01): a protocol this firmware does not run (older firmware: the ECU-flashing gate, 'Allow ECU flashing' off)";
    case 0x02: return "the WiCAN refused: ERR_INVALID_CHANNEL_ID (0x02): no such channel (disconnected, or the session was reset)";
    case 0x03: return "the WiCAN refused: ERR_INVALID_PROTOCOL_ID (0x03)";
    case 0x04: return "the WiCAN refused: ERR_NULL_PARAMETER (0x04)";
    case 0x05: return "the WiCAN refused: ERR_INVALID_IOCTL_VALUE (0x05): the ioctl value was rejected";
    case 0x06: return "the WiCAN refused: ERR_INVALID_FLAGS (0x06)";
    case 0x07: return "the WiCAN refused: ERR_FAILED (0x07): the CAN node is not transmitting (no bit rate proven on a silent bus with Automatic, a mismatch, listen-only: check /api/can), an ISO-TP send failed, or no free channel / periodic slot";
    case 0x08: return "the WiCAN refused: ERR_DEVICE_NOT_CONNECTED (0x08): CONNECT before OPEN";
    case 0x09: return "the WiCAN refused: ERR_TIMEOUT (0x09): the ISO-TP transfer timed out (no flow control or no answer from the ECU)";
    case 0x0A: return "the WiCAN refused: ERR_INVALID_MSG (0x0A): malformed message (a CAN frame is the 4-byte id + 0..8 bytes, an ISO15765 message the id + the payload, with ISO15765_ADDR_TYPE the id + the address extension + the payload; WiCAN firmware before 6.0.0-beta.13 refuses that flag)";
    case 0x0C: return "the WiCAN refused: ERR_EXCEEDED_LIMIT (0x0C): the channel's filter table or its ISO-TP sessions are full (10 filters; 10 sessions, 4 on WiCAN firmware before 6.0.0-beta.13)";
    case ERR_DEVICE_IN_USE:
               return "the WiCAN refused: ERR_DEVICE_IN_USE (0x0E): another tester is attached (TCP, USB serial or BLE)";
    case ERR_MSG_PROTOCOL_ID:
               return "the WiCAN refused: ERR_MSG_PROTOCOL_ID (0x15): the message's protocol does not match the channel";
    case ERR_INVALID_FILTER_ID:
               return "the WiCAN refused: ERR_INVALID_FILTER_ID (0x16)";
    case ERR_NO_FLOW_CONTROL:
               return "the WiCAN refused: ERR_NO_FLOW_CONTROL (0x17): a multi-frame ISO15765 message to an id with no FLOW_CONTROL filter";
    case ERR_NOT_UNIQUE:
               return "the WiCAN refused: ERR_NOT_UNIQUE (0x18): that id is already in use";
    case ERR_INVALID_BAUDRATE:
               return "the WiCAN refused: ERR_INVALID_BAUDRATE (0x19): the CAN bus runs at another bit rate (the WiCAN's setting, or what it read off the wire)";
    case ERR_INVALID_DEVICE_ID:
               return "the WiCAN refused: ERR_INVALID_DEVICE_ID (0x1A)";
    case ERR_NO_CONNECTION_ESTABLISHED:
               return "the WiCAN refused: ERR_NO_CONNECTION_ESTABLISHED (0x10001): no TP2.0 connection for that id";
    default:   return "the WiCAN refused the request (the status code is in the driver log)";
    }
}

/* send a request, wait for its ACK. Returns J2534 status; *result set. */
static long transact(unsigned char type, unsigned channel,
                     const unsigned char *payload, unsigned len,
                     unsigned long *result)
{
    unsigned char hdr[WW_HDR_SIZE];
    long st;
    if (!g_dev.connected) { set_err("not connected"); return ERR_DEVICE_NOT_CONNECTED; }
    flush_pending();   /* a held write goes before this request (1.6) */

    EnterCriticalSection(&g_dev.req_cs);
    EnterCriticalSection(&g_dev.tx_cs);
    g_dev.seq = (g_dev.seq + 1) & 0xFFFF;
    unsigned myseq = g_dev.seq;
    put_u16(hdr+0, WW_MAGIC); hdr[2]=WW_VERSION; hdr[3]=type;
    put_u16(hdr+4, myseq); put_u16(hdr+6, channel); put_u32(hdr+8, len);
    g_dev.await_seq = myseq; g_dev.ack_ready = 0;
    ResetEvent(g_dev.ack_ev);
    int ok = io_write_all(hdr, WW_HDR_SIZE) &&
             (!len || io_write_all(payload, len));
    LeaveCriticalSection(&g_dev.tx_cs);

    if (!ok) {
        LeaveCriticalSection(&g_dev.req_cs);
        set_err("device write failed"); return ERR_DEVICE_NOT_CONNECTED;
    }

    DWORD t0 = GetTickCount();
    while (!g_dev.ack_ready && g_dev.connected) {
        DWORD el = GetTickCount() - t0;
        if (el >= ACK_WAIT_MS) break;
        WaitForSingleObject(g_dev.ack_ev, ACK_WAIT_MS - el);
    }

    if (!g_dev.ack_ready) {
        LeaveCriticalSection(&g_dev.req_cs);
        if (!g_dev.connected) { set_err("device link lost"); return ERR_DEVICE_NOT_CONNECTED; }
        set_err("device ACK timeout"); return ERR_TIMEOUT;
    }

    st = (long)g_dev.ack_status;
    if (st == STATUS_NOERROR && result != NULL)
    {
        if (!g_dev.ack_has_result)
        {
            LeaveCriticalSection(&g_dev.req_cs);
            set_err("WiCAN returned no result; update the device firmware");
            return ERR_NOT_SUPPORTED;
        }
        *result = g_dev.ack_result;
    }
    LeaveCriticalSection(&g_dev.req_cs);
    if (st != STATUS_NOERROR) set_err(device_status_text(st));   /* 1.5 */
    return st;
}

/* 1.6: send a request and return as soon as it is on the link; the
 * reader thread takes its ACK (a refusal goes to deferred_status). Takes
 * only the link's write lock, so a transact() waiting on another thread
 * is not held up and its ACK is still matched by seq. */
static long send_nowait(unsigned char type, unsigned channel,
                        const unsigned char *payload, unsigned len,
                        unsigned *out_seq)
{
    unsigned char hdr[WW_HDR_SIZE];
    if (!g_dev.connected) { set_err("not connected"); return ERR_DEVICE_NOT_CONNECTED; }

    EnterCriticalSection(&g_dev.tx_cs);
    g_dev.seq = (g_dev.seq + 1) & 0xFFFF;
    unsigned myseq = g_dev.seq;
    put_u16(hdr+0, WW_MAGIC); hdr[2]=WW_VERSION; hdr[3]=type;
    put_u16(hdr+4, myseq); put_u16(hdr+6, channel); put_u32(hdr+8, len);
    int ok = io_write_all(hdr, WW_HDR_SIZE) &&
             (!len || io_write_all(payload, len));
    LeaveCriticalSection(&g_dev.tx_cs);

    if (out_seq) *out_seq = myseq;
    if (!ok) { set_err("device write failed"); return ERR_DEVICE_NOT_CONNECTED; }
    return STATUS_NOERROR;
}

/* the pending write request, if any, onto the link (pend_cs held) */
static long flush_locked(void)
{
    long st = STATUS_NOERROR;
    if (g_dev.pend_count) {
        unsigned seq = 0;
        put_u32(g_dev.pend_buf, g_dev.pend_count);
        st = send_nowait(WW_WRITE_MSGS, g_dev.pend_channel, g_dev.pend_buf, g_dev.pend_n, &seq);
        log_line("write request: channel %u, %u message(s) in one request, seq %u%s",
                 g_dev.pend_channel, g_dev.pend_count, seq, st != STATUS_NOERROR ? " (the link write failed)" : "");
        g_dev.pend_count = 0; g_dev.pend_n = 0;
    }
    return st;
}

static long flush_pending(void)
{
    long st;
    EnterCriticalSection(&g_dev.pend_cs);
    st = flush_locked();
    LeaveCriticalSection(&g_dev.pend_cs);
    return st;
}

static DWORD WINAPI flusher_thread(LPVOID arg)
{
    (void)arg;
    while (g_dev.flusher_run) {
        WaitForSingleObject(g_dev.flush_ev, INFINITE);
        if (!g_dev.flusher_run) break;
        Sleep(1);               /* the join window */
        flush_pending();
    }
    return 0;
}

/* ---- connection setup ------------------------------------------------- */

typedef struct {
    int  write_wait;  /* 1.6: every PassThruWriteMsgs waits for the device's ACK */
    int  serial;      /* 1 = CDC-ACM COM port, 0 = TCP */
    char com[16];     /* "COM7"; "" = auto-discover by USB VID/PID */
    char host[128];
    unsigned short port;
    char log[260];    /* debug log file; "" = off */
} target_t;

/* "COM<digits>" (case-insensitive)? Lets Host / WICAN_J2534_HOST name a
 * COM port directly, so single-value configs can pick serial too. */
static int is_com_name(const char *s)
{
    return (s[0]=='C'||s[0]=='c') && (s[1]=='O'||s[1]=='o') &&
           (s[2]=='M'||s[2]=='m') && s[3] >= '0' && s[3] <= '9';
}

static void resolve_target(target_t *t)
{
    char *e;
    memset(t, 0, sizeof(*t));
    strcpy(t->host, "192.168.80.1"); t->port = 6809;

    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\WiCAN\\J2534", 0,
                      KEY_READ, &k) == ERROR_SUCCESS) {
        char tr[16] = ""; DWORD sz, type;
        sz = sizeof(t->host);
        RegQueryValueExA(k, "Host", NULL, &type, (LPBYTE)t->host, &sz);
        DWORD p = 0; sz = sizeof(p);
        if (RegQueryValueExA(k, "Port", NULL, &type, (LPBYTE)&p, &sz) == ERROR_SUCCESS && p)
            t->port = (unsigned short)p;
        sz = sizeof(tr);
        RegQueryValueExA(k, "Transport", NULL, &type, (LPBYTE)tr, &sz);
        if (_stricmp(tr, "serial") == 0) t->serial = 1;
        sz = sizeof(t->com);
        RegQueryValueExA(k, "ComPort", NULL, &type, (LPBYTE)t->com, &sz);
        sz = sizeof(t->log);
        RegQueryValueExA(k, "LogFile", NULL, &type, (LPBYTE)t->log, &sz);
        DWORD ww = 0; sz = sizeof(ww);
        if (RegQueryValueExA(k, "WriteWait", NULL, &type, (LPBYTE)&ww, &sz) == ERROR_SUCCESS)
            t->write_wait = ww != 0;
        RegCloseKey(k);
    }
    if ((e = getenv("WICAN_J2534_WRITE_WAIT"))) t->write_wait = atoi(e) != 0;
    t->host[sizeof(t->host)-1] = 0; t->com[sizeof(t->com)-1] = 0; t->log[sizeof(t->log)-1] = 0;
    if (is_com_name(t->host)) {          /* Host="COM7" shorthand */
        t->serial = 1;
        strncpy(t->com, t->host, sizeof(t->com)-1);
    }

    if ((e = getenv("WICAN_J2534_HOST"))) {
        if (is_com_name(e)) {
            t->serial = 1;
            strncpy(t->com, e, sizeof(t->com)-1); t->com[sizeof(t->com)-1]=0;
        } else {
            t->serial = 0;
            strncpy(t->host, e, sizeof(t->host)-1); t->host[sizeof(t->host)-1]=0;
        }
    }
    if ((e = getenv("WICAN_J2534_PORT"))) { int p = atoi(e); if (p>0) t->port = (unsigned short)p; }
    if ((e = getenv("WICAN_J2534_COM"))) {  /* strongest: force serial */
        t->serial = 1;
        strncpy(t->com, e, sizeof(t->com)-1); t->com[sizeof(t->com)-1]=0;
    }
    if ((e = getenv("WICAN_J2534_LOG"))) {
        strncpy(t->log, e, sizeof(t->log)-1); t->log[sizeof(t->log)-1]=0;
    }
    if (_stricmp(t->com, "auto") == 0) t->com[0] = 0;
}

/* Stop the reader and close whichever transport is up. TCP: close the
 * socket first to unblock recv(). Serial: the reader's ReadFile returns
 * within its 100 ms timeout, so wait for the thread BEFORE closing the
 * handle it reads from; drop DTR so the firmware ends the session. */
static void transport_teardown(void)
{
    g_dev.reader_run = 0; g_dev.connected = 0;
    if (g_dev.flusher) {           /* 1.6: the join-window thread first */
        g_dev.flusher_run = 0; SetEvent(g_dev.flush_ev);
        WaitForSingleObject(g_dev.flusher, 1000); CloseHandle(g_dev.flusher); g_dev.flusher = NULL;
    }
    if (g_dev.timer_up) { timeEndPeriod(1); g_dev.timer_up = 0; }
    if (g_dev.is_serial) {
        if (g_dev.reader) { WaitForSingleObject(g_dev.reader, 1500); CloseHandle(g_dev.reader); }
        if (g_dev.hcom && g_dev.hcom != INVALID_HANDLE_VALUE) {
            EscapeCommFunction(g_dev.hcom, CLRDTR);
            CloseHandle(g_dev.hcom);
        }
        if (g_dev.rd_ev) CloseHandle(g_dev.rd_ev);
        if (g_dev.wr_ev) CloseHandle(g_dev.wr_ev);
        g_dev.hcom = NULL; g_dev.rd_ev = NULL; g_dev.wr_ev = NULL;
    } else {
        shutdown(g_dev.sock, SD_BOTH); closesocket(g_dev.sock);
        if (g_dev.reader) { WaitForSingleObject(g_dev.reader, 1000); CloseHandle(g_dev.reader); }
        WSACleanup();
    }
    g_dev.reader = NULL;
    if (g_dev.ack_ev) { CloseHandle(g_dev.ack_ev); g_dev.ack_ev = NULL; }
}

static void locks_delete(void)
{
    DeleteCriticalSection(&g_dev.tx_cs);
    DeleteCriticalSection(&g_dev.req_cs);
    DeleteCriticalSection(&g_dev.pend_cs);
    if (g_dev.flush_ev) { CloseHandle(g_dev.flush_ev); g_dev.flush_ev = NULL; }
    for (int i=0;i<J2534_MAX_CH;i++) {
        DeleteCriticalSection(&g_dev.ring[i].cs);
        if (g_dev.ring[i].ev) { CloseHandle(g_dev.ring[i].ev); g_dev.ring[i].ev = NULL; }
    }
}

/* ---- J2534 API -------------------------------------------------------- */

J2534_API PassThruOpen(const void *pName, unsigned long *pDeviceID)
{
    (void)pName;
    if (g_open) { set_err("already open"); return ERR_DEVICE_IN_USE; }

    target_t tgt;
    resolve_target(&tgt);
    log_open(tgt.log);
    memset(&g_dev, 0, sizeof(g_dev));
    g_dev.write_wait = tgt.write_wait;

    if (tgt.serial) {
        HANDLE h = tgt.com[0] ? com_open(tgt.com)
                              : com_autodiscover(tgt.com, sizeof(tgt.com));
        if (h == INVALID_HANDLE_VALUE) {
            set_err(tgt.com[0]
                ? "COM port open failed (in use, or wrong port?)"
                : "no WiCAN COM port found (VID 16D0 PID 1263 - is the WiCAN plugged in and in device/cdc mode?)");
            log_line("PassThruOpen: serial %s: %s", tgt.com[0] ? tgt.com : "auto", g_last_error);
            return ERR_DEVICE_NOT_CONNECTED;
        }
        g_dev.is_serial = 1; g_dev.hcom = h;
        g_dev.rd_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
        g_dev.wr_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!g_dev.rd_ev || !g_dev.wr_ev) {
            transport_teardown(); set_err("CreateEvent failed"); return ERR_FAILED;
        }
    } else {
        WSADATA w; if (WSAStartup(MAKEWORD(2,2), &w) != 0) { set_err("WSAStartup"); return ERR_FAILED; }

        struct addrinfo hints, *res = NULL; char pstr[8];
        memset(&hints,0,sizeof(hints)); hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
        sprintf(pstr, "%u", tgt.port);
        if (getaddrinfo(tgt.host, pstr, &hints, &res) != 0) {
            set_err("resolve failed"); WSACleanup();
            log_line("PassThruOpen: tcp %s:%u: resolve failed", tgt.host, tgt.port);
            return ERR_DEVICE_NOT_CONNECTED;
        }

        SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s == INVALID_SOCKET || connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
            freeaddrinfo(res); if (s!=INVALID_SOCKET) closesocket(s);
            set_err("connect failed"); WSACleanup();
            log_line("PassThruOpen: tcp %s:%u: connect failed", tgt.host, tgt.port);
            return ERR_DEVICE_NOT_CONNECTED;
        }
        freeaddrinfo(res);
        int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof(one));
        g_dev.sock = s;
    }

    g_dev.ack_ev = CreateEventA(NULL, FALSE, FALSE, NULL);   /* auto-reset */
    if (!g_dev.ack_ev) { transport_teardown(); set_err("CreateEvent failed"); return ERR_FAILED; }
    g_dev.connected = 1;
    InitializeCriticalSection(&g_dev.tx_cs);
    InitializeCriticalSection(&g_dev.req_cs);
    InitializeCriticalSection(&g_dev.pend_cs);
    g_dev.flush_ev = CreateEventA(NULL, FALSE, FALSE, NULL);         /* auto-reset (1.6) */
    for (int i=0;i<J2534_MAX_CH;i++) {
        InitializeCriticalSection(&g_dev.ring[i].cs);
        g_dev.ring[i].ev = CreateEventA(NULL, FALSE, FALSE, NULL);   /* auto-reset (1.4) */
    }
    g_dev.reader_run = 1;
    g_dev.reader = CreateThread(NULL, 0, reader_thread, NULL, 0, NULL);
    timeBeginPeriod(1);            /* the join window's Sleep(1) = 1 ms, not 15.6 */
    g_dev.timer_up = 1;
    g_dev.flusher_run = 1;
    g_dev.flusher = g_dev.flush_ev ? CreateThread(NULL, 0, flusher_thread, NULL, 0, NULL) : NULL;

    /* 1.10: HELLO asks for wire version 2 (SAE's status numbers, EVENT
     * frames). A firmware before it ignores the payload and answers 1. */
    unsigned long result = 0;
    unsigned char p[4]; put_u32(p, WW_SESSION_VERSION);
    g_dev.wire_ver = 1;
    long hst = transact(WW_HELLO, 0, p, 4, &result);
    if (hst != STATUS_NOERROR) {
        char why[sizeof(g_last_error)];
        strcpy(why, g_last_error);
        transport_teardown(); locks_delete();
        log_line("PassThruOpen: HELLO failed: status 0x%lX (%s)", (unsigned long)hst, why);
        if (hst == ERR_DEVICE_IN_USE) {
            /* the device said so: another tester holds it (1.10: the
             * application gets the standard's code, ERR_FAILED before) */
            set_err(device_status_text(hst));
            return ERR_DEVICE_IN_USE;
        }
        set_err("HELLO failed");
        return ERR_FAILED;
    }
    g_dev.wire_ver = result >= 2 ? 2 : 1;
    unsigned long dev = 0;
    if (transact(WW_OPEN, 0, NULL, 0, &dev) != STATUS_NOERROR) {
        transport_teardown(); locks_delete();
        set_err("OPEN failed");
        log_line("PassThruOpen: OPEN failed");
        return ERR_FAILED;
    }

    g_open = 1;
    if (pDeviceID) *pDeviceID = dev ? dev : 1;
    set_err("no error");
    log_line("PassThruOpen: %s %s%s: ok, wire v%lu%s, device %lu (" DLL_VERSION "); log %s",
             tgt.serial ? "serial" : "tcp", tgt.serial ? tgt.com : tgt.host,
             tgt.serial ? "" : "", result,
             g_dev.wire_ver >= 2 ? "" : " (a firmware before wire version 2: its status numbers are turned into SAE's here, "
                                        "and it does not report lost messages)",
             dev ? dev : 1,
             g_log ? g_log_path : "off (OutputDebugString only)");
    return STATUS_NOERROR;
}

J2534_API PassThruClose(unsigned long DeviceID)
{
    (void)DeviceID;
    if (!g_open) return ERR_INVALID_DEVICE_ID;
    transact(WW_CLOSE, 0, NULL, 0, NULL);
    transport_teardown();
    locks_delete();
    g_open = 0;
    log_line("PassThruClose: ok");
    log_close();
    return STATUS_NOERROR;
}

J2534_API PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID,
                          unsigned long Flags, unsigned long BaudRate,
                          unsigned long *pChannelID)
{
    (void)DeviceID;
    if (!g_open) return ERR_INVALID_DEVICE_ID;
    /* CAN + ISO15765 on the native controller; J1850 VPW (GM Class 2) on
     * the device's OBD chip; TP2.0 (0x800E) is forwarded and the device
     * runs the state machine. For J1850 the app puts the whole message
     * ([3-byte header][payload]) in PASSTHRU_MSG.Data with no CAN-id
     * prefix, and the DLL passes it through verbatim like every message. */
    if (ProtocolID != CAN && ProtocolID != ISO15765 &&
        ProtocolID != J1850VPW && ProtocolID != TP2_0_PS) {
        log_line("PassThruConnect: protocol %lu: ERR_INVALID_PROTOCOL_ID", ProtocolID);
        return ERR_INVALID_PROTOCOL_ID;
    }

    unsigned char p[20];
    put_u32(p+0, ProtocolID); put_u32(p+4, Flags); put_u32(p+8, BaudRate);
    put_u32(p+12, 0); put_u32(p+16, 0);           /* tx/rx via FLOW_CONTROL filter */
    unsigned long chid = 0;
    long st = transact(WW_CONNECT, 0, p, sizeof(p), &chid);
    log_line("PassThruConnect: protocol %lu flags 0x%lX baud %lu: status 0x%lX channel %lu",
             ProtocolID, Flags, BaudRate, (unsigned long)st, chid);
    if (st != STATUS_NOERROR) return st;
    if (chid < 1 || chid > J2534_MAX_CH) return ERR_FAILED;
    g_dev.ch_proto[chid-1] = ProtocolID; g_dev.ch_open[chid-1] = 1;
    g_dev.ch_baud[chid-1] = BaudRate;
    if (pChannelID) *pChannelID = chid;
    return STATUS_NOERROR;
}

J2534_API PassThruDisconnect(unsigned long ChannelID)
{
    if (!g_open || ChannelID<1 || ChannelID>J2534_MAX_CH) return ERR_INVALID_CHANNEL_ID;
    long st = transact(WW_DISCONNECT, (unsigned)ChannelID, NULL, 0, NULL);
    g_dev.ch_open[ChannelID-1] = 0;
    ring_clear(&g_dev.ring[ChannelID-1]);   /* 1.10: the next channel with this id starts empty */
    log_line("PassThruDisconnect: channel %lu: status 0x%lX", ChannelID, (unsigned long)st);
    return st;
}

J2534_API PassThruReadMsgs(unsigned long ChannelID, PASSTHRU_MSG *pMsg,
                           unsigned long *pNumMsgs, unsigned long Timeout)
{
    if (!g_open || ChannelID<1 || ChannelID>J2534_MAX_CH) return ERR_INVALID_CHANNEL_ID;
    if (!pMsg || !pNumMsgs) return ERR_NULL_PARAMETER;
    flush_pending();   /* a held write goes out before the tool waits for its answer (1.6) */
    unsigned long want = *pNumMsgs, got = 0;
    rx_ring_t *r = &g_dev.ring[ChannelID-1];
    DWORD deadline = GetTickCount() + Timeout;
    /* Every protocol but TP2_0_PS (the case that asked for it is raw CAN):
     * once any message is in hand, a few quiet milliseconds mean the burst
     * stopped. MED9 asks for a whole TP2.0 block in one call
     * (up to 20 ms). The ECU sends the block-end and then waits for the
     * tester's ACK, so filling that request holds the block-end until the
     * timeout. Bench 2026-11-15: 940 of those waits, median 34 ms, 343 of
     * them about 60 ms, 35 s of a 66 s read. Frames on this bus are about
     * 2 ms apart, so 4 ms of silence is the end of the burst, not a gap
     * inside it. An empty read still waits out Timeout.
     * TP2_0_PS is different: the device already reassembled the message,
     * so a read returns what is queued and does not sit on it. */
    int coalesce = g_dev.ch_proto[ChannelID - 1] != TP2_0_PS;
    /* 1.10: a message with others lost in front of it is the FIRST of the
     * call that returns it, and that call returns ERR_BUFFER_OVERFLOW (SAE
     * J2534-1: the messages read are valid, pNumMsgs counts them). So the
     * application knows where the gap is without a status bit: in front of
     * what this call returned. A call that is filling up stops before it. */
    int overflow = 0;
    while (got < want) {
        int lost = 0;
        if (got > 0 && ring_next_lost(r)) break;
        if (ring_pop(r, &pMsg[got], &lost)) { if (lost) overflow = 1; got++; continue; }
        if (!coalesce && got > 0) break;
        if (Timeout == 0 || !g_dev.connected) break;
        long left = (long)(deadline - GetTickCount());
        if (left <= 0) break;
        /* 1.4: wait for the reader thread's push instead of a Sleep(1)
         * poll. Sleep(1) is the Windows timer period (15.6 ms unless some
         * app raised it), so every frame a tool waited for cost up to a
         * timer tick; bench 2026-10-27: 19 ms per TP 2.0 block-end ACK. */
        DWORD slice = (DWORD)left;
        if (coalesce && got > 0 && slice > 4) slice = 4;
        if (r->ev) {
            DWORD w = WaitForSingleObject(r->ev, slice);
            if (coalesce && got > 0 && w == WAIT_TIMEOUT) break;
        } else {
            Sleep(1);   /* no event (CreateEvent failed at open): the old poll */
            if (got > 0) break;
        }
    }
    *pNumMsgs = got;
    if (got) {
        char hx[3 * WW_MAX_DATA + 8];
        for (unsigned long i = 0; i < got; i++)
            log_line("PassThruReadMsgs: channel %lu rx_status 0x%lX ts %lu size %lu [%s]",
                     ChannelID, pMsg[i].RxStatus, pMsg[i].Timestamp, pMsg[i].DataSize,
                     hex_all(pMsg[i].Data, pMsg[i].DataSize, hx));
        if (overflow) {
            set_err("ERR_BUFFER_OVERFLOW (0x12): received messages were lost in front of the first message of this read "
                    "(a receive buffer ran over; the driver log says where)");
            log_line("PassThruReadMsgs: channel %lu: ERR_BUFFER_OVERFLOW with %lu message(s): others were lost in front of the first",
                     ChannelID, got);
            return ERR_BUFFER_OVERFLOW;
        }
        return STATUS_NOERROR;
    }
    /* Nothing read: ERR_BUFFER_EMPTY whatever the Timeout, as 1.1 and the
     * common devices answer. SAE J2534-1 names ERR_TIMEOUT for an expired
     * non-zero Timeout, but udsoncan 1.26's reader (1 ms polls) takes
     * ERR_TIMEOUT as data and queues empty payloads (bench-hit 2026-10-19). */
    return ERR_BUFFER_EMPTY;
}

/* 1.6 (the VW TP 2.0 speed report of 2026-11-05): a tool that acknowledges
 * every block of a transfer paid the device's round trip on every ACK it
 * wrote, because every message was its own request and the call waited
 * for the device's ACK. Now: one WRITE_MSGS request per call with every
 * message in it (the device takes a batch up to its request cap), and
 * with Timeout 0 the call returns once the request is on the link, as
 * SAE J2534-1 says ("queue and return"); the device's answer comes in on
 * the reader thread, and a refusal is reported by the NEXT call, which
 * then sends nothing. WriteWait, ISO15765, and J1850 still wait.
 *
 * CAN frames (MED9, 2026-11-15): that tool passes Timeout 20 on every raw
 * CAN frame, so the wait above never let the join window run. The driver
 * log of a 64 KiB read shows about 9 ms from the previous call to each
 * write's return, a couple of thousand times (74 s on WiCAN, 40 s on
 * OpenPort). A raw CAN frame is the 4-byte id plus at most 8 data bytes.
 * Those are handed to the link before the call returns, and the call does
 * not wait for the device's ACK. The tool can drain frames it already
 * pulled before it reads again, so the frame cannot stay in the join
 * window until that next read. */
J2534_API PassThruWriteMsgs(unsigned long ChannelID, PASSTHRU_MSG *pMsg,
                            unsigned long *pNumMsgs, unsigned long Timeout)
{
    if (!g_open || ChannelID<1 || ChannelID>J2534_MAX_CH) return ERR_INVALID_CHANNEL_ID;
    if (!pMsg || !pNumMsgs) return ERR_NULL_PARAMETER;
    static unsigned char buf[WW_REQ_CAP];
    unsigned long want = *pNumMsgs, sent = 0;
    char hx[3 * WW_MAX_DATA + 8];
    int raw_can = want > 0;
    for (unsigned long i = 0; i < want; i++) {
        const PASSTHRU_MSG *m = &pMsg[i];
        if (m->ProtocolID != CAN || m->DataSize < 4 || m->DataSize > 12)
            raw_can = 0;
    }
    (void)Timeout;
    int wait = g_dev.write_wait || !raw_can;

    if (g_dev.deferred_status) {
        long st = (long)g_dev.deferred_status;
        g_dev.deferred_status = 0;
        set_err(device_status_text(st));
        log_line("PassThruWriteMsgs: channel %lu: the queued write seq %u was refused "
                 "(status 0x%lX): reported now, nothing of this call sent",
                 ChannelID, g_dev.deferred_seq, (unsigned long)st);
        *pNumMsgs = 0;
        return st;
    }

    if (!wait) {
        /* queue and return: into the pending request, which the flusher
         * sends after the join window (or the next call sends first) */
        if (!g_dev.connected) { *pNumMsgs = 0; set_err("not connected"); return ERR_DEVICE_NOT_CONNECTED; }
        for (unsigned long i = 0; i < want; i++) {
            const PASSTHRU_MSG *m = &pMsg[i];
            if (m->DataSize > WW_MAX_DATA) { *pNumMsgs = sent; set_err("message too long"); return ERR_INVALID_MSG; }
            EnterCriticalSection(&g_dev.pend_cs);
            if (g_dev.pend_count &&
                (g_dev.pend_channel != (unsigned)ChannelID ||
                 g_dev.pend_n + 24 + m->DataSize > sizeof(g_dev.pend_buf)))
                flush_locked();
            if (!g_dev.pend_count) { g_dev.pend_channel = (unsigned)ChannelID; g_dev.pend_n = 4; }
            g_dev.pend_n += (unsigned)encode_msg(m, g_dev.pend_buf + g_dev.pend_n);
            g_dev.pend_count++;
            int first = g_dev.pend_count == 1;
            LeaveCriticalSection(&g_dev.pend_cs);
            if (first && g_dev.flush_ev) SetEvent(g_dev.flush_ev);
            log_line("PassThruWriteMsgs: channel %lu protocol %lu tx_flags 0x%lX size %lu [%s]: queued",
                     ChannelID, m->ProtocolID, m->TxFlags, m->DataSize, hex_all(m->Data, m->DataSize, hx));
            sent++;
        }
        /* A raw CAN frame has to leave before this call returns. The MED9
         * tool often still holds frames from the last read, and it will
         * not call PassThruReadMsgs (which flushes) until that queue is
         * empty. Timeout 0 for any other protocol keeps the join window. */
        if (raw_can || !g_dev.flusher) {
            long st = flush_pending();
            if (st != STATUS_NOERROR) { *pNumMsgs = 0; return st; }
        }
        *pNumMsgs = sent;
        return STATUS_NOERROR;
    }

    flush_pending();   /* in order: what was queued goes before this call's messages */
    while (sent < want) {
        unsigned n = 4, count = 0, seq = 0;
        unsigned long first = sent;
        while (sent + count < want) {
            const PASSTHRU_MSG *m = &pMsg[sent + count];
            if (m->DataSize > WW_MAX_DATA) { *pNumMsgs = sent; set_err("message too long"); return ERR_INVALID_MSG; }
            if (count > 0 && n + 24 + m->DataSize > sizeof(buf)) break;   /* the next request */
            n += (unsigned)encode_msg(m, buf + n);
            count++;
        }
        put_u32(buf, count);
        (void)seq;
        long st = transact(WW_WRITE_MSGS, (unsigned)ChannelID, buf, n, NULL);
        for (unsigned long i = first; i < first + count; i++)
            log_line("PassThruWriteMsgs: channel %lu protocol %lu tx_flags 0x%lX size %lu [%s]: status 0x%lX%s",
                     ChannelID, pMsg[i].ProtocolID, pMsg[i].TxFlags, pMsg[i].DataSize,
                     hex_all(pMsg[i].Data, pMsg[i].DataSize, hx), (unsigned long)st,
                     count > 1 ? " (one request for the batch)" : "");
        if (st != STATUS_NOERROR) { *pNumMsgs = sent; return st; }
        sent += count;
    }
    *pNumMsgs = sent;
    return STATUS_NOERROR;
}

J2534_API PassThruStartMsgFilter(unsigned long ChannelID,
                                 unsigned long FilterType,
                                 const PASSTHRU_MSG *pMaskMsg,
                                 const PASSTHRU_MSG *pPatternMsg,
                                 const PASSTHRU_MSG *pFlowControlMsg,
                                 unsigned long *pFilterID)
{
    if (!g_open || ChannelID<1 || ChannelID>J2534_MAX_CH) return ERR_INVALID_CHANNEL_ID;
    static unsigned char buf[4 + 3*(24+16)];
    unsigned char *p = buf;
    char h1[80], h2[80], h3[80];
    put_u32(p, FilterType); p += 4;
    PASSTHRU_MSG empty; memset(&empty, 0, sizeof(empty));
    const PASSTHRU_MSG *mk = pMaskMsg ? pMaskMsg : &empty;
    const PASSTHRU_MSG *pt = pPatternMsg ? pPatternMsg : &empty;
    if (mk->DataSize > 16 || pt->DataSize > 16 ||
        (pFlowControlMsg && pFlowControlMsg->DataSize > 16)) {
        set_err("filter message too long"); return ERR_INVALID_MSG;
    }
    p += encode_msg(mk, p);
    p += encode_msg(pt, p);
    if (pFlowControlMsg) p += encode_msg(pFlowControlMsg, p);
    unsigned long fid = 0;
    long st = transact(WW_START_FILTER, (unsigned)ChannelID, buf,
                       (unsigned)(p - buf), &fid);
    log_line("PassThruStartMsgFilter: channel %lu type %lu mask [%s] pattern [%s] flow [%s] tx_flags 0x%lX: status 0x%lX filter %lu",
             ChannelID, FilterType, hex16(mk->Data, mk->DataSize, h1),
             hex16(pt->Data, pt->DataSize, h2),
             pFlowControlMsg ? hex16(pFlowControlMsg->Data, pFlowControlMsg->DataSize, h3) : "-",
             pFlowControlMsg ? pFlowControlMsg->TxFlags : pt->TxFlags, (unsigned long)st, fid);
    if (st == STATUS_NOERROR && pFilterID) *pFilterID = fid;
    return st;
}

J2534_API PassThruStopMsgFilter(unsigned long ChannelID, unsigned long FilterID)
{
    if (!g_open || ChannelID<1 || ChannelID>J2534_MAX_CH) return ERR_INVALID_CHANNEL_ID;
    unsigned char p[4]; put_u32(p, FilterID);
    long st = transact(WW_STOP_FILTER, (unsigned)ChannelID, p, 4, NULL);
    log_line("PassThruStopMsgFilter: channel %lu filter %lu: status 0x%lX", ChannelID, FilterID, (unsigned long)st);
    return st;
}

J2534_API PassThruStartPeriodicMsg(unsigned long ChannelID,
                                   const PASSTHRU_MSG *pMsg,
                                   unsigned long *pMsgID,
                                   unsigned long TimeInterval)
{
    if (!g_open || ChannelID<1 || ChannelID>J2534_MAX_CH) return ERR_INVALID_CHANNEL_ID;
    if (!pMsg) return ERR_NULL_PARAMETER;
    if (pMsg->DataSize > WW_MAX_DATA) { set_err("message too long"); return ERR_INVALID_MSG; }
    static unsigned char buf[4 + 24 + WW_MAX_DATA];
    char hx[80];
    put_u32(buf, TimeInterval);
    int n = 4 + encode_msg(pMsg, buf + 4);
    unsigned long id = 0;
    long st = transact(WW_START_PERIODIC, (unsigned)ChannelID, buf, (unsigned)n, &id);
    log_line("PassThruStartPeriodicMsg: channel %lu every %lu ms size %lu [%s]: status 0x%lX id %lu",
             ChannelID, TimeInterval, pMsg->DataSize, hex16(pMsg->Data, pMsg->DataSize, hx),
             (unsigned long)st, id);
    if (st == STATUS_NOERROR && pMsgID) *pMsgID = id;
    return st;
}

J2534_API PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID)
{
    if (!g_open || ChannelID<1 || ChannelID>J2534_MAX_CH) return ERR_INVALID_CHANNEL_ID;
    unsigned char p[4]; put_u32(p, MsgID);
    long st = transact(WW_STOP_PERIODIC, (unsigned)ChannelID, p, 4, NULL);
    log_line("PassThruStopPeriodicMsg: channel %lu id %lu: status 0x%lX", ChannelID, MsgID, (unsigned long)st);
    return st;
}

/* GET_CONFIG, what the DLL answers itself: DATA_RATE is the connected
 * baud. ISO15765_BS and ISO15765_STMIN here are BS 8 / STmin 1 ms, the
 * flow control of a device that does not answer GET_CONFIG (every
 * firmware before 2026-11-17: it sends those whatever a tool sets). A
 * firmware that takes the two answers with the values in force, and
 * PassThruIoctl puts them in place of these (iso_flow_from_device). */
static unsigned long config_value(unsigned long channel, unsigned long param)
{
    switch (param) {
        case DATA_RATE:      return (channel >= 1 && channel <= J2534_MAX_CH) ? g_dev.ch_baud[channel-1] : 0;
        case LOOPBACK:       return 0;
        case ISO15765_BS:    return 8;
        case ISO15765_STMIN: return 1;
        default:             return 0;
    }
}

#define IOCTL_CFG_MAX 64

static int channel_is(unsigned long channel, unsigned long protocol)
{
    return channel >= 1 && channel <= J2534_MAX_CH
        && g_dev.ch_open[channel - 1]
        && g_dev.ch_proto[channel - 1] == protocol;
}

static int channel_is_tp20(unsigned long channel)
{
    return channel_is(channel, TP2_0_PS);
}

/* WW_IOCTL payload: u32 ioctl_id, u32 body_len, body[body_len].
 * An IOCTL without parameters (READ_VBATT, the CLEAR ones) is its id
 * alone: the 4 bytes every driver up to 1.7 sent. The device reads that
 * as a body of length 0.
 * The DLL does not interpret a TP2.0 telegram; the device does. */
static long ioctl_send(unsigned channel, unsigned long ioctl_id,
                       const unsigned char *body, unsigned body_len,
                       unsigned long *result)
{
    unsigned char buf[8 + 4 + IOCTL_CFG_MAX * 8];
    if (body_len > sizeof(buf) - 8) {
        set_err("ioctl body too long");
        return ERR_INVALID_MSG;
    }
    put_u32(buf, ioctl_id);
    if (body_len == 0 || body == NULL)
        return transact(WW_IOCTL, channel, buf, 4, result);
    put_u32(buf + 4, body_len);
    memcpy(buf + 8, body, body_len);
    return transact(WW_IOCTL, channel, buf, 8 + body_len, result);
}

/* SET_CONFIG / GET_CONFIG body: u32 count, then count pairs. GET_CONFIG's
 * ACK is the status word, then a count and the pairs with values.
 * answered NULL (a TP2.0 channel): a GET the device answers nothing to is
 * ERR_NOT_SUPPORTED. answered given (an ISO15765 channel): it receives the
 * number of values the device filled, and none is not an error, it is a
 * firmware that does not hold the parameters. */
static long ioctl_config(unsigned long channel, unsigned long ioctl_id,
                         const SCONFIG_LIST *l, unsigned *answered)
{
    unsigned long count = l->NumOfParams;
    unsigned char body[4 + IOCTL_CFG_MAX * 8];
    long st;
    if (answered) *answered = 0;
    if (count > IOCTL_CFG_MAX) {
        set_err("too many config parameters");
        return ERR_INVALID_MSG;
    }
    put_u32(body, count);
    for (unsigned long i = 0; i < count; i++) {
        put_u32(body + 4 + i * 8, l->ConfigPtr[i].Parameter);
        put_u32(body + 8 + i * 8, l->ConfigPtr[i].Value);
        if (ioctl_id == SET_CONFIG)
            log_line("PassThruIoctl: SET_CONFIG channel %lu param 0x%lX = %lu",
                     channel, l->ConfigPtr[i].Parameter, l->ConfigPtr[i].Value);
    }
    st = ioctl_send((unsigned)channel, ioctl_id, body,
                    4u + (unsigned)count * 8u, NULL);
    if (st != STATUS_NOERROR || ioctl_id != GET_CONFIG) {
        log_line("PassThruIoctl: channel %lu ioctl 0x%lX: status 0x%lX",
                 channel, ioctl_id, (unsigned long)st);
        return st;
    }
    if (count == 0) return STATUS_NOERROR;
    {
        unsigned n = g_dev.ack_body_len;
        const unsigned char *b = g_dev.ack_body;
        unsigned off = 4;
        unsigned filled = 0;
        unsigned long got;
        if (n < 4) {
            if (answered) return STATUS_NOERROR;
            set_err("WiCAN returned no result; update the device firmware");
            log_line("PassThruIoctl: GET_CONFIG channel %lu: no pairs in the ACK", channel);
            return ERR_NOT_SUPPORTED;
        }
        got = get_u32(b);
        for (unsigned long i = 0; i < got; i++) {
            unsigned long param, value;
            if (off + 8 > n) break;
            param = get_u32(b + off);
            value = get_u32(b + off + 4);
            off += 8;
            for (unsigned long j = 0; j < count; j++) {
                if (l->ConfigPtr[j].Parameter != param) continue;
                l->ConfigPtr[j].Value = value;
                filled++;
                log_line("PassThruIoctl: GET_CONFIG channel %lu param 0x%lX = %lu",
                         channel, param, value);
            }
        }
        if (answered) {
            *answered = filled;
            return STATUS_NOERROR;
        }
        if (!filled) {
            set_err("WiCAN returned no result; update the device firmware");
            log_line("PassThruIoctl: GET_CONFIG channel %lu: ACK count %lu matched nothing",
                     channel, got);
            return ERR_NOT_SUPPORTED;
        }
    }
    return STATUS_NOERROR;
}

/* The flow control an ISO15765 channel sends as a receiver lives in the
 * device: ISO15765_BS and ISO15765_STMIN are sent to it (SET_CONFIG) and
 * read back from it (GET_CONFIG). Only those two travel: the other
 * parameters of a list stay with the DLL, as before, so a long list never
 * runs into the device's limit of 24 pairs. */
static int is_iso_flow_param(unsigned long param)
{
    return param == ISO15765_BS || param == ISO15765_STMIN;
}

/* The pairs of l that name one of the two, in l's order. */
static unsigned long iso_flow_pick(const SCONFIG_LIST *l, SCONFIG *out, unsigned long cap)
{
    unsigned long n = 0;
    for (unsigned long i = 0; i < l->NumOfParams && n < cap; i++)
        if (is_iso_flow_param(l->ConfigPtr[i].Parameter)) out[n++] = l->ConfigPtr[i];
    return n;
}

/* GET_CONFIG: the two values in force on the device put over the DLL's
 * own answer. A device that answers nothing keeps the DLL's (BS 8 /
 * STmin 1 ms, what such a firmware sends). */
static long iso_flow_from_device(unsigned long channel, SCONFIG_LIST *l)
{
    SCONFIG ask[2] = { { ISO15765_BS, 0 }, { ISO15765_STMIN, 0 } };
    SCONFIG_LIST two = { 2, ask };
    unsigned answered = 0;
    long st = ioctl_config(channel, GET_CONFIG, &two, &answered);
    if (st != STATUS_NOERROR) return st;
    if (!answered) {
        log_line("PassThruIoctl: GET_CONFIG channel %lu: the device does not answer ISO15765_BS / ISO15765_STMIN "
                 "(a firmware before them): it sends BS 8 / STmin 1 ms", channel);
        return STATUS_NOERROR;
    }
    for (unsigned long i = 0; i < l->NumOfParams; i++)
        for (int k = 0; k < 2; k++)
            if (l->ConfigPtr[i].Parameter == ask[k].Parameter) l->ConfigPtr[i].Value = ask[k].Value;
    return STATUS_NOERROR;
}

/* The SBYTE_ARRAY is the caller's bytes. The wire body is u32 NumOfBytes,
 * then those bytes, so REQUEST_CONNECTION is u32 11 plus the 11 setup
 * bytes (body_len 15) and TEARDOWN_CONNECTION is u32 4 plus the RX id
 * (body_len 8). A wrong length is the device's ERR_INVALID_IOCTL_VALUE. */
static long ioctl_bytes(unsigned long channel, unsigned long ioctl_id,
                        const SBYTE_ARRAY *a)
{
    unsigned char body[4 + 256];
    unsigned n;
    long st;
    if (!a || (a->NumOfBytes && !a->BytePtr)) {
        set_err("ioctl requires an input buffer");
        return ERR_NULL_PARAMETER;
    }
    n = (unsigned)a->NumOfBytes;
    if (n > 256) {
        set_err("ioctl body too long");
        return ERR_INVALID_MSG;
    }
    put_u32(body, n);
    if (n) memcpy(body + 4, a->BytePtr, n);
    st = ioctl_send((unsigned)channel, ioctl_id, body, 4u + n, NULL);
    log_line("PassThruIoctl: channel %lu ioctl 0x%lX body %u: status 0x%lX",
             channel, ioctl_id, 4u + n, (unsigned long)st);
    return st;
}

/* GET_DEVICE_INFO (SAE J2534-2 section 25): the application's SPARAM list
 * goes to the device as u32 count plus (parameter, value) pairs and comes
 * back as u32 count plus (parameter, value, supported) triples. The device
 * answers DEV_INFO_MAX parameters per request, so a longer list goes in
 * slices. A parameter the device does not have comes back not supported
 * with its Value as the application set it, and the call is still
 * STATUS_NOERROR. */
#define DEV_INFO_MAX 16

static long ioctl_device_info(SPARAM_LIST *l)
{
    if (!l || (l->NumOfParams && !l->ParamPtr)) {
        set_err("GET_DEVICE_INFO requires an SPARAM_LIST in the output pointer");
        return ERR_NULL_PARAMETER;
    }
    for (unsigned long at = 0; at < l->NumOfParams; at += DEV_INFO_MAX) {
        SPARAM *p = &l->ParamPtr[at];
        unsigned long n = l->NumOfParams - at;
        unsigned char body[4 + DEV_INFO_MAX * 8];
        const unsigned char *b = g_dev.ack_body;
        long st;
        if (n > DEV_INFO_MAX) n = DEV_INFO_MAX;
        put_u32(body, n);
        for (unsigned long i = 0; i < n; i++) {
            put_u32(body + 4 + i * 8, p[i].Parameter);
            put_u32(body + 8 + i * 8, p[i].Value);
        }
        st = ioctl_send(0, GET_DEVICE_INFO, body, 4u + (unsigned)n * 8u, NULL);
        if (st != STATUS_NOERROR) {
            log_line("PassThruIoctl: GET_DEVICE_INFO: status 0x%lX", (unsigned long)st);
            return st;
        }
        if (g_dev.ack_body_len < 4u + n * 12u || get_u32(b) != n) {
            set_err("WiCAN returned no device information; update the device firmware");
            log_line("PassThruIoctl: GET_DEVICE_INFO: no parameter list in the ACK (%u bytes)",
                     g_dev.ack_body_len);
            return ERR_NOT_SUPPORTED;
        }
        for (unsigned long i = 0; i < n; i++) {
            unsigned long supported = get_u32(b + 12 + i * 12);
            if (get_u32(b + 4 + i * 12) != p[i].Parameter) {
                set_err("WiCAN answered another parameter list than the one asked");
                return ERR_FAILED;
            }
            if (supported) p[i].Value = get_u32(b + 8 + i * 12);
            p[i].Supported = supported ? 1 : 0;
            log_line("PassThruIoctl: GET_DEVICE_INFO param 0x%lX: value 0x%lX, %s",
                     p[i].Parameter, p[i].Value, supported ? "supported" : "not supported");
        }
    }
    return STATUS_NOERROR;
}

J2534_API PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID,
                        const void *pInput, void *pOutput)
{
    if (!g_open) return ERR_INVALID_DEVICE_ID;
    if (IoctlID == READ_VBATT)
    {
        /* SAE J2534-1 passes the device id here. Tools that pass one of
         * their channel ids instead were answered by every earlier version,
         * so an id in the channel range stays accepted: the reading is
         * device-wide. Anything else is refused. */
        if (ChannelID < 1 || ChannelID > J2534_MAX_CH)
        {
            set_err("READ_VBATT requires the device id returned by PassThruOpen");
            return ERR_INVALID_DEVICE_ID;
        }
        if (pOutput == NULL)
        {
            set_err("READ_VBATT requires an output pointer");
            return ERR_NULL_PARAMETER;
        }
        {
            unsigned long millivolts = 0;
            long st = ioctl_send(0, READ_VBATT, NULL, 0, &millivolts);
            if (st == STATUS_NOERROR)
            {
                *(unsigned long *)pOutput = millivolts;
                log_line("PassThruIoctl: READ_VBATT: %lu mV", millivolts);
            }
            else
            {
                if (st == ERR_FAILED)
                    set_err("WiCAN could not read the battery voltage");
                log_line("PassThruIoctl: READ_VBATT: status 0x%lX", (unsigned long)st);
            }
            return st;
        }
    }
    if (IoctlID == GET_DEVICE_INFO)
    {
        /* The device id, by the same acceptance rule as READ_VBATT. The
         * input pointer is NULL by the standard and is not read. */
        if (ChannelID < 1 || ChannelID > J2534_MAX_CH)
        {
            set_err("GET_DEVICE_INFO requires the device id returned by PassThruOpen");
            return ERR_INVALID_DEVICE_ID;
        }
        return ioctl_device_info((SPARAM_LIST *)pOutput);
    }
    if (IoctlID == GET_CONFIG) {
        const SCONFIG_LIST *l = (const SCONFIG_LIST*)pInput;
        if (!l || (l->NumOfParams && !l->ConfigPtr)) return ERR_NULL_PARAMETER;
        if (channel_is_tp20(ChannelID))
            return ioctl_config(ChannelID, GET_CONFIG, l, NULL);
        {
            int flow = 0;
            for (unsigned long i = 0; i < l->NumOfParams; i++) {
                l->ConfigPtr[i].Value = config_value(ChannelID, l->ConfigPtr[i].Parameter);
                flow |= is_iso_flow_param(l->ConfigPtr[i].Parameter);
            }
            if (flow && channel_is(ChannelID, ISO15765)) {
                long st = iso_flow_from_device(ChannelID, (SCONFIG_LIST *)l);
                if (st != STATUS_NOERROR) return st;
            }
            for (unsigned long i = 0; i < l->NumOfParams; i++)
                log_line("PassThruIoctl: GET_CONFIG channel %lu param 0x%lX = %lu",
                         ChannelID, l->ConfigPtr[i].Parameter, l->ConfigPtr[i].Value);
        }
        return STATUS_NOERROR;
    }
    if (IoctlID == SET_CONFIG && channel_is_tp20(ChannelID)) {
        const SCONFIG_LIST *l = (const SCONFIG_LIST*)pInput;
        if (!l || (l->NumOfParams && !l->ConfigPtr)) return ERR_NULL_PARAMETER;
        return ioctl_config(ChannelID, SET_CONFIG, l, NULL);
    }
    if (IoctlID == SET_CONFIG && pInput) {
        const SCONFIG_LIST *l = (const SCONFIG_LIST*)pInput;
        SCONFIG flow[IOCTL_CFG_MAX];
        unsigned long n = 0;
        if (l->ConfigPtr && channel_is(ChannelID, ISO15765))
            n = iso_flow_pick(l, flow, IOCTL_CFG_MAX);
        for (unsigned long i = 0; l->ConfigPtr && i < l->NumOfParams; i++)
            if (!n || !is_iso_flow_param(l->ConfigPtr[i].Parameter))
                log_line("PassThruIoctl: SET_CONFIG channel %lu param 0x%lX = %lu (device keeps its own bus timing)",
                         ChannelID, l->ConfigPtr[i].Parameter, l->ConfigPtr[i].Value);
        if (n) {
            /* a firmware before the two acknowledges and keeps BS 8 / STmin
             * 1 ms; GET_CONFIG says which it is */
            SCONFIG_LIST picked = { n, flow };
            return ioctl_config(ChannelID, SET_CONFIG, &picked, NULL);
        }
    }
    if (IoctlID == REQUEST_CONNECTION || IoctlID == TEARDOWN_CONNECTION) {
        if (ChannelID < 1 || ChannelID > J2534_MAX_CH) return ERR_INVALID_CHANNEL_ID;
        return ioctl_bytes(ChannelID, IoctlID, (const SBYTE_ARRAY *)pInput);
    }
    {
        long st = ioctl_send((unsigned)ChannelID, IoctlID, NULL, 0, NULL);
        log_line("PassThruIoctl: channel %lu ioctl 0x%lX: status 0x%lX", ChannelID, IoctlID, (unsigned long)st);
        return st;
    }
}

J2534_API PassThruSetProgrammingVoltage(unsigned long DeviceID,
                                        unsigned long Pin, unsigned long Voltage)
{ (void)DeviceID;(void)Pin;(void)Voltage; return ERR_NOT_SUPPORTED; }

J2534_API PassThruReadVersion(unsigned long DeviceID, char *pFirmware,
                              char *pDll, char *pApi)
{
    (void)DeviceID;
    if (pFirmware) strcpy(pFirmware, "WiCAN v6");
    if (pDll) strcpy(pDll, DLL_VERSION);
    if (pApi) strcpy(pApi, "04.04");
    return STATUS_NOERROR;
}

J2534_API PassThruGetLastError(char *pErrorDescription)
{
    if (!pErrorDescription) return ERR_NULL_PARAMETER;
    strcpy(pErrorDescription, g_last_error);
    return STATUS_NOERROR;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)h;(void)r;
    if (reason==DLL_PROCESS_DETACH) { if (g_open) PassThruClose(0); log_close(); }
    return TRUE;
}
