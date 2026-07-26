# WiCAN J2534 PassThru — User Manual

The WiCAN Pro can act as an **SAE J2534-1 (04.04) PassThru interface**:
any Windows J2534 diagnostic or reflash application talks to a vehicle's
CAN bus **through the WiCAN**, over WiFi or USB. This manual covers
installing the driver, connecting the WiCAN to your PC, using it from a
diagnostic tool, the safety gates, and troubleshooting.

---

## 1. What it is and what it supports

A J2534 "PassThru" driver is a standard Windows DLL that OEM diagnostic
tools (and reflash utilities) load to reach a vehicle. WiCAN provides
that DLL (`wican_j2534.dll`); it forwards the J2534 calls over the
network to the WiCAN firmware, which drives the CAN bus.

**Supported**
- Protocols: **CAN** and **ISO15765** (ISO-TP) — the diagnostics and UDS
  reflash used by modern (≈2008+) vehicles.
- The full PassThru call set a real tool uses: `PassThruOpen / Close /
  Connect / Disconnect`, `ReadMsgs / WriteMsgs`, `StartMsgFilter /
  StopMsgFilter` (including **FLOW_CONTROL** filters for ISO-TP),
  `StartPeriodicMsg / StopPeriodicMsg` (tester-present), `Ioctl`
  (`READ_VBATT`, the `CLEAR_*` buffers), `ReadVersion`, `GetLastError`.

**Not supported** (by design — WiCAN is CAN hardware)
- Non-CAN protocols (J1850, ISO9141, KWP2000 over K-line, SCI…) →
  `ERR_NOT_SUPPORTED`.
- `SetProgrammingVoltage` (no pin-control hardware) → `ERR_NOT_SUPPORTED`.

**Validated:** passed the free **DrewTech "Tool for J2534-1 API"**
conformance app over both USB and WiFi — Open, ReadVersion, Connect
(CAN + ISO15765), FLOW_CONTROL filters, and correctly-framed ISO-TP
WriteMsgs/ReadMsgs.

---

## 2. Requirements

- A WiCAN Pro on firmware v6.
- A Windows PC (10/11). Most OEM J2534 apps are **32-bit** and load a
  32-bit driver — the WiCAN DLL is 32-bit; keep that in mind for the
  registry (§5).
- A way to connect the PC to the WiCAN (WiFi or USB — §4).
- A vehicle (or an ECU / the bundled ECU simulator) on the WiCAN CAN bus.
- Admin rights once, to import the driver registration.

---

## 3. Overview of the steps

1. **Firmware:** enable the J2534 server, pick the transport, set the
   safety gates (§4).
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

Default TCP port is **6809**. Two safety gates (both default **false**)
are covered in §7 — leave them off for read-only diagnostics.

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

Run **`wican_j2534_setup_<version>.exe`** (accept the UAC prompt). It
does everything in this section for you:

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
   id) — forces the **serial** transport
2. Environment: `WICAN_J2534_HOST` / `WICAN_J2534_PORT` (a `COMn` value
   in `Host` selects serial too)
3. Registry: `HKLM\SOFTWARE\WiCAN\J2534` → `Transport` (`tcp`/`serial`),
   `ComPort` (`COMn` or `auto`), `Host`, `Port`
   (the `WOW6432Node` copy for 32-bit apps)
4. Compiled default: TCP `192.168.80.1:6809`

So you can override the target quickly with an environment variable
without touching the registry. In serial mode with `ComPort=auto` the
driver locates the WiCAN's COM port by its USB id (VID 16D0 PID 1263)
— no port number to configure.

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
   - J2534 DLL Version: `WiCAN J2534 1.1`
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
5. **Send/receive.** Protocol ISO15765, send `10 02` → expect `50 02 …`;
   `27 01` → `67 01` + seed; and so on. The tool sends the ISO15765
   payload **without** a leading 4-byte CAN id and relies on the
   flow-control filter for addressing, which matches WiCAN's convention.

That's the whole loop: **Open → Connect → Flow-Control filter →
Write/Read**. OEM reflash apps do the same, plus periodic tester-present
and the reflash services (which need the gate in §7).

---

## 7. Safety gates (read this before reflashing)

J2534 can reprogram ECUs, so WiCAN guards it with two settings (both
default **false**):

- **`allow_reflash`** — when **false**, the device **rejects the UDS
  reprogramming services** (RequestDownload `0x34`, Upload `0x35`,
  TransferData `0x36`, TransferExit `0x37`). A connected tool can read
  and diagnose but **cannot write ECU firmware**. Flip it **true** only
  for a real, intended reflash job:
  ```
  PUT /api/settings/j2534_server {"enabled":true,"allow_reflash":true}
  POST /api/settings/submit
  ```
  Set it back to `false` when done.
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
  and the `allow_reflash` / `allow_lan` flags.
- **`GET /api/can`** — `tx`, `rx`, `tx_errors`, `bus_errors`: use these
  to tell a WiCAN problem from a bus/ECU one (e.g. no ACK = wrong bitrate
  or nothing on the bus).
- **`GET /api/logs/ring`** — the `ISO15765 channel bound …` line and
  per-frame send/tx traces.

---

## 9. Test without a diagnostic tool

You can exercise the entire path (driver protocol → firmware → a
simulated ECU) before involving a real tool:

```
python tools/testbench/j2534_bench.py <wican-ip> 6809 --reflash
```

If that passes, a real J2534 app against the same device will too. To
give the tool something to talk to, run the bundled ECU simulator on the
CAN bus (e.g. `pcan_reflash_ecu.py`, request `0x7E0` / response `0x7E8`).

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
| Reflash services rejected (`0x34/36/37` fail) | `allow_reflash` is `false` (default). Enable it only for an intended reflash (§7). |
| Data looks wrong / cross-talk on the bus | An `autopid` group polling the same bus can interleave with the tester. Disable autopid while running J2534. |
| Flow-Control column greyed out | You're on a **CAN** channel; flow control is ISO15765-only. Connect an ISO15765 channel for ISO-TP. |

---

## 11. Reference

- **Ports/IPs:** TCP **6809**; USB-device `192.168.82.1`, AP
  `192.168.80.1`. USB-serial: COM port, VID `16D0` PID `1263`.
- **Version strings** (`PassThruReadVersion`): firmware `WiCAN v6`, DLL
  `WiCAN J2534 1.1`, API `04.04`.
- **Protocols:** CAN, ISO15765. **Baud:** 500000 typical (match the
  bus).
- **Exports:** the 14 `PassThru*` functions (see `wican_j2534.def`).
- **Transports:** TCP over WiFi (AP or STA) / USB-Ethernet / USB-device
  NCM, and **USB-device CDC-ACM serial** (a virtual COM port,
  `device_class=cdc`, auto-discovered by the driver — the classic
  COM-port J2534 device).
- **Limitations:** CAN/ISO15765 only; no `SetProgrammingVoltage` (returns
  `ERR_NOT_SUPPORTED`).

---

*Developer/protocol details (the wire format, building the DLL, the
DrewTech validation log) live in `README.md` and `TESTING_DREWTECH.md`
alongside this file.*
