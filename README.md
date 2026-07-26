# WiCAN J2534 PassThru DLL

A Windows SAE J2534-1 (04.04) pass-through driver for WiCAN. It lets any
J2534 application (OEM diagnostic/reflash tools, OpenPort-style utilities)
drive a vehicle's CAN bus **through WiCAN**. The DLL translates the ~14
`PassThru*` calls into WiCAN's wire protocol (`wican_wire.h`) over one of
two transports to the firmware's `j2534_server`:

- **TCP** — WiFi (STA or the WiCAN AP), USB-Ethernet, USB device NCM/RNDIS.
- **Serial** — WiCAN as a USB CDC-ACM virtual COM port
  (`device_class=cdc`, VID 16D0 PID 1263), the Tactrix-style COM-port
  J2534 device. The DLL can auto-discover the port by USB VID/PID.

## What it supports

- Protocols **CAN** and **ISO15765** (ISO-TP) — the CAN-bus diagnostics
  and UDS reflash that modern (2008+) vehicles use. Other protocols return
  `ERR_NOT_SUPPORTED` (WiCAN is CAN hardware).
- `PassThruOpen/Close/Connect/Disconnect`, `ReadMsgs/WriteMsgs`,
  `StartMsgFilter/StopMsgFilter` (incl. FLOW_CONTROL for ISO-TP),
  `StartPeriodicMsg/StopPeriodicMsg` (tester present), `Ioctl`
  (READ_VBATT, CLEAR_*), `ReadVersion`, `GetLastError`.
- `SetProgrammingVoltage` → `ERR_NOT_SUPPORTED` (no pin-control hardware).

## Build

32-bit is the safe default — most OEM J2534 apps load a **32-bit** DLL.

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

Output: `wican_j2534.dll`.

## Install

**End users: use the installer.** `installer\build_installer.ps1` builds
`wican_j2534_setup_<version>.exe` (Inno Setup 6 —
`winget install JRSoftware.InnoSetup`) from a built 32-bit DLL. The
setup.exe copies the DLL to `Program Files (x86)\WiCAN`, asks which
transport the user has (USB network / USB COM-port / SoftAP / custom IP),
registers both registry views, and ships the user manual as a PDF
(rendered at build time from USER_MANUAL.md via `installer\md2html.mjs`
+ Edge headless — needs node.exe and Edge on the build PC); re-running
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
for keys **starting with** `VID_16D0&PID_1263` (prefix match — CDC-ACM
is a composite device, so the COM child sits under `...&MI_00`), reads
their `PortName`s, and uses the first one that opens (a successful open
is the presence test; the SYSTEM hive is not WOW64-redirected).

Typical setups:
- **WiFi STA** — WiCAN on the shop LAN: set `Host` to its LAN IP.
- **WiCAN AP** — connect the PC to WiCAN's WiFi: use the AP gateway IP.
- **USB-Ethernet (host)** — WiCAN's wired-uplink IP.
- **USB-device NCM/RNDIS** — plug WiCAN into the PC
  (`usb_host_manager.role=device`, `device_class=ncm`); it appears as a
  network adapter ("MeatPi Electronics WiCAN Pro", inbox `UsbNcm.sys` on
  Win10/11) and serves DHCP on the link. Set `Host` to **`192.168.82.1`**
  (the USB-link address — the .80.1 default is the WiFi AP). Driver-tested
  end-to-end over the cable 2026-07-08.
- **USB-device CDC-ACM (serial)** — `device_class=cdc`: WiCAN enumerates as
  a virtual COM port (inbox `usbser.sys` → "USB Serial Device (COMn)",
  VID 16D0 PID 1263) carrying the same framed wire protocol over serial
  instead of TCP. The DLL opens it overlapped with DTR asserted (the
  firmware gates the serial session on DTR); set `Transport=serial` in
  the registry (or `WICAN_J2534_COM=auto`) and the port is found by
  VID/PID — no configuration at all. Firmware side bench-tested by
  `tools/testbench/j2534_serial_test.py`.

On the firmware side, enable the server:
`/api/settings/j2534_server` → `{"enabled":true,"port":6809}` (reboot).
Two safety gates default off: `allow_reflash` (blocks ECU flashing) and
`allow_lan` (AP+USB only). See `TESTING_DREWTECH.md` §6.

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

> The wire protocol is the contract in
> `components/j2534_server/TASK_j2534_server.md §4`. Change one side, change
> both (and bump `WW_VERSION`).
