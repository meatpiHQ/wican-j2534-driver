# WiCAN J2534 PassThru DLL

A Windows SAE J2534-1 (04.04) pass-through driver for WiCAN. It lets any
J2534 application (OEM diagnostic/reflash tools, OpenPort-style utilities)
drive a vehicle's CAN bus **through WiCAN**. The DLL translates the ~14
`PassThru*` calls into WiCAN's wire protocol (`wican_wire.h`) over one of
two transports to the firmware's `j2534_server`:

- **TCP**: WiFi (STA or the WiCAN AP), USB-Ethernet, USB device NCM/RNDIS.
- **Serial**: WiCAN as a USB CDC-ACM virtual COM port
  (`device_class=cdc`, VID 16D0 PID 1263), the Tactrix-style COM-port
  J2534 device. The DLL can auto-discover the port by USB VID/PID.

> **Use at your own risk.** The driver forwards whatever a J2534
> application sends to the vehicle, ECU reprogramming included, and
> nothing in between judges whether a message is safe. A failed,
> interrupted or wrong operation can permanently damage a control unit or
> leave a vehicle unable to start or drive. The driver comes with **no
> warranty**, and MeatPi Electronics and the contributors are **not
> liable** for any damage, loss, cost or injury that results from its
> use. Read [DISCLAIMER.txt](DISCLAIMER.txt) before you build, install or
> use it. Licence: [GPL-3.0](LICENSE), see
> [Licence and disclaimer](#licence-and-disclaimer).

## What it supports

- Protocols **CAN** and **ISO15765** (ISO-TP) on WiCAN's CAN controller
  (the diagnostics and UDS reflash that 2008+ vehicles use), and **J1850
  VPW** (GM Class 2) on WiCAN's OBD chip. J1850 PWM, ISO9141 and ISO14230
  return `ERR_NOT_SUPPORTED` (chip-wired on the hardware, not built yet).
- `PassThruOpen/Close/Connect/Disconnect`, `ReadMsgs/WriteMsgs`,
  `StartMsgFilter/StopMsgFilter` (incl. FLOW_CONTROL for ISO-TP),
  `StartPeriodicMsg/StopPeriodicMsg` (tester present), `Ioctl`
  (READ_VBATT, CLEAR_*), `ReadVersion`, `GetLastError`.
- `SetProgrammingVoltage` → `ERR_NOT_SUPPORTED` (no pin-control hardware).

Messages are copied verbatim both ways. An ISO15765 PASSTHRU_MSG carries the
4-byte CAN id in `Data[0..3]` and the UDS payload after it, in both
directions, as SAE J2534-1 says; the WiCAN firmware of 2026-10-20 or later
speaks that layout (older firmware took the bare payload, and every standard
tool timed out on its first request: the field report of 2026-10-19). A
J1850 VPW PASSTHRU_MSG carries the 3-byte header and the payload whole, with
no CAN-id prefix; the DLL passes it through the same way.

## Versions

- **1.7 (2026-11-14)**: the measured battery voltage, and the licence.
  `READ_VBATT` asks the device for its measured battery voltage in
  millivolts instead of returning a nominal 12,000 by itself. It needs the
  matching firmware support (WiCAN PRO 6.0.0-beta.10 or later). Older
  firmware with no result returns `ERR_NOT_SUPPORTED` ("WiCAN returned no
  result; update the device firmware"), never an invented value; an
  unavailable ADC reading returns the device's error. Failed calls leave
  the output untouched. A null output and an invalid device id are
  refused. The id is the device id of `PassThruOpen` (SAE J2534-1); an id
  in the channel range is answered as well, because tools that pass their
  channel id there were answered by every earlier version and the reading
  is device-wide. The driver is licensed under the GNU General Public
  License, version 3 from this version on (`LICENSE`, an SPDX header in
  every source file), and the installer opens with the disclaimer
  (`DISCLAIMER.txt`, "use at your own risk"): it goes on only when the
  user accepts it, and installs the disclaimer and the licence
  (`LICENSE.txt`) next to the DLL. A silent install
  (`/SILENT`, `/VERYSILENT`) shows no wizard pages, so no disclaimer
  page either; the two files are installed all the same. The DLL's
  behaviour is otherwise that of 1.6.
- **1.6 (2026-11-06)**: the write path no longer costs a device round trip
  per message. With `Timeout` 0, `PassThruWriteMsgs` queues the message
  and returns, as SAE J2534-1 says ("queue and return") and as the Tactrix
  Openport driver does; the queued messages are held for about a
  millisecond so that the writes that follow join them, and go to the
  WiCAN in ONE `WRITE_MSGS` request (up to the device's request cap, 4384
  bytes; any other call, a read above all, sends what is held first, so a
  write followed by a read goes out at once). Until now every message was
  its own request and the call waited for the WiCAN's ACK, so a tool that
  sends or acknowledges a transfer frame by frame (a VW TP 2.0 flasher: 37
  frames per 255-byte block, one ACK per 15 frames; the field report of
  2026-10-27 and 2026-11-05, "20 min on an Openport, 40 on the WiCAN")
  paid the link's round trip on every frame it wrote. Bench 2026-11-06, a
  2 MB TP 2.0 transfer projected from 40 blocks over the WiCAN's own WiFi
  hotspot: the write direction 18.8 min with 1.5's wait, 3.1 min now; the
  read direction is set by the link's round trip per block end, which the
  firmware of the same day trims (USB CDC 2.4 to 1.3 ms, the Openport's
  1.4). The WiCAN's answer to a queued write arrives on the reader thread;
  a refusal is logged and reported by the next `PassThruWriteMsgs` call,
  which then sends nothing (the conditions a write is refused for are the
  channel's, not the message's). A non-zero `Timeout` waits for the ACK as
  before, and the registry value `WriteWait` (DWORD 1) or the environment
  variable `WICAN_J2534_WRITE_WAIT=1` makes every write wait (the 1.5
  behaviour). One consequence to know: a tool's response timer (UDS P2,
  50 ms in udsoncan and in what most ECUs advertise) now starts when the
  write is queued, not after the link's round trip, so on a link slower
  than the tool's P2 a request that scraped through with 1.5 (the wait hid
  half the round trip from the timer) times out with 1.6. Bench
  2026-11-06: udsoncan over the Pi tunnel (46 ms per round trip) failed
  its 50 ms P2 with the default and passed with `WriteWait`; over the
  WiCAN's own hotspot (5 ms) it passed with the default. Such a link is
  too slow for such a tool either way; `WriteWait` is the knob when a tool
  cannot raise its P2. `timeBeginPeriod(1)` is in effect while the device
  is open (the join window's sleep would otherwise be a 15.6 ms timer
  tick). Pairs
  with the firmware of 2026-11-06, whose RX pump wakes on the bus frame
  instead of polling every 2 ms and coalesces a burst of received frames
  into one transport write; either side works with the other.
- **1.5 (2026-10-27)**: `PassThruGetLastError` explains a device refusal.
  Until now a status the WiCAN answered (ERR_FAILED, ERR_INVALID_BAUDRATE,
  ERR_NO_FLOW_CONTROL, ...) left the last-error text at "no error", so a
  tool's dialog said nothing useful (the field report of 2026-10-27 showed
  exactly that). Every non-zero device status now sets a sentence with the
  code and the WiCAN-side reason ("the WiCAN refused: ERR_FAILED (0x07): the
  CAN node is not transmitting (no bit rate proven on a silent bus ...)").
  Pairs with the 2026-10-27 firmware, which no longer gates ECU-flashing
  services: nothing a tool writes is refused for its content any more.
- **1.4 (2026-10-27)**: `PassThruReadMsgs` waits on an event the reader
  thread signals for every received message instead of polling the ring
  with `Sleep(1)`. `Sleep(1)` sleeps one Windows timer period: 15.6 ms on a
  PC where nothing raised the timer resolution, 1 to 2 ms where a browser
  did, so every frame a tool waited for could cost a timer tick. Bench
  2026-10-27 (a VW TP 2.0 flasher's pattern, one block-end ACK per 15
  frames): the ECU saw the tool's ACK 2.8 ms after its frame over USB-serial
  with the event, against up to 19 ms with the poll and a 16-message read.
  An empty read still answers `ERR_BUFFER_EMPTY`. The **driver log is on by
  default**: `C:\Users\Public\Documents\WiCAN\wican_j2534.log` (a folder
  every user can write, no admin; the installer adds a Start Menu shortcut
  to it), one line per call with every message's bytes in full in both
  directions and the device timestamp on reads, 16 MB then one rotation to
  `.1`; `WICAN_J2534_LOG` / the registry `LogFile` move it, `off` disables
  it (1.2's 16-byte excerpt and "off unless set" are gone: a field report
  should come with the log, not with a request to enable it).
- **1.3 (2026-10-19)**: accepts the **J1850 VPW** protocol (id 1, GM Class
  2), which the firmware runs on the OBD chip; the DLL only adds it to the
  `PassThruConnect` accept list and passes its `[3-byte header][payload]`
  messages through verbatim (no CAN-id prefix). No other behaviour changed.
- **1.2 (2026-10-19)**: the ACK wait is event-driven (the 10 ms sleep poll
  put a 10 to 16 ms floor under every message); a debug log
  (`WICAN_J2534_LOG=<file>` or the registry `LogFile`) records every call
  with its bytes and status, and every line also goes to `OutputDebugString`;
  an empty `PassThruReadMsgs` stays `ERR_BUFFER_EMPTY` whatever the `Timeout`
  (SAE J2534-1 names `ERR_TIMEOUT` there, but udsoncan 1.26's reader takes
  `ERR_TIMEOUT` as data and fails on empty payloads: bench-hit); `GET_CONFIG` fills the
  values the device runs with (DATA_RATE from CONNECT, LOOPBACK 0,
  ISO15765_BS 8, ISO15765_STMIN 1); requests are serialized across threads;
  the ACK wait is 8 s (the device's longest WRITE, a multi-frame send at a
  slow STmin, is capped at 5.5 s).
- **1.1 (2026-07-18)**: the USB CDC-ACM serial transport, the installer.
- **1.0 (2026-07-08)**: TCP, CAN + ISO15765, validated with the DrewTech tool.

## Build

32-bit is the safe default: most OEM J2534 apps load a **32-bit** DLL.

**MSVC**
```
cmake -A Win32 -B build
cmake --build build --config Release
```

**MinGW-w64**
```
cmake -G "MinGW Makefiles" -DCMAKE_C_FLAGS=-m32 -B build
cmake --build build
```

Output: `wican_j2534.dll`. On a PC with MSVC, `.\build.ps1` does the
configure and the build in one step and installs nothing.

To test a build before it is installed, set `WICAN_J2534_DLL` to the
built DLL's full path and run the driver gates from a checkout of the
firmware repository ([wican-fw](https://github.com/meatpiHQ/wican-fw),
branch `dev-v6`): `.\test.ps1 j2534vbat driver -SkipBenchCheck` checks the
DLL against a fake device on the PC alone; `.\test.ps1 j2534vbat` also
checks two real supply voltages and needs that repository's bench.

## Install

**End users: use the installer.** `installer\build_installer.ps1` builds
`wican_j2534_setup_<version>.exe` (Inno Setup 6
`winget install JRSoftware.InnoSetup`) from a built 32-bit DLL. The
setup.exe shows the disclaimer (`DISCLAIMER.txt`) and goes on only when
the user accepts it, copies the DLL to `Program Files (x86)\WiCAN` with
the disclaimer and the licence (`LICENSE.txt`), asks which
transport the user has (USB network / USB COM-port / SoftAP / custom IP),
registers both registry views, and ships the user manual as a PDF
(rendered at build time from USER_MANUAL.md via `installer\md2html.mjs`
+ Edge headless, needs node.exe and Edge on the build PC); re-running
it changes the transport, and uninstall removes everything. Manual steps
below remain the dev path.

1. Copy `wican_j2534.dll` somewhere stable, e.g. `C:\Program Files\WiCAN\`.
2. Edit `wican_j2534.reg`:
   - `FunctionLibrary` → the DLL's full path (both blocks).
   - `Host` / `Port` → how to reach WiCAN (see below).
3. Import it (admin): double-click, or `reg import wican_j2534.reg`.

The tool's device picker now lists **WiCAN**.

## Pointing the DLL at WiCAN

The DLL resolves the target in this order (strongest last wins):

1. env `WICAN_J2534_COM` = `COMn` or `auto` → **serial** (strongest)
2. env `WICAN_J2534_HOST` / `WICAN_J2534_PORT` → TCP (a `COMn` value
   selects serial instead)
3. registry `HKLM\SOFTWARE\WiCAN\J2534`:
   `Transport` (REG_SZ `tcp`|`serial`), `ComPort` (REG_SZ `COMn`, empty
   or `auto` = discover by USB VID 16D0 PID 1263), `Host` (REG_SZ; a
   `COMn` value selects serial), `Port` (DWORD)
4. default `192.168.80.1:6809` (the WiCAN SoftAP gateway)

Serial auto-discovery walks `HKLM\SYSTEM\CurrentControlSet\Enum\USB\`
for keys **starting with** `VID_16D0&PID_1263` (prefix match: CDC-ACM
is a composite device, so the COM child sits under `...&MI_00`), reads
their `PortName`s, and uses the first one that opens (a successful open
is the presence test; the SYSTEM hive is not WOW64-redirected).

Typical setups:
- **WiFi STA**: WiCAN on the shop LAN: set `Host` to its LAN IP.
- **WiCAN AP**: connect the PC to WiCAN's WiFi: use the AP gateway IP.
- **USB-Ethernet (host)**: WiCAN's wired-uplink IP.
- **USB-device NCM/RNDIS**: plug WiCAN into the PC
  (`usb_host_manager.role=device`, `device_class=ncm`); it appears as a
  network adapter ("MeatPi Electronics WiCAN Pro", inbox `UsbNcm.sys` on
  Win10/11) and serves DHCP on the link. Set `Host` to **`192.168.82.1`**
  (the USB-link address, the .80.1 default is the WiFi AP). Driver-tested
  end-to-end over the cable 2026-07-08.
- **USB-device CDC-ACM (serial)**: `device_class=cdc`: WiCAN enumerates as
  a virtual COM port (inbox `usbser.sys` → "USB Serial Device (COMn)",
  VID 16D0 PID 1263) carrying the same framed wire protocol over serial
  instead of TCP. The DLL opens it overlapped with DTR asserted (the
  firmware gates the serial session on DTR); set `Transport=serial` in
  the registry (or `WICAN_J2534_COM=auto`) and the port is found by
  VID/PID: no configuration at all. Firmware side bench-tested by
  `tools/testbench/j2534_serial_test.py`.

On the firmware side, enable the server:
`/api/settings/j2534_server` → `{"enabled":true,"port":6809}` (reboot).
One exposure gate defaults off: `allow_lan` (AP+USB only). The device
forwards every write since the firmware of 2026-10-27 (the old
`allow_reflash` content gate is gone; `/api/j2534` counts the reprogramming
services it saw as `reflash_seen`). See `TESTING_DREWTECH.md` §6.

## Test without a tool

`../testbench/j2534_bench.py <wican-ip> 6809 --reflash` drives the same
wire protocol end-to-end (a reflash preamble vs a simulated ECU). If that
passes, the DLL against the same device will too.

## Files

| file | purpose |
|---|---|
| `wican_j2534.c` | the driver implementation (winsock + reader thread) |
| `j2534.h` | SAE J2534 API types/prototypes |
| `wican_wire.h` | the WiCAN wire protocol (keep in sync with the firmware) |
| `wican_j2534.def` | DLL exports |
| `wican_j2534.reg` | registry template (PassThruSupport + DLL config) |
| `CMakeLists.txt` | build |
| `installer/` | end-user setup.exe (Inno Setup script + build_installer.ps1) |
| `LICENSE` | the licence: GNU General Public License, version 3 |
| `DISCLAIMER.txt` | the disclaimer and terms of use (the installer's accept page) |

> The wire protocol is the contract in the firmware's
> `components/j2534_server/J2534_WIRE_PROTOCOL.md`. Change one side, change
> both (and bump `WW_VERSION` when the bytes change).

## Licence and disclaimer

Copyright (C) 2026 MeatPi Electronics.

The driver is free software under the **GNU General Public License,
version 3** (GPL-3.0-only); the full text is in [LICENSE](LICENSE). In
practice:

- You may use, study, change and redistribute it.
- If you pass it on, changed or not, as source or as a built DLL or
  installer, you pass it on under the same licence and with its complete
  source code. A closed-source version is not allowed.
- Running the driver is not restricted: any J2534 application may load
  the DLL and call it.

**Use it at your own risk.** There is no warranty, and MeatPi Electronics
and the contributors are not liable for damage to a vehicle, a control
unit or anything else, or for any loss, cost or injury, that results from
its use. The complete terms are in [DISCLAIMER.txt](DISCLAIMER.txt): the
installer shows them and installs only when the user accepts them, and
its warranty and liability sections are additional terms of the licence
(GPL section 7), so keep the file with every copy you pass on.
