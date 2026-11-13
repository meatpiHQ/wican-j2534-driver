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
 * j2534_test.c — a minimal J2534 conformance/pass-through test for the
 * WiCAN DLL. Loads wican_j2534.dll like a real PassThru app (LoadLibrary
 * + GetProcAddress), opens an ISO15765 channel, sets the flow-control
 * filter, and drives a UDS reflash preamble against whatever ECU is on
 * WiCAN's CAN bus (use pcan_reflash_ecu.py on the bench).
 *
 * Point the DLL at the device via WICAN_J2534_HOST / WICAN_J2534_PORT
 * (or the registry). Build: see README (mingw or MSVC).
 *
 *   set WICAN_J2534_HOST=127.0.0.1   (e.g. through an ssh -L tunnel)
 *   j2534_test.exe
 *
 * Expected final line: J2534 DRIVER TEST PASS
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "j2534.h"

typedef long (__stdcall *fOpen)(const void*, unsigned long*);
typedef long (__stdcall *fClose)(unsigned long);
typedef long (__stdcall *fConnect)(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long*);
typedef long (__stdcall *fDisc)(unsigned long);
typedef long (__stdcall *fRead)(unsigned long, PASSTHRU_MSG*, unsigned long*, unsigned long);
typedef long (__stdcall *fWrite)(unsigned long, PASSTHRU_MSG*, unsigned long*, unsigned long);
typedef long (__stdcall *fFilter)(unsigned long, unsigned long, const PASSTHRU_MSG*, const PASSTHRU_MSG*, const PASSTHRU_MSG*, unsigned long*);
typedef long (__stdcall *fErr)(char*);

static fOpen Open; static fClose Close; static fConnect Connect;
static fDisc Disconnect; static fRead ReadMsgs; static fWrite WriteMsgs;
static fFilter StartMsgFilter; static fErr GetLastError_;

static int fails = 0;
static void check(const char *name, int ok, const char *detail)
{
    printf("%s: %s%s%s\n", ok ? "PASS" : "FAIL", name,
           detail && detail[0] ? " " : "", detail ? detail : "");
    if (!ok) fails++;
}

static void set_id(PASSTHRU_MSG *m, unsigned long id)
{
    memset(m, 0, sizeof(*m));
    m->ProtocolID = ISO15765; m->DataSize = 4;
    m->Data[0]=(unsigned char)(id>>24); m->Data[1]=(unsigned char)(id>>16);
    m->Data[2]=(unsigned char)(id>>8);  m->Data[3]=(unsigned char)id;
}

/* WiCAN ISO15765 convention: PASSTHRU_MSG.Data = the UDS payload (the
 * CAN id comes from the channel's flow-control filter). */
static long uds(unsigned long ch, const unsigned char *req, int rlen,
                unsigned char *resp, int *resplen, unsigned long collect_ms)
{
    PASSTHRU_MSG m; memset(&m, 0, sizeof(m));
    m.ProtocolID = ISO15765; m.DataSize = rlen; memcpy(m.Data, req, rlen);
    unsigned long n = 1;
    long st = WriteMsgs(ch, &m, &n, 1000);
    if (st != STATUS_NOERROR) return st;

    /* drain RX for up to collect_ms; keep the last message */
    DWORD end = GetTickCount() + collect_ms;
    *resplen = 0;
    for (;;) {
        PASSTHRU_MSG r; unsigned long rn = 1;
        long rs = ReadMsgs(ch, &r, &rn, 200);
        if (rs == STATUS_NOERROR && rn == 1) {
            memcpy(resp, r.Data, r.DataSize); *resplen = (int)r.DataSize;
        }
        if (GetTickCount() >= end) break;
    }
    return STATUS_NOERROR;
}

int main(void)
{
    HMODULE dll = LoadLibraryA("wican_j2534.dll");
    if (!dll) { printf("J2534 DRIVER TEST FAIL: LoadLibrary (put wican_j2534.dll next to this exe)\n"); return 1; }
    Open=(fOpen)GetProcAddress(dll,"PassThruOpen");
    Close=(fClose)GetProcAddress(dll,"PassThruClose");
    Connect=(fConnect)GetProcAddress(dll,"PassThruConnect");
    Disconnect=(fDisc)GetProcAddress(dll,"PassThruDisconnect");
    ReadMsgs=(fRead)GetProcAddress(dll,"PassThruReadMsgs");
    WriteMsgs=(fWrite)GetProcAddress(dll,"PassThruWriteMsgs");
    StartMsgFilter=(fFilter)GetProcAddress(dll,"PassThruStartMsgFilter");
    GetLastError_=(fErr)GetProcAddress(dll,"PassThruGetLastError");
    if (!Open||!Connect||!WriteMsgs||!ReadMsgs||!StartMsgFilter) {
        printf("J2534 DRIVER TEST FAIL: missing exports\n"); return 1; }

    unsigned long dev = 0;
    long st = Open(NULL, &dev);
    if (st != STATUS_NOERROR) { char e[256]={0}; if(GetLastError_)GetLastError_(e);
        printf("J2534 DRIVER TEST FAIL: PassThruOpen=%ld (%s)\n", st, e); return 1; }
    check("PassThruOpen", 1, "");

    unsigned long ch = 0;
    st = Connect(dev, ISO15765, 0, 500000, &ch);
    check("PassThruConnect ISO15765", st == STATUS_NOERROR, "");

    /* flow-control filter: rx pattern 0x7E8, flow-control (tx) 0x7E0 */
    PASSTHRU_MSG mask, patt, flow; unsigned long fid = 0;
    set_id(&mask, 0xFFFFFFFF); set_id(&patt, 0x7E8); set_id(&flow, 0x7E0);
    st = StartMsgFilter(ch, FLOW_CONTROL_FILTER, &mask, &patt, &flow, &fid);
    check("StartMsgFilter FLOW_CONTROL", st == STATUS_NOERROR, "");

    unsigned char resp[4200]; int rl = 0; char hx[64];
    #define HEX2(b0,b1) (resp[0]==(b0)&&resp[1]==(b1))

    if (getenv("WICAN_J2534_ECUSIM")) {
        /* wican-ecu-sim-fw "realistic reprogramming" flow (matches the
         * sim's tools/testbench/pcan_reflash.py): fingerprint-gated,
         * validated 0x08000000 flash window, CRC-checked transfer.
         * Exercises multi-frame ISO-TP both directions + a 0x78 storm. */
        uds(ch, (unsigned char[]){0x11,0x01}, 2, resp, &rl, 800);
        check("reset 11 01 -> 51 01", rl>=2 && HEX2(0x51,0x01), "");

        uds(ch, (unsigned char[]){0x10,0x02}, 2, resp, &rl, 800);
        check("session 10 02 -> 50 02", rl>=2 && HEX2(0x50,0x02), "");

        uds(ch, (unsigned char[]){0x27,0x01}, 2, resp, &rl, 800);
        unsigned char key[4]={0,0,0,0}; int have_seed = (rl>=6 && HEX2(0x67,0x01));
        if (have_seed) for(int i=0;i<4;i++) key[i]=resp[2+i]^0xFF;
        check("seed 27 01 -> 67 01 + seed", have_seed, "");

        unsigned char k[6]={0x27,0x02,key[0],key[1],key[2],key[3]};
        uds(ch, k, 6, resp, &rl, 800);
        check("key 27 02 -> 67 02 (unlocked)", rl>=2 && HEX2(0x67,0x02), "");

        uds(ch, (unsigned char[]){0x2E,0xF1,0x5A,0x01}, 4, resp, &rl, 800);
        check("fingerprint 2E F15A -> 6E", rl>=3 && resp[0]==0x6E, "");

        uds(ch, (unsigned char[]){0x31,0x01,0xFF,0x00}, 4, resp, &rl, 4000);
        sprintf(hx, "final %02X", rl?resp[0]:0);
        check("erase 31 01 FF00 -> 71 (rides 0x78)", rl>=1 && resp[0]==0x71, hx);

        unsigned char fw[64]; unsigned long crc = 0xFFFFFFFFul;
        for (int i=0;i<64;i++) fw[i] = (unsigned char)((i*7+3)&0xFF);
        for (int i=0;i<64;i++) {            /* zlib crc32 */
            crc ^= fw[i];
            for (int b=0;b<8;b++) crc = (crc>>1) ^ (0xEDB88320ul & (0ul-(crc&1)));
        }
        crc ^= 0xFFFFFFFFul;

        unsigned char rd[11]={0x34,0x00,0x44, 0x08,0,0,0, 0,0,0,64};
        uds(ch, rd, 11, resp, &rl, 800);
        check("requestDownload 34 @08000000 -> 74", rl>=1 && resp[0]==0x74, "");

        unsigned char blk[66]={0x36,0x01};
        memcpy(blk+2, fw, 64);
        uds(ch, blk, 66, resp, &rl, 1200);   /* multi-frame ISO-TP TX */
        check("transferData 36 (64B MF) -> 76", rl>=2 && HEX2(0x76,0x01), "");

        uds(ch, (unsigned char[]){0x37}, 1, resp, &rl, 800);
        check("transferExit 37 -> 77", rl>=1 && resp[0]==0x77, "");

        unsigned char cm[8]={0x31,0x01,0x02,0x02,
            (unsigned char)(crc>>24),(unsigned char)(crc>>16),
            (unsigned char)(crc>>8),(unsigned char)crc};
        uds(ch, cm, 8, resp, &rl, 2500);
        sprintf(hx, "%02X %02X %02X %02X %02X", rl>0?resp[0]:0, rl>1?resp[1]:0,
                rl>2?resp[2]:0, rl>3?resp[3]:0, rl>4?resp[4]:0);
        check("checkMemory 31 0202 CRC -> 71", rl>=4 && resp[0]==0x71, hx);
    } else {
    uds(ch, (unsigned char[]){0x10,0x02}, 2, resp, &rl, 800);
    sprintf(hx, "%d bytes", rl);
    check("session 10 02 -> 50 02", rl>=2 && HEX2(0x50,0x02), hx);

    uds(ch, (unsigned char[]){0x27,0x01}, 2, resp, &rl, 800);
    unsigned char key[4]={0,0,0,0}; int have_seed = (rl>=6 && HEX2(0x67,0x01));
    if (have_seed) for(int i=0;i<4;i++) key[i]=resp[2+i]^0xFF;
    check("seed 27 01 -> 67 01 + seed", have_seed, "");

    unsigned char k[6]={0x27,0x02,key[0],key[1],key[2],key[3]};
    uds(ch, k, 6, resp, &rl, 800);
    check("key 27 02 -> 67 02 (unlocked)", rl>=2 && HEX2(0x67,0x02), "");

    uds(ch, (unsigned char[]){0x34,0x00,0x44,0,0,0x04,0x00}, 7, resp, &rl, 800);
    check("requestDownload 34 -> 74", rl>=1 && resp[0]==0x74, "");

    uds(ch, (unsigned char[]){0x31,0x01,0xFF,0x00}, 4, resp, &rl, 2500);
    sprintf(hx, "final %02X", rl?resp[0]:0);
    check("erase 31 01 FF00 -> 71 (rides 0x78)", rl>=1 && resp[0]==0x71, hx);
    }

    if (Disconnect) Disconnect(ch);
    Close(dev);
    FreeLibrary(dll);

    if (fails) { printf("J2534 DRIVER TEST FAIL: %d checks\n", fails); return 1; }
    printf("J2534 DRIVER TEST PASS\n");
    return 0;
}
