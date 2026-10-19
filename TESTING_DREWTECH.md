# Validating the WiCAN J2534 driver with the DrewTech J2534-1 tool

The **Drew Technologies "Tool for J2534-1 API"** (free) is a real PassThru
application: it loads any registered `wican_j2534.dll`, enumerates the
device, and lets you drive `PassThruOpen / ReadVersion / Connect /
StartMsgFilter / WriteMsgs / ReadMsgs` from a GUI. It's the fastest way to
confirm the driver behaves against third-party software rather than only
our own `j2534_test.exe`.

This guide is written for the **USB-device (CDC-NCM) transport** — WiCAN
plugged straight into the PC — which is the most reliable path. WiFi/AP and
USB-Ethernet work the same way; only the `Host` address changes.

Result of the 2026-07-08 bench run: **PASS** for the full API surface the
tool exercises — Open, ReadVersion (`WiCAN v6` / `WiCAN J2534 1.0` /
`04.04`), Connect (CAN and ISO15765), StartMsgFilter (FLOW_CONTROL), and
WriteMsgs (correct ISO-TP framing on the wire).

---

## 1. One-time setup on the PC

### 1a. Put the DLL somewhere stable
Copy `wican_j2534.dll` to a fixed path, e.g. `C:\Users\<you>\WiCAN\`. (A
user directory avoids needing admin just to place the file.)

### 1b. Register it — mind the 32-bit registry view
The DrewTech tool is a **32-bit** process, and so is our DLL. On 64-bit
Windows a 32-bit process reads registry keys under **`WOW6432Node`**, so
BOTH the device registration and the DLL's own `Host`/`Port` config must be
in the `WOW6432Node` view or the tool/DLL won't see them.

> **Gotcha we hit:** putting `Host` only under
> `HKLM\SOFTWARE\WiCAN\J2534` (the native 64-bit view) left the 32-bit DLL
> reading the redirected `WOW6432Node\WiCAN\J2534`, which was empty, so it
> fell back to the compiled default `192.168.80.1` and failed to connect.
> Put the Host key under `WOW6432Node` too.

Import this (`.reg`, double-click → accept UAC). Adjust the DLL path and
`Host` to your setup:

```reg
Windows Registry Editor Version 5.00

; 32-bit device registration (what DrewTech enumerates)
[HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\PassThruSupport.04.04\WiCAN]
"Name"="WiCAN"
"Vendor"="meatpi"
"FunctionLibrary"="C:\\Users\\<you>\\WiCAN\\wican_j2534.dll"
"CAN"=dword:00000001
"ISO15765"=dword:00000001

; 32-bit DLL runtime config — reach WiCAN over the USB-device NCM link
[HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\WiCAN\J2534]
"Host"="192.168.82.1"
"Port"=dword:00001a99
```

(`0x1a99` = 6809. Keep the native 64-bit copies in sync if you also run a
64-bit PassThru app.)

---

## 2. Prepare WiCAN and the transport

1. Put WiCAN in USB-device mode: `usb_host_manager` `role=device`,
   `device_class=ncm`, then reboot. Plug WiCAN's USB port into the PC.
   Windows binds the inbox `UsbNcm.sys` and WiCAN serves DHCP; the PC gets
   `192.168.82.2`, WiCAN is `192.168.82.1`. Confirm with
   `ping 192.168.82.1`.
2. Enable the J2534 server and (for a reflash test) open the flashing gate:
   `PUT /api/settings/j2534_server {"enabled":true,"allow_reflash":true}`
   then `POST /api/settings/submit` (reboots). Leave `allow_reflash` at its
   default `false` for read-only diagnostics — see the safety note below.
   The USB-device link counts as an allowed interface, so `allow_lan` can
   stay `false`.
3. Put an ECU (or the `pcan_reflash_ecu.py` simulator) on WiCAN's CAN bus
   so there's something to answer. For the sim:
   `python -u tools/testbench/pcan_reflash_ecu.py 1200 --scenario happy`
   (`-u` so its prints flush live). It listens req `0x7E0` → resp `0x7E8`.

---

## 3. Drive the GUI

Launch `DrewTech_J2534-1.exe`.

1. **Connect tab → Interface:** pick **WiCAN**. The panel shows the DLL
   path and `... loaded - OK`.
2. Click **Open**. → `PassThruOpen`. The tool auto-reads version; you
   should see:
   - Supported Protocols: `CAN, ISO15765`
   - Device Firmware Version: `WiCAN v6`
   - J2534 DLL Version: `WiCAN J2534 1.0`
   - SAE J2534 API Version: `04.04`
3. **Connect** panel → Protocol **ISO15765**, Baud `500000`, **Connect**.
   → `PassThruConnect`. Status bar: `PassThruConnect - OK`.
   *(A raw **CAN** channel also connects, but Flow Control filters are
   ISO15765-only — on CAN the Flow Control column is greyed, which is
   correct J2534 behaviour, not a bug.)*
4. **Filters tab**, row 0:
   - **Filter:** dropdown (far right) → **Flow Control**  ← the step people
     miss; setting the text fields alone does nothing until the filter
     *type* is armed.
   - Mask `FF FF FF FF`, Pattern `00 00 07 E8`, Flow Control `00 00 07 E0`
   - **Apply**. → `PassThruStartMsgFilter`. WiCAN logs
     `ISO15765 channel bound tx 7E0 rx 7E8`; this is what binds the ISO-TP
     addressing.
5. **Messages tab:** protocol ISO15765, Scratch Pad `00 00 07 E0 10 02`,
   **Send**. → `PassThruWriteMsgs` + `PassThruReadMsgs`. Expect
   `00 00 07 E8 50 02 ...` in the RX list, preceded by the TxDone
   indication (RxStatus 0x09, data `00 00 07 E0`). Try `00 00 07 E0 27 01`
   → `... 67 01` + 4-byte seed; `00 00 07 E0 27 02 <key>` → `... 67 02`.

The scratch pad is the raw PASSTHRU_MSG `Data` field: SAE J2534-1 puts the
4-byte CAN id in front of an ISO15765 payload, both ways, and WiCAN
follows that since 2026-10-19. (The 2026-07-08 run typed `10 02` without
the id and the firmware of that time happened to accept the bare payload;
that reading was wrong, and udsoncan and the OEM suites, which always send
the id, timed out against it: the field report of 2026-10-19.)

---

## 4. Watch WiCAN's side

- `GET /api/j2534` — `client_connected`, `channels`, `frames_rx/tx`,
  and the safety flags `allow_reflash` / `allow_lan`.
- `GET /api/logs/ring` — the ISO-TP bind line and per-frame
  `send start` / `tx frame` traces.
- `GET /api/can` — `tx`, `rx`, and error counters (`tx_errors`,
  `bus_errors`) to distinguish a WiCAN issue from a bus/ECU one.

---

## 5. Bench gotchas (all cost real time on 2026-07-08)

- **PID collision → Code 10.** If a previous MeatPi USB product's INF
  (`meatpi_can.inf`, which claims `VID_16D0&PID_1261` as a COM port) is
  installed, it out-ranks the inbox NCM class driver by hardware-id match
  and the device fails to start. WiCAN's device PID is **0x1262** to avoid
  this. One PID per USB function.
- **USB subnet must not be the AP subnet.** WiCAN's USB link is
  `192.168.82.1/24`; if it shared `192.168.80.x` with the SoftAP, lwIP
  routed the PC's DHCP OFFER out the AP netif and the PC never got a lease.
- **`autopid` cross-talk.** If an autopid group is polling, its OBD
  responses collide with UDS on `0x7E8`. Disable it first:
  `POST /api/autopid/group {"name":"default","enabled":false}` (runtime
  only — re-disable after any DUT reboot).
- **PCAN adapter wedges after force-kills.** Repeatedly `kill -9`-ing the
  Python ECU (without `bus.shutdown()`) leaves the PEAK driver's channel
  receive-dead: the ECU re-inits without error but never sees frames, and
  WiCAN transmits into silence (its own CAN stays healthy — `tx_errors:0`).
  Unplug/replug the PCAN USB adapter to clear it. Run the sim with `-u` so
  its `Reflash ECU on PCAN...` / `<req> -> <resp>` prints are visible.

---

## 6. Safety defaults (see the server settings)

The J2534 server ships **disabled**. When you enable it, two gates stay
closed by default:

- **`allow_reflash` (default false)** — the device rejects UDS
  memory-transfer services (RequestDownload/Upload, TransferData,
  TransferExit). A tool can read and diagnose, but cannot write ECU
  firmware. Open it only for a real reflash job.
- **`allow_lan` (default false)** - connections are accepted only on
  WiCAN's SoftAP and USB-device link (someone physically at the device).
  Connections arriving over WiFi-STA or a USB-Ethernet uplink (a shared
  "shop LAN") are refused, because the transport is unauthenticated. Open
  it only on a trusted network.

---

## 7. J1850 VPW (GM Class 2), driver 1.3

VPW runs on WiCAN's OBD chip, not the CAN controller, so validating it
needs a J1850 VPW node on the bus (a GM vehicle, or a VPW-capable ECU /
bench node), which the CAN-only ECU simulator cannot provide. In the
DrewTech tool:

1. **Connect** with Protocol `J1850VPW` (1); BaudRate and flags are
   ignored. The device runs the VPW init on the chip. CONNECT returns
   `ERR_FAILED` if the OBD chip is not ready.
2. **WriteMsgs** a J1850 message in `Data`: the 3-byte header (priority,
   target, source, e.g. `68 6A F1`) followed by the payload (e.g. `01 00`
   for mode 01 PID 00). No CAN-id prefix.
3. **ReadMsgs** returns each J1850 reply as `[3-byte header][payload]`
   (e.g. `.. .. .. 41 00 ..`), and a TxDone echo (RxStatus with
   `TX_MSG_TYPE | TX_INDICATION`) carrying the request header.
4. **StartMsgFilter** PASS over the header (mask `00 FF 00`, pattern
   `00 F0 00`) narrows replies to the tester address F0.

The firmware bench `tools/testbench/usb/j2534_vpw_bench.py` drives the same
sequence over the wire protocol and prints `J2534 VPW PASS` / `SKIP` /
`FAIL`. Programming a GM module runs at 10.4 kbit/s (4x speed is not
implemented) with `allow_reflash` on.
