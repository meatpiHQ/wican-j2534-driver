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

**Download:** the built installer (`wican_j2534_setup_<version>.exe`, with
its SHA-256 and the install steps) is in the WiCAN firmware repository,
[wican-fw `drivers/j2534`](https://github.com/meatpiHQ/wican-fw/tree/dev-v6/drivers/j2534),
and is attached to each firmware release. This repository holds the source
that installer is built from. End users: read [USER_MANUAL.md](USER_MANUAL.md).

## What it supports

- Protocols **CAN** and **ISO15765** (ISO-TP) on WiCAN's CAN controller
  (the diagnostics and UDS reflash that 2008+ vehicles use), and **J1850
  VPW** (GM Class 2) on WiCAN's OBD chip. J1850 PWM, ISO9141 and ISO14230
  return `ERR_NOT_SUPPORTED` (chip-wired on the hardware, not built yet).
  Since 1.8: **TP2_0_PS** (VW TP 2.0 as its own protocol, SAE J2534-2
  section 19); the transport runs on the device.
- `PassThruOpen/Close/Connect/Disconnect`, `ReadMsgs/WriteMsgs`,
  `StartMsgFilter/StopMsgFilter` (incl. FLOW_CONTROL for ISO-TP),
  `StartPeriodicMsg/StopPeriodicMsg` (tester present), `Ioctl`
  (READ_VBATT, CLEAR_*, GET_DEVICE_INFO, on a TP 2.0 channel
  SET_CONFIG, GET_CONFIG, REQUEST_CONNECTION, TEARDOWN_CONNECTION, and
  since 1.9 on an ISO15765 channel SET_CONFIG and GET_CONFIG for
  `ISO15765_BS` and `ISO15765_STMIN`), `ReadVersion`, `GetLastError`.
  The return codes carry SAE J2534-1's numbers since 1.10, and
  `ReadMsgs` reports lost messages with `ERR_BUFFER_OVERFLOW`.
- `SetProgrammingVoltage` → `ERR_NOT_SUPPORTED` (no pin-control hardware).

Messages are copied verbatim both ways. An ISO15765 PASSTHRU_MSG carries the
4-byte CAN id in `Data[0..3]` and the UDS payload after it, in both
directions, as SAE J2534-1 says; the WiCAN firmware of 2026-10-20 or later
speaks that layout (older firmware took the bare payload, and every standard
tool timed out on its first request: the field report of 2026-10-19). A
J1850 VPW PASSTHRU_MSG carries the 3-byte header and the payload whole, with
no CAN-id prefix; the DLL passes it through the same way.

ISO 15765 extended addressing (`ISO15765_ADDR_TYPE`, for an ECU behind a
gateway that answers for several ECUs on one pair of ids) is the device's
too: the DLL passes the flag, the fifth byte of a message and the 5-byte
mask, pattern and flow-control message of a FLOW_CONTROL filter through
as they are. WiCAN firmware from 6.0.0-beta.13 serves it; older firmware
answers the flag `ERR_INVALID_MSG`. Driver 1.10 needs no change for it
(the firmware's `.\test.ps1 j2534ext` calls the DLL like an application).

## Versions

- **Not released yet (after 1.10)**: two texts of `PassThruGetLastError`
  only, no change to a call. `ERR_INVALID_MSG` no longer says extended
  addressing is not supported (firmware from 6.0.0-beta.13 serves it),
  and `ERR_EXCEEDED_LIMIT` names the limits of both firmware generations
  (10 filters; 10 ISO-TP sessions on a channel, 4 before 6.0.0-beta.13).
  The DLL still calls itself 1.10.

- **1.10 (2026-11-18)**: the return codes are SAE J2534-1's numbers, and
  lost messages are reported. Up to 1.9 seven return codes had the wrong
  number, in this driver's header and in the WiCAN firmware alike:

  | Return code | up to 1.9 | 1.10, SAE J2534-1 |
  |---|---|---|
  | `ERR_DEVICE_IN_USE` | 0x1A | 0x0E |
  | `ERR_MSG_PROTOCOL_ID` | 0x14 | 0x15 |
  | `ERR_INVALID_FILTER_ID` | 0x15 | 0x16 |
  | `ERR_NO_FLOW_CONTROL` | 0x16 | 0x17 |
  | `ERR_NOT_UNIQUE` | 0x17 | 0x18 |
  | `ERR_INVALID_BAUDRATE` | 0x18 | 0x19 |
  | `ERR_INVALID_DEVICE_ID` | 0x19 | 0x1A |

  An application that follows the standard read "the bus runs at another
  bit rate" as `ERR_NOT_UNIQUE` and "no such device id" as
  `ERR_INVALID_BAUDRATE`. Only refusals were affected, and the text of
  `PassThruGetLastError` was right all along. From 1.10 the application
  gets the standard's numbers **with any firmware**: the driver asks the
  WiCAN for wire version 2 in its `HELLO`, where the device answers the
  standard's numbers itself; a firmware from before (it answers version
  1) still sends the old ones and the driver turns them into the
  standard's. A program that was written against this driver's old
  numbers (one that compares a return value with 0x18, say) must follow.
  Firmware with wire version 2 answers a driver up to 1.9 with the old
  numbers, so an old driver on new firmware behaves as it always did.

  `PassThruReadMsgs` returns `ERR_BUFFER_OVERFLOW` (0x12) when received
  messages were lost, as SAE J2534-1 asks. The call still hands messages
  over (`pNumMsgs` counts them, they are valid); the first of them is the
  first message received after the loss, so the gap is always in front of
  what that call returned (a read that is filling up stops in front of
  such a message and the next call returns it). Two places lose messages.
  The WiCAN's own receive queue of a channel (256 bus frames) runs over
  when the driver's link cannot keep up with the bus: firmware with wire
  version 2 then says how many are missing and where (an `EVENT` frame,
  `J2534_WIRE_PROTOCOL.md` section 13; older firmware cannot say). The
  driver's own queue (255 messages a channel) runs over when the
  application does not call `PassThruReadMsgs`: that is reported with any
  firmware. Until 1.9 both were silent. The driver log has a line for
  each loss with the count. The `BUFFER_OVERFLOW` bit that API 05.00 has
  in `RxStatus` is not set: this driver speaks API 04.04, which has no
  such bit. `PassThruOpen` on a WiCAN that says another tester holds it
  (over USB serial or Bluetooth) returns `ERR_DEVICE_IN_USE` with the
  last-error text, where it returned `ERR_FAILED` "HELLO failed". Checked
  against fake devices of both wire versions and against the WiCAN with
  both firmware generations (`.\test.ps1 j2534session` in the firmware
  repository).
- **1.9 (2026-11-17)**: the flow control of an ISO15765 channel is the
  tool's to set. `SET_CONFIG` sends `ISO15765_BS` and `ISO15765_STMIN` to
  the device, and `GET_CONFIG` reads the two back from it, so the DLL
  reports what the bus sees. Contributed by Elias Kotlyar (the idea and
  the first version; the device side is his too). Until now the DLL
  acknowledged both and answered 8 and 1 by itself, and the device sent
  BS 8 / STmin 1 ms whatever a tool set: an ECU's long answer came eight
  frames at a time, one frame a millisecond. With BS 0 / STMIN 0 a
  4095-byte answer was in the tool's hand 0.16 s after the request on the
  bench, where it took 0.72 s with 8 / 1 (`.\test.ps1 j2534fc` in the
  firmware repository). `ISO15765_BS` takes 0 to 0xFF, `ISO15765_STMIN`
  0 to 0x7F (milliseconds) and 0xF1 to 0xF9 (100 to 900 microseconds);
  another value returns `ERR_INVALID_IOCTL_VALUE` and changes nothing,
  the good values of the same list included. The other parameters of a
  list (`DATA_RATE`, `LOOPBACK`, ...) stay with the DLL, as before. It
  needs firmware that takes the two (WiCAN PRO after 6.0.0-beta.11). On
  that firmware a channel opens with BS 0 / STmin 0, the defaults SAE
  J2534-1 names, so a tool that sets nothing gets its long answers in one
  go as well; a tool that wants the old pace sets 8 and 1. On older
  firmware `SET_CONFIG` is acknowledged without effect, as in every
  version before, and `GET_CONFIG` tells: it reads 8 and 1 whatever was
  set, which is what such a firmware sends. With BS 0 an ECU may send a
  whole answer without a pause. The same firmware deepens the WiCAN's CAN
  receive queue for that: while it held 64 frames, an answer was lost on
  the bench when the WiCAN wrote its own flash in the middle of it, and
  with 256 the same test loses none. For a tool that already sets the
  two: udsoncan's J2534 connection sets BS 0x20 / STMIN 0, so its long
  answers come 32 frames at a time without a gap.
- **1.8 (2026-11-17)**: VW TP 2.0 as its own protocol, `TP2_0_PS`
  (0x800E, SAE J2534-2 section 19), contributed by Elias Kotlyar. The DLL forwards a TP 2.0
  channel to the device, which does the transport as SAE J2819 describes
  it; the DLL keeps no state machine of its own and returns reassembled
  messages as they arrive. On a TP 2.0 channel `SET_CONFIG` and
  `GET_CONFIG` reach the device, `REQUEST_CONNECTION` and
  `TEARDOWN_CONNECTION` take an SBYTE_ARRAY, and a write to an id without
  a connection returns `ERR_NO_CONNECTION_ESTABLISHED` (0x00010001).
  `GET_DEVICE_INFO` takes NULL and an SPARAM_LIST (section 25). A message
  passes untouched: the two length bytes a VW ECU expects in front of a
  message are the application's, on a write and on a read. On the other
  protocols (CAN, ISO15765, J1850 VPW) `PassThruReadMsgs` now returns the
  messages it has once nothing more arrived for 4 ms, instead of waiting
  for the full count until its timeout: a tool that asks for a whole
  TP 2.0 block in one read on a raw CAN channel waited out the timeout at
  every block end (940 such waits, 35 s of a 66 s read on the
  contributor's bench). An empty read still waits its timeout. An IOCTL
  without parameters goes to the device as its id alone, as in every
  version before. TP 2.0 needs firmware that serves `TP2_0_PS` (WiCAN PRO
  after 6.0.0-beta.11); everything else works with the firmware 1.7
  worked with. Checked against a bench model of the document and one
  published real exchange (`.\test.ps1 j2534tp20` in the firmware
  repository); not run on a real ECU yet. The installer writes a `TP2_0`
  registry value beside `CAN` and `ISO15765`, the same as
  `wican_j2534.reg`; SAE J2534-2 names no registry entry for TP 2.0 and
  reports the protocol through `GET_DEVICE_INFO`.
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
  2026-11-06: udsoncan over a slow routed link (46 ms per round trip) failed
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
  `tools/testbench/usb/j2534_serial_test.py` of the firmware repository.

On the firmware side, enable the server:
`/api/settings/j2534_server` → `{"enabled":true,"port":6809}` (reboot).
One exposure gate defaults off: `allow_lan` (AP+USB only). The device
forwards every write since the firmware of 2026-10-27 (the old
`allow_reflash` content gate is gone; `/api/j2534` counts the reprogramming
services it saw as `reflash_seen`). See `TESTING_DREWTECH.md` §6.

## Test without a tool

`tools/testbench/usb/j2534_bench.py <wican-ip> 6809 --reflash`, in the
firmware repository ([wican-fw](https://github.com/meatpiHQ/wican-fw),
branch `dev-v6`), drives the same wire protocol end-to-end (a reflash
preamble vs a simulated ECU). If that passes, the DLL against the same
device will too.

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

> The wire protocol is the contract in the firmware's components
> repository:
> [J2534_WIRE_PROTOCOL.md](https://github.com/meatpiHQ/meatpi-components/blob/main/components/j2534_server/J2534_WIRE_PROTOCOL.md).
> Change one side, change both (and bump `WW_VERSION` when the bytes
> change).

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
