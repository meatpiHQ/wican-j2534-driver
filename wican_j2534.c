/*
 * wican_j2534.c — SAE J2534 PassThru DLL for WiCAN.
 *
 * Translates the J2534 API into the WiCAN wire protocol (wican_wire.h)
 * over one of two transports carrying the same framed protocol:
 *   - TCP: WiCAN reachable by IP — WiFi (STA or the device AP),
 *     USB-Ethernet, or USB device NCM/RNDIS.
 *   - Serial: WiCAN as a USB CDC-ACM virtual COM port
 *     (usb_host_manager.role=device, device_class=cdc; VID 16D0 PID 1263).
 *
 * Target resolution (priority):
 *   - env WICAN_J2534_COM = COMn | auto        -> serial
 *   - env WICAN_J2534_HOST / WICAN_J2534_PORT  -> TCP (COMn value -> serial)
 *   - registry HKLM\SOFTWARE\WiCAN\J2534 :
 *       Transport (REG_SZ "tcp"|"serial"), ComPort (REG_SZ, ""/"auto" =
 *       find by USB VID/PID), Host (REG_SZ; "COMn" -> serial), Port (DWORD)
 *   - default 192.168.80.1 : 6809  (WiCAN SoftAP gateway)
 *
 * RX model: WiCAN pushes RX_MSG frames; one reader thread owns the
 * transport read side and dispatches — ACK frames wake the pending
 * request, RX_MSG frames go to a per-channel ring that PassThruReadMsgs
 * drains (networked-J2534 pattern; identical over serial).
 *
 * Build: CMakeLists.txt (MSVC or MinGW). 32-bit is the safe default —
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "j2534.h"
#include "wican_wire.h"

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

#define J2534_MAX_CH  8
#define RX_RING       256
#define DLL_VERSION   "WiCAN J2534 1.1"

/* USB ids of the WiCAN CDC-ACM function (WICAN_USB_DEV_PID_CDC) — what
 * "auto" COM-port discovery matches in the Enum tree. Prefix, not exact:
 * CDC-ACM is a composite device, so the serial child enumerates under
 * VID_16D0&PID_1263&MI_00 (bench-hit 2026-07-18). */
#define WICAN_CDC_VIDPID "VID_16D0&PID_1263"

typedef struct {
    PASSTHRU_MSG msg[RX_RING];
    volatile int head, tail;
    CRITICAL_SECTION cs;
} rx_ring_t;

typedef struct {
    SOCKET sock;
    int    is_serial;
    HANDLE hcom;              /* CDC-ACM COM port (overlapped) */
    HANDLE rd_ev, wr_ev;      /* per-direction OVERLAPPED events */
    volatile int connected;
    unsigned seq;
    CRITICAL_SECTION tx_cs;

    HANDLE reader;
    volatile int reader_run;

    /* ACK handoff from the reader thread to transact() */
    volatile unsigned      await_seq;
    volatile int           ack_ready;
    volatile unsigned long ack_status;
    volatile unsigned long ack_result;

    rx_ring_t     ring[J2534_MAX_CH];
    unsigned long ch_proto[J2534_MAX_CH];
    int           ch_open[J2534_MAX_CH];
} device_t;

static device_t g_dev;
static int  g_open;
static char g_last_error[256] = "no error";

static void set_err(const char *m)
{ strncpy(g_last_error, m, sizeof(g_last_error) - 1); g_last_error[sizeof(g_last_error)-1] = 0; }

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
 * (prefix match — the CDC child sits under ...&MI_00) for PortName values
 * and return the first one that actually opens (Enum keys persist for
 * unplugged devices — a successful open is the presence test). The
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

static void ring_push(rx_ring_t *r, const PASSTHRU_MSG *m)
{
    EnterCriticalSection(&r->cs);
    int next = (r->head + 1) % RX_RING;
    if (next != r->tail) { r->msg[r->head] = *m; r->head = next; }
    /* else drop-oldest by discarding the new one (ring full) */
    LeaveCriticalSection(&r->cs);
}

static int ring_pop(rx_ring_t *r, PASSTHRU_MSG *out)
{
    int got = 0;
    EnterCriticalSection(&r->cs);
    if (r->tail != r->head) { *out = r->msg[r->tail]; r->tail = (r->tail+1)%RX_RING; got = 1; }
    LeaveCriticalSection(&r->cs);
    return got;
}

static DWORD WINAPI reader_thread(LPVOID arg)
{
    (void)arg;
    unsigned char hdr[WW_HDR_SIZE];
    static unsigned char payload[WW_MAX_DATA + 64];

    while (g_dev.reader_run) {
        if (!io_read_all(hdr, WW_HDR_SIZE)) break;
        if (get_u16(hdr) != WW_MAGIC) break;
        unsigned type = hdr[3], seq = get_u16(hdr+4), ch = get_u16(hdr+6);
        unsigned long len = get_u32(hdr+8);
        if (len > sizeof(payload)) break;
        if (len && !io_read_all(payload, (int)len)) break;

        if (type == WW_ACK) {
            g_dev.ack_status = (len>=4) ? get_u32(payload) : ERR_FAILED;
            g_dev.ack_result = (len>=8) ? get_u32(payload+4) : 0;
            if (seq == g_dev.await_seq) g_dev.ack_ready = 1;
        } else if (type == WW_RX_MSG && ch >= 1 && ch <= J2534_MAX_CH) {
            PASSTHRU_MSG m; decode_msg(payload, (int)len, &m);
            ring_push(&g_dev.ring[ch-1], &m);
        }
        /* WW_EVENT: ignored for now */
    }
    g_dev.connected = 0;
    return 0;
}

/* send a request, wait for its ACK. Returns J2534 status; *result set. */
static long transact(unsigned char type, unsigned channel,
                     const unsigned char *payload, unsigned len,
                     unsigned long *result)
{
    unsigned char hdr[WW_HDR_SIZE];
    if (!g_dev.connected) { set_err("not connected"); return ERR_DEVICE_NOT_CONNECTED; }

    EnterCriticalSection(&g_dev.tx_cs);
    g_dev.seq = (g_dev.seq + 1) & 0xFFFF;
    unsigned myseq = g_dev.seq;
    put_u16(hdr+0, WW_MAGIC); hdr[2]=WW_VERSION; hdr[3]=type;
    put_u16(hdr+4, myseq); put_u16(hdr+6, channel); put_u32(hdr+8, len);
    g_dev.await_seq = myseq; g_dev.ack_ready = 0;
    int ok = io_write_all(hdr, WW_HDR_SIZE) &&
             (!len || io_write_all(payload, len));
    LeaveCriticalSection(&g_dev.tx_cs);

    if (!ok) { set_err("device write failed"); return ERR_DEVICE_NOT_CONNECTED; }

    for (int i = 0; i < 600 && !g_dev.ack_ready && g_dev.connected; i++) Sleep(10);
    if (!g_dev.ack_ready) { set_err("device ACK timeout"); return ERR_TIMEOUT; }

    if (result) *result = g_dev.ack_result;
    return (long)g_dev.ack_status;
}

/* ---- connection setup ------------------------------------------------- */

typedef struct {
    int  serial;      /* 1 = CDC-ACM COM port, 0 = TCP */
    char com[16];     /* "COM7"; "" = auto-discover by USB VID/PID */
    char host[128];
    unsigned short port;
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
        RegCloseKey(k);
    }
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
    if (_stricmp(t->com, "auto") == 0) t->com[0] = 0;
}

/* Stop the reader and close whichever transport is up. TCP: close the
 * socket first to unblock recv(). Serial: the reader's ReadFile returns
 * within its 100 ms timeout, so wait for the thread BEFORE closing the
 * handle it reads from; drop DTR so the firmware ends the session. */
static void transport_teardown(void)
{
    g_dev.reader_run = 0; g_dev.connected = 0;
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
}

static void locks_delete(void)
{
    DeleteCriticalSection(&g_dev.tx_cs);
    for (int i=0;i<J2534_MAX_CH;i++) DeleteCriticalSection(&g_dev.ring[i].cs);
}

/* ---- J2534 API -------------------------------------------------------- */

J2534_API PassThruOpen(const void *pName, unsigned long *pDeviceID)
{
    (void)pName;
    if (g_open) { set_err("already open"); return ERR_DEVICE_IN_USE; }

    target_t tgt;
    resolve_target(&tgt);
    memset(&g_dev, 0, sizeof(g_dev));

    if (tgt.serial) {
        HANDLE h = tgt.com[0] ? com_open(tgt.com)
                              : com_autodiscover(tgt.com, sizeof(tgt.com));
        if (h == INVALID_HANDLE_VALUE) {
            set_err(tgt.com[0]
                ? "COM port open failed (in use, or wrong port?)"
                : "no WiCAN COM port found (VID 16D0 PID 1263 - is the WiCAN plugged in and in device/cdc mode?)");
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
        if (getaddrinfo(tgt.host, pstr, &hints, &res) != 0) { set_err("resolve failed"); WSACleanup(); return ERR_DEVICE_NOT_CONNECTED; }

        SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s == INVALID_SOCKET || connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
            freeaddrinfo(res); if (s!=INVALID_SOCKET) closesocket(s);
            set_err("connect failed"); WSACleanup(); return ERR_DEVICE_NOT_CONNECTED;
        }
        freeaddrinfo(res);
        int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof(one));
        g_dev.sock = s;
    }

    g_dev.connected = 1;
    InitializeCriticalSection(&g_dev.tx_cs);
    for (int i=0;i<J2534_MAX_CH;i++) InitializeCriticalSection(&g_dev.ring[i].cs);
    g_dev.reader_run = 1;
    g_dev.reader = CreateThread(NULL, 0, reader_thread, NULL, 0, NULL);

    unsigned long result = 0;
    unsigned char p[4]; put_u32(p, WW_VERSION);
    if (transact(WW_HELLO, 0, p, 4, &result) != STATUS_NOERROR) {
        transport_teardown(); locks_delete();
        set_err("HELLO failed"); return ERR_FAILED;
    }
    unsigned long dev = 0;
    if (transact(WW_OPEN, 0, NULL, 0, &dev) != STATUS_NOERROR) {
        transport_teardown(); locks_delete();
        set_err("OPEN failed"); return ERR_FAILED;
    }

    g_open = 1;
    if (pDeviceID) *pDeviceID = dev ? dev : 1;
    set_err("no error");
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
    return STATUS_NOERROR;
}

J2534_API PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID,
                          unsigned long Flags, unsigned long BaudRate,
                          unsigned long *pChannelID)
{
    (void)DeviceID;
    if (!g_open) return ERR_INVALID_DEVICE_ID;
    if (ProtocolID != CAN && ProtocolID != ISO15765) return ERR_INVALID_PROTOCOL_ID;

    unsigned char p[20];
    put_u32(p+0, ProtocolID); put_u32(p+4, Flags); put_u32(p+8, BaudRate);
    put_u32(p+12, 0); put_u32(p+16, 0);           /* tx/rx via FLOW_CONTROL filter */
    unsigned long chid = 0;
    long st = transact(WW_CONNECT, 0, p, sizeof(p), &chid);
    if (st != STATUS_NOERROR) return st;
    if (chid < 1 || chid > J2534_MAX_CH) return ERR_FAILED;
    g_dev.ch_proto[chid-1] = ProtocolID; g_dev.ch_open[chid-1] = 1;
    if (pChannelID) *pChannelID = chid;
    return STATUS_NOERROR;
}

J2534_API PassThruDisconnect(unsigned long ChannelID)
{
    if (!g_open || ChannelID<1 || ChannelID>J2534_MAX_CH) return ERR_INVALID_CHANNEL_ID;
    long st = transact(WW_DISCONNECT, (unsigned)ChannelID, NULL, 0, NULL);
    g_dev.ch_open[ChannelID-1] = 0;
    return st;
}

J2534_API PassThruReadMsgs(unsigned long ChannelID, PASSTHRU_MSG *pMsg,
                           unsigned long *pNumMsgs, unsigned long Timeout)
{
    if (!g_open || ChannelID<1 || ChannelID>J2534_MAX_CH) return ERR_INVALID_CHANNEL_ID;
    if (!pMsg || !pNumMsgs) return ERR_NULL_PARAMETER;
    unsigned long want = *pNumMsgs, got = 0;
    rx_ring_t *r = &g_dev.ring[ChannelID-1];
    DWORD deadline = GetTickCount() + Timeout;
    while (got < want) {
        if (ring_pop(r, &pMsg[got])) { got++; continue; }
        if (Timeout == 0 || GetTickCount() >= deadline) break;
        Sleep(2);
    }
    *pNumMsgs = got;
    return got ? STATUS_NOERROR : ERR_BUFFER_EMPTY;
}

J2534_API PassThruWriteMsgs(unsigned long ChannelID, PASSTHRU_MSG *pMsg,
                            unsigned long *pNumMsgs, unsigned long Timeout)
{
    (void)Timeout;
    if (!g_open || ChannelID<1 || ChannelID>J2534_MAX_CH) return ERR_INVALID_CHANNEL_ID;
    if (!pMsg || !pNumMsgs) return ERR_NULL_PARAMETER;
    static unsigned char buf[4 + 24 + WW_MAX_DATA];
    unsigned long sent = 0;
    for (unsigned long i = 0; i < *pNumMsgs; i++) {
        put_u32(buf, 1);
        int n = 4 + encode_msg(&pMsg[i], buf + 4);
        long st = transact(WW_WRITE_MSGS, (unsigned)ChannelID, buf, (unsigned)n, NULL);
        if (st != STATUS_NOERROR) { *pNumMsgs = sent; return st; }
        sent++;
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
    put_u32(p, FilterType); p += 4;
    PASSTHRU_MSG empty; memset(&empty, 0, sizeof(empty));
    p += encode_msg(pMaskMsg ? pMaskMsg : &empty, p);
    p += encode_msg(pPatternMsg ? pPatternMsg : &empty, p);
    if (pFlowControlMsg) p += encode_msg(pFlowControlMsg, p);
    unsigned long fid = 0;
    long st = transact(WW_START_FILTER, (unsigned)ChannelID, buf,
                       (unsigned)(p - buf), &fid);
    if (st == STATUS_NOERROR && pFilterID) *pFilterID = fid;
    return st;
}

J2534_API PassThruStopMsgFilter(unsigned long ChannelID, unsigned long FilterID)
{
    if (!g_open || ChannelID<1 || ChannelID>J2534_MAX_CH) return ERR_INVALID_CHANNEL_ID;
    unsigned char p[4]; put_u32(p, FilterID);
    return transact(WW_STOP_FILTER, (unsigned)ChannelID, p, 4, NULL);
}

J2534_API PassThruStartPeriodicMsg(unsigned long ChannelID,
                                   const PASSTHRU_MSG *pMsg,
                                   unsigned long *pMsgID,
                                   unsigned long TimeInterval)
{
    if (!g_open || ChannelID<1 || ChannelID>J2534_MAX_CH) return ERR_INVALID_CHANNEL_ID;
    if (!pMsg) return ERR_NULL_PARAMETER;
    static unsigned char buf[4 + 24 + WW_MAX_DATA];
    put_u32(buf, TimeInterval);
    int n = 4 + encode_msg(pMsg, buf + 4);
    unsigned long id = 0;
    long st = transact(WW_START_PERIODIC, (unsigned)ChannelID, buf, (unsigned)n, &id);
    if (st == STATUS_NOERROR && pMsgID) *pMsgID = id;
    return st;
}

J2534_API PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID)
{
    if (!g_open || ChannelID<1 || ChannelID>J2534_MAX_CH) return ERR_INVALID_CHANNEL_ID;
    unsigned char p[4]; put_u32(p, MsgID);
    return transact(WW_STOP_PERIODIC, (unsigned)ChannelID, p, 4, NULL);
}

J2534_API PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID,
                        const void *pInput, void *pOutput)
{
    if (!g_open) return ERR_INVALID_DEVICE_ID;
    if (IoctlID == READ_VBATT) { if (pOutput) *(unsigned long*)pOutput = 12000; return STATUS_NOERROR; }
    (void)pInput;
    unsigned char p[4]; put_u32(p, IoctlID);
    return transact(WW_IOCTL, (unsigned)ChannelID, p, 4, NULL);
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
{ (void)h;(void)r; if (reason==DLL_PROCESS_DETACH && g_open) PassThruClose(0); return TRUE; }
