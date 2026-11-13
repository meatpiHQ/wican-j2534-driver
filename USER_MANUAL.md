# WiCAN J2534 PassThru — User Manual

The WiCAN PRO can act as an **SAE J2534-1 (04.04) PassThru interface**:
any Windows J2534 diagnostic or reflash application talks to a vehicle's
CAN bus **through the WiCAN**, over WiFi or USB. This manual covers
installing the driver, connecting the WiCAN to your PC, using it from a
diagnostic tool, the safety gates, and troubleshooting.

## Read this first: use at your own risk

> **Warning.** The driver forwards everything your J2534 application
> sends to the vehicle, ECU reprogramming included, and nothing in
> between judges whether a message is safe. A failed, interrupted or
> wrong operation can permanently damage a control unit, leave the
> vehicle unable to start or drive, or make it unsafe.

- The driver is free, open-source software (GNU GPL version 3) and comes
  with **no warranty**. It is not certified or approved by SAE, by any
  vehicle manufacturer or by any tool vendor.
- **You are responsible** for what you do with it. MeatPi Electronics and
  the contributors are **not liable** for any damage, loss, cost or
  injury that results from its use.
- When you program a control unit: use the USB cable rather than WiFi,
  keep a battery maintainer on the vehicle, stop the PC from sleeping,
  and never work on a vehicle that is being driven.

The complete terms are the disclaimer the installer asked you to accept:
`DISCLAIMER.txt` in the installation folder
(`C:\Program Files (x86)\WiCAN\`), next to the licence (`LICENSE.txt`).

---

## 1. What it is and what it supports

A J2534 "PassThru" driver is a standard Windows DLL that OEM diagnostic
tools (and reflash utilities) load to reach a vehicle. WiCAN provides
that DLL (`wican_j2534.dll`); it forwards the J2534 calls over the
network to the WiCAN firmware, which drives the CAN bus.

**Supported**
- Protocols: **CAN** and **ISO15765** (ISO-TP) on WiCAN's CAN controller
  (the diagnostics and UDS reflash used by 2008-and-later vehicles), and
  **J1850 VPW** (GM Class 2) on WiCAN's OBD chip (older GM vehicles). A
  J1850 message is its 3-byte header plus the payload, no CAN id; WiCAN
  segments large transfers itself. J1850 VPW is request/response only and
  runs at the standard 10.4 kbit/s (GM 4x programming speed is not
  implemented).
- The full PassThru call set a real tool uses: `PassThruOpen / Close /
  Connect / Disconnect`, `ReadMsgs / WriteMsgs`, `StartMsgFilter /
  StopMsgFilter` (including **FLOW_CONTROL** filters for ISO-TP),
  `StartPeriodicMsg / StopPeriodicMsg` (tester-present), `Ioctl`
  (`READ_VBATT`, the `CLEAR_*` buffers), `ReadVersion`, `GetLastError`.

Since driver 1.7, `READ_VBATT` reads the WiCAN's calibrated battery voltage
in millivolts. It requires matching firmware support (WiCAN PRO
6.0.0-beta.10 or later); old firmware without a voltage result returns
`ERR_NOT_SUPPORTED`. Driver 1.6 and earlier return a nominal 12.0 V
instead, so update both sides for a measured reading.

**Not supported**
- J1850 PWM, ISO9141 and ISO14230 (KWP2000 over K-line), SCI: these answer
  `ERR_NOT_SUPPORTED` (chip-wired on the hardware, not built yet).
- `SetProgrammingVoltage` (no pin-control hardware): `ERR_NOT_SUPPORTED`.

**Validated:** passed the free **DrewTech "Tool for J2534-1 API"**
conformance app over both USB and WiFi — Open, ReadVersion, Connect
(CAN + ISO15765), FLOW_CONTROL filters, and correctly-framed ISO-TP
WriteMsgs/ReadMsgs.

---

## 2. Requirements

- A WiCAN PRO on firmware v6.
- A Windows PC (10/11). Most OEM J2534 apps are **32-bit** and load a
  32-bit driver — the WiCAN DLL is 32-bit; keep that in mind for the
  registry (§5).
- A way to connect the PC to the WiCAN (WiFi or USB — §4).
- A vehicle (or an ECU, or an ECU simulator: §9) on the WiCAN CAN bus.
- Admin rights once, to import the driver registration.

---

## 3. Overview of the steps

1. **Firmware:** enable the J2534 server and pick the transport (§4);
   read §7 before you reflash.
2. **Connect** the PC to the WiCAN and note the WiCAN's IP address (§4).
3. **Install** the driver DLL and register it on Windows (§5).
4. **Use** it from your J2534 application (§6).

---

## 4. Firmware setup + connecting the PC

### 4a. Enable the J2534 server

The server is **off by default**. Enable it via the web UI (the **J2534**
screen under TOOLS) or the API:

```
PUT  /api/settings/j2534_server   {"enabled": true, "port": 6809}
POST /api/settings/submit          (the device reboots)
```

Default TCP port is **6809**. Once the server is enabled the WiCAN
forwards everything the tool sends, reprogramming included; one exposure
gate (`allow_lan`, default **false**) decides which networks may connect.
Both are covered in §7.

### 4b. Choose how the PC reaches the WiCAN

Pick **one** transport. The only thing that changes downstream is the
**Host IP** you give the driver.

| Transport | How | WiCAN IP to use |
|---|---|---|
| **WiCAN SoftAP** (simplest) | Join the WiCAN's own WiFi from the PC | **`192.168.80.1`** (AP gateway) |
| **WiFi station** (shop LAN) | WiCAN joins your network | WiCAN's LAN IP (from your router / the Status screen) |
| **USB-device (NCM)** — most reliable | Set `usb_host_manager.role=device`, `device_class=ncm`, reboot, plug USB into the PC | **`192.168.82.1`** (the USB link) |
| **USB-device (COM port / CDC)** | Set `role=device`, `device_class=cdc`, reboot, plug USB into the PC | *(no IP — a serial COM port, found automatically)* |
| **USB-Ethernet (host)** | A USB-Ethernet adapter on WiCAN's host port to your LAN | WiCAN's wired uplink IP |

**USB-device (NCM) is the recommended path.** After setting NCM mode and
plugging in, Windows binds its inbox `UsbNcm.sys` driver, the device
appears as a network adapter ("MeatPi Electronics WiCAN Pro"), and WiCAN
serves DHCP on the link — your PC gets `192.168.82.2`, WiCAN is
`192.168.82.1`. Confirm with `ping 192.168.82.1`.

> **Important:** the USB-device link is `192.168.82.1`, **not** the
> `192.168.80.1` AP default. Use the right IP for your transport.

---

## 5. Install the driver on Windows

### The easy way — the installer

Run **`wican_j2534_setup_<version>.exe`**: accept the UAC prompt, then
read the disclaimer and accept it (the setup goes no further without
that). It then does everything in this section for you:

1. Asks **how your PC is connected** to the WiCAN — USB cable in
   network mode (recommended), USB cable in COM-port mode, the WiCAN's
   own WiFi hotspot, or a custom IP for a WiCAN on your network — and
   configures the driver accordingly.
2. Copies the DLL to `C:\Program Files (x86)\WiCAN\`.
3. Registers the device in **both** registry views (32-bit *and* 64-bit
   tools find it).

To **change the IP later** (e.g. you switched from USB to WiFi), just
re-run the installer — it remembers your current setting. Uninstall via
Windows "Apps → Installed apps" removes the DLL and all registry keys.

If you used the installer, skip the rest of §5 (it documents what the
installer did, and the manual fallback).

### 5a. Place the DLL

Copy `wican_j2534.dll` to a stable path, e.g.
`C:\Program Files\WiCAN\wican_j2534.dll` (or a user folder like
`C:\Users\<you>\WiCAN\` to avoid needing admin just to place it).

> If you were given source instead of a built DLL, build 32-bit:
> `cmake -A Win32 -B build && cmake --build build --config Release`
> (MSVC), or MinGW `cmake -G "MinGW Makefiles" -DCMAKE_C_FLAGS=-m32 …`.

### 5b. Register it — **mind the 32-bit registry view**

This is the step that trips people up. A **32-bit** PassThru app (most of
them) reads the registry under **`WOW6432Node`** on 64-bit Windows. Both
the device registration *and* the DLL's Host/Port config must exist in
that view or the tool won't find the device / the DLL falls back to the
wrong IP.

Edit `wican_j2534.reg` (fix the DLL path and the `Host` for your
transport), then double-click it → accept the UAC prompt. A ready
32-bit-correct template:

```reg
Windows Registry Editor Version 5.00

; Device registration — what the tool enumerates (32-bit view)
[HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\PassThruSupport.04.04\WiCAN]
"Name"="WiCAN"
"Vendor"="meatpi"
"FunctionLibrary"="C:\\Program Files\\WiCAN\\wican_j2534.dll"
"CAN"=dword:00000001
"ISO15765"=dword:00000001

; DLL runtime config — where to reach WiCAN (32-bit view)
[HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\WiCAN\J2534]
"Host"="192.168.82.1"
"Port"=dword:00001a99
```

- `0x1a99` = **6809** (the default port). Change if you set a different
  `port`.
- Set `Host` to the IP from the §4 table for your transport.
- If you also run a **64-bit** J2534 app, add the same two blocks without
  `WOW6432Node` (the native view) and keep them in sync. The shipped
  `wican_j2534.reg` includes both views.

### 5c. How the DLL finds the WiCAN (resolution order)

1. Environment: `WICAN_J2534_COM` (`COM7`, or `auto` to search by USB
   id): forces the **serial** transport
2. Environment: `WICAN_J2534_HOST` / `WICAN_J2534_PORT` (a `COMn` value
   in `Host` selects serial too)
3. Registry: `HKLM\SOFTWARE\WiCAN\J2534` → `Transport` (`tcp`/`serial`),
   `ComPort` (`COMn` or `auto`), `Host`, `Port`
   (the `WOW6432Node` copy for 32-bit apps)
4. Compiled default: TCP `192.168.80.1:6809`

So you can override the target quickly with an environment variable
without touching the registry. In serial mode with `ComPort=auto` the
driver locates the WiCAN's COM port by its USB id (VID 16D0 PID 1263),
no port number to configure.

One more value, for the write path (driver 1.6): `WriteWait` (a DWORD
under the same registry key) or the environment variable
`WICAN_J2534_WRITE_WAIT`. With it set to 1, every `PassThruWriteMsgs`
waits for the WiCAN's answer before it returns, as driver 1.5 did. By
default a call with `Timeout` 0 queues its messages and returns (SAE
J2534-1: "queue and return"); the queued messages are held for about a
millisecond so that the writes that follow join them in one request to
the WiCAN (a read, or any other call, sends them first), the WiCAN's
answer arrives behind, and a refusal is reported by the next write call,
which then sends nothing. A tool that passes a non-zero `Timeout` always
waits.

---

## 6. Using it from a J2534 application

Any J2534 tool works the same way. Below is the flow using the free
**DrewTech "Tool for J2534-1 API"** as a concrete, verified example.

1. **Pick the interface.** In the tool's device/interface list, choose
   **WiCAN**. It shows the DLL path and `... loaded - OK`.
2. **Open.** → `PassThruOpen`. The tool auto-reads version; you should
   see:
   - Supported Protocols: `CAN, ISO15765`
   - Device Firmware Version: `WiCAN v6`
   - J2534 DLL Version: `WiCAN J2534 1.7`
   - SAE J2534 API Version: `04.04`
3. **Connect.** Protocol **ISO15765**, Baud **500000**, Connect. →
   `PassThruConnect`.
   *(A raw **CAN** channel also connects; Flow-Control filters are
   ISO15765-only, so on a CAN channel that column is greyed — correct
   J2534 behaviour.)*
4. **Add a Flow-Control filter** (this is what binds ISO-TP addressing):
   - Set the filter **type** to **Flow Control** (the dropdown people
     miss — setting the text fields alone does nothing until the type is
     armed).
   - Mask `FF FF FF FF`, Pattern `00 00 07 E8`, Flow Control
     `00 00 07 E0` (for a standard `0x7E0`→`0x7E8` ECU; use your ECU's
     request/response IDs).
   - Apply. → `PassThruStartMsgFilter`. WiCAN logs
     `ISO15765 channel bound tx 7E0 rx 7E8`.
5. **Send/receive.** Protocol ISO15765, send `00 00 07 E0 10 02` → expect
   `00 00 07 E8 50 02 …`; `00 00 07 E0 27 01` → `… 67 01` + seed; and so
   on. An ISO15765 message carries the CAN id in its first 4 data bytes,
   both ways, exactly as SAE J2534-1 lays it out (the DrewTech scratch
   pad is the raw `Data` field, so type the id yourself). After each
   transmit the RX list also shows a TxDone indication (RxStatus 0x09,
   the id alone) and, for a long answer, a START_OF_MESSAGE one (0x02).
   Firmware before 2026-10-19 expected the bare payload instead; every
   standard tool timed out against it.

That's the whole loop: **Open → Connect → Flow-Control filter →
Write/Read**. OEM reflash apps do the same, plus periodic tester-present
and the reflash services (nothing on the WiCAN holds them back: read §7).

---

## 7. What enabling the server means (read this before reflashing)

J2534 can reprogram ECUs. The WiCAN does not inspect what a tool sends:
once the server is enabled it forwards every write, reads and
reprogramming alike, like a cable, and counts the reprogramming services
it saw on the J2534 page (`reflash_seen` on `GET /api/j2534`). Enabling the
server is the consent; leave it off when no tool should drive the bus.

Firmware before 2026-10-27 had an extra **`allow_reflash`** switch that
refused the UDS reprogramming services (`0x34..0x37`) and, on raw CAN,
ordinary frames of tools with their own framing (VW TP 2.0): on such
firmware turn "Allow ECU flashing" on for flashing work, or update.

One exposure gate remains (default **false**):

- **`allow_lan`** — when **false**, the server accepts tester connections
  **only** on the WiCAN SoftAP and the USB-device link (someone
  physically on the AP or plugged into the cable). A connection arriving
  over **WiFi-station** or a **USB-Ethernet uplink** (a shared "shop LAN")
  is dropped, so the unauthenticated reflash channel isn't reachable
  across a network. Set `true` only if you understand that exposes the
  port to the whole LAN.

With the recommended **USB-device (NCM)** transport, `allow_lan` can stay
`false` — the USB link is an allowed interface.

---

## 8. Checking status

- **Device CLI:** the `j2534` command prints enabled/listening, whether a
  tester is connected, channel count, and rx/tx frame counters.
- **`GET /api/j2534`** — `client_connected`, `channels`, `frames_rx/tx`,
  `reflash_seen` (reprogramming services forwarded since boot) and the
  `allow_lan` flag.
- **`GET /api/can`** — `tx`, `rx`, `tx_errors`, `bus_errors`: use these
  to tell a WiCAN problem from a bus/ECU one (e.g. no ACK = wrong bitrate
  or nothing on the bus).
- **`GET /api/logs/ring`** — the `ISO15765 channel bound …` line and
  per-frame send/tx traces.

---

## 9. Test without a diagnostic tool

You can exercise the entire path (driver protocol → firmware → a
simulated ECU) before involving a real tool. The scripts are in the WiCAN
firmware repository (https://github.com/meatpiHQ/wican-fw, branch
`dev-v6`); from a checkout of it:

```
python tools/testbench/usb/j2534_bench.py <wican-ip> 6809 --reflash
```

If that passes, a real J2534 app against the same device will too. To
give the tool something to talk to, run that repository's ECU simulator
on the CAN bus (`tools/testbench/actors/pcan_reflash_ecu.py`, request
`0x7E0` / response `0x7E8`; it needs a PEAK PCAN-USB adapter on the bus).

---

## 10. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Tool doesn't list WiCAN, or "DLL not found" | The registration is in the wrong registry view. A 32-bit tool reads **`WOW6432Node`** — put both the `PassThruSupport.04.04\WiCAN` and `WiCAN\J2534` keys there (§5b). |
| Connects but times out / can't reach device | Wrong `Host` IP for your transport. USB-device = **`192.168.82.1`**, AP = `192.168.80.1`, STA/Ethernet = the WiCAN's LAN IP. `ping` it first. |
| DLL uses `192.168.80.1` no matter what | The 32-bit DLL is reading an empty `WOW6432Node\WiCAN\J2534` and falling back to the compiled default. Put `Host`/`Port` under `WOW6432Node` too (or set `WICAN_J2534_HOST`). |
| USB device won't start (Device Manager Code 10) | A previous MeatPi USB product's INF may claim the hardware id. WiCAN's device PID is **0x1262** to avoid this; remove the conflicting INF. One PID per USB function. |
| PC never gets a USB-link IP (no DHCP) | The USB subnet must not overlap the AP subnet. WiCAN's USB link is `192.168.82.1/24`, separate from the `192.168.80.x` AP. |
| Connection over the shop LAN is refused | `allow_lan` is `false` (the safe default) — WiFi-STA / USB-Ethernet connections are dropped. Use AP/USB-device, or set `allow_lan=true` if you accept the exposure. |
| Serial mode: "no WiCAN COM port found" | The WiCAN isn't enumerated as a COM port. Check `role=device` + `device_class=cdc` is set and the device rebooted; look for "USB Serial Device (COMn)" in Device Manager (VID 16D0 PID 1263). |
| Serial mode: COM port open failed | Another program has the port open (only one app at a time), or `ComPort` names the wrong port — set it to `auto`. |
| Reflash services rejected (`0x34/36/37` fail), or `PassThruWriteMsgs` answers `NotSupported (0x01)` now and then on a **raw CAN** channel with `GetLastError` "no error" | Firmware before 2026-10-27 had an ECU-flashing gate (`allow_reflash`, default off) that refused those services and, on raw CAN, read every frame as ISO-TP and refused ordinary frames of tools with their own framing (VW TP 2.0) whose byte at the service position was `0x34..0x37`. Update the firmware (the gate is gone, every write is forwarded) and the driver (1.5 explains every refusal in `GetLastError`); on old firmware turn "Allow ECU flashing" on for that work. |
| Data looks wrong / cross-talk on the bus | An `autopid` group polling the same bus can interleave with the tester. Disable autopid while running J2534. |
| Flow-Control column greyed out | You're on a **CAN** channel; flow control is ISO15765-only. Connect an ISO15765 channel for ISO-TP. |
| Every ISO15765 request times out although the car answers other tools | The WiCAN firmware predates 2026-10-20: it expected the bare UDS payload instead of the J2534-1 layout (the 4-byte CAN id first). Update the firmware; the driver is unchanged. |
| You need to see what the tool really sends | Open the driver log (Start Menu: "WiCAN J2534 driver log", or `C:\Users\Public\Documents\WiCAN\wican_j2534.log`): every call, every byte sent to the ECU and received from it, and the device's answer are in it (§10a). |

## 10a. Driver log

The driver keeps a log of everything that passes through it, on by default
since 1.4, so a problem can be shown instead of described:

- **Where:** `C:\Users\Public\Documents\WiCAN\wican_j2534.log` (the Start
  Menu entry "WiCAN J2534 driver log" opens the folder). Any user's tool can
  write there, no administrator rights needed. At 16 MB the file is renamed
  to `wican_j2534.log.1` and a fresh one starts, so the folder holds the
  last 16 to 32 MB: about an hour of a busy flash.
- **What:** one line per API call with the local time (ms) and the calling
  thread: `PassThruOpen` (transport, firmware and driver versions, the log
  path), `PassThruConnect` (protocol, flags, baud, status), every
  `PassThruWriteMsgs` message in full (`[00 00 07 40 B1]`: the 4-byte CAN id
  first, then the frame bytes, then the device's status), every message
  `PassThruReadMsgs` returned in full with its `rx_status` and the device's
  microsecond timestamp `ts`, every filter, periodic message and IOCTL with
  its status, and `reader: link closed` when the link dropped. The same
  lines go to `OutputDebugString` for DebugView.
- **Sending it:** reproduce the problem, then send the `.log` (and the `.1`
  if the problem happened earlier) with the WiCAN's firmware version. A
  refused write shows as `status 0x1` (NotSupported), `0x7` (failed) or
  `0x9` (timeout) on its own line, with the bytes that were refused.
- **Moving or silencing it:** set the environment variable
  `WICAN_J2534_LOG` to another file path before starting the tool, or the
  registry value `LogFile` under `HKLM\SOFTWARE\WiCAN\J2534` (and
  `WOW6432Node\WiCAN\J2534` for 32-bit tools); the value `off` disables the
  file (DebugView still gets the lines).

---

## 11. Reference

- **Ports/IPs:** TCP **6809**; USB-device `192.168.82.1`, AP
  `192.168.80.1`. USB-serial: COM port, VID `16D0` PID `1263`.
- **Version strings** (`PassThruReadVersion`): firmware `WiCAN v6`, DLL
  `WiCAN J2534 1.7`, API `04.04`.
- **Driver log:** on by default (1.4) at
  `C:\Users\Public\Documents\WiCAN\wican_j2534.log` (Start Menu: "WiCAN
  J2534 driver log"), one line per call with every message's bytes in full
  both ways; see "Driver log" below. The environment variable
  `WICAN_J2534_LOG` or the registry value `LogFile` under
  `HKLM\SOFTWARE\WiCAN\J2534` (`WOW6432Node` for 32-bit tools) move it,
  `off` disables it. The same lines go to `OutputDebugString` (DebugView)
  always.
- **Protocols:** CAN, ISO15765, J1850 VPW. **Baud:** 500000 typical for
  CAN (match the bus); J1850 VPW runs at 10.4 kbit/s.
- **Exports:** the 14 `PassThru*` functions (see `wican_j2534.def`).
- **Transports:** TCP over WiFi (AP or STA) / USB-Ethernet / USB-device
  NCM, and **USB-device CDC-ACM serial** (a virtual COM port,
  `device_class=cdc`, auto-discovered by the driver: the classic
  COM-port J2534 device).
- **Limitations:** CAN, ISO15765 and J1850 VPW only (§1); no
  `SetProgrammingVoltage` (returns `ERR_NOT_SUPPORTED`).
- **Source:** https://github.com/meatpiHQ/wican-j2534-driver
- **Licence:** GNU General Public License, version 3 (`LICENSE.txt` in
  the installation folder). The driver is used at your own risk: see
  "Read this first" at the top of this manual and `DISCLAIMER.txt`.

---

*Developer/protocol details (the wire format, building the DLL, the
DrewTech validation log) live in `README.md` and `TESTING_DREWTECH.md`
alongside this file.*
