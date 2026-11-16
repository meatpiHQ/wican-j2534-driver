; WiCAN J2534 PassThru driver installer (Inno Setup 6).
;
; Does what USER_MANUAL.md §5 describes by hand:
;   1. copies the 32-bit wican_j2534.dll to Program Files (x86)\WiCAN
;   2. registers the device under PassThruSupport.04.04 in BOTH registry
;      views (WOW6432Node for 32-bit tools + native for 64-bit tools)
;   3. writes the DLL's Host/Port config, asking the user which transport
;      they use (USB = 192.168.82.1, SoftAP = 192.168.80.1, or a custom IP)
;   4. provides a normal Add/Remove Programs uninstall that deletes the
;      keys again
;   5. creates the driver's log folder (Public Documents\WiCAN, 1.4) and a
;      Start Menu shortcut to it, plus one to the user manual
;   6. shows the disclaimer (..\DISCLAIMER.txt, "use at your own risk") on
;      the wizard's licence page, which the user must accept to go on, and
;      installs it next to the DLL with the licence (..\LICENSE, GPL-3.0)
;
; Build with build_installer.ps1 (or:  iscc /DDllPath=<path-to-dll> this-file).
; Re-running the installer is the supported way to change Host/Port: it
; pre-selects the transport from the current registry values.

#ifndef DllPath
  #define DllPath "..\build\Release\wican_j2534.dll"
#endif
#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif

[Setup]
AppId={{6F1D9A2E-3C6B-4E1F-9B0A-52A7C4E81B34}
AppName=WiCAN J2534 PassThru Driver
AppVersion={#AppVersion}
AppPublisher=MeatPi Electronics
AppPublisherURL=https://www.meatpi.com
DefaultDirName={autopf32}\WiCAN
DisableProgramGroupPage=yes
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir=Output
OutputBaseFilename=wican_j2534_setup_{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; The disclaimer is the wizard's licence page: Next stays disabled until the
; user accepts it. Plain ASCII text (Inno reads a file with no BOM as ANSI).
LicenseFile=..\DISCLAIMER.txt
InfoAfterFile=after_install.txt
UninstallDisplayName=WiCAN J2534 PassThru Driver

[Messages]
; The licence page shows the disclaimer, so it says so (the defaults speak
; of a "License Agreement").
WizardLicense=Disclaimer: use at your own risk
LicenseLabel=Please read the following disclaimer and terms of use before continuing.
LicenseLabel3=This driver can send commands to a vehicle, including commands that reprogram its control units. You must accept the following terms before continuing with the installation.
LicenseAccepted=I have read the disclaimer and I &accept it
LicenseNotAccepted=I &do not accept it

[Files]
Source: "{#DllPath}"; DestDir: "{app}"; Flags: ignoreversion
; The disclaimer the user accepted above, and the licence (GPL-3.0)
Source: "..\DISCLAIMER.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
; Rendered from ..\USER_MANUAL.md by build_installer.ps1 (node + Edge headless)
Source: "Output\USER_MANUAL.pdf"; DestDir: "{app}"; Flags: ignoreversion

[Dirs]
; the driver's log folder (1.4): the DLL writes wican_j2534.log there from
; any user's tool, no admin rights needed (USER_MANUAL.md "Driver log")
Name: "{commondocs}\WiCAN"; Permissions: users-modify

[Icons]
Name: "{autoprograms}\WiCAN J2534 driver log"; Filename: "{commondocs}\WiCAN"; Comment: "The driver's log: every message sent to the ECU and received from it"
Name: "{autoprograms}\WiCAN J2534 user manual"; Filename: "{app}\USER_MANUAL.pdf"

[Registry]
; ---- 32-bit view (WOW6432Node on 64-bit Windows): what 32-bit tools read.
Root: HKLM32; Subkey: "SOFTWARE\PassThruSupport.04.04"; Flags: uninsdeletekeyifempty
Root: HKLM32; Subkey: "SOFTWARE\PassThruSupport.04.04\WiCAN"; Flags: uninsdeletekey
Root: HKLM32; Subkey: "SOFTWARE\PassThruSupport.04.04\WiCAN"; ValueType: string; ValueName: "Name"; ValueData: "WiCAN"
Root: HKLM32; Subkey: "SOFTWARE\PassThruSupport.04.04\WiCAN"; ValueType: string; ValueName: "Vendor"; ValueData: "meatpi"
Root: HKLM32; Subkey: "SOFTWARE\PassThruSupport.04.04\WiCAN"; ValueType: string; ValueName: "FunctionLibrary"; ValueData: "{app}\wican_j2534.dll"
Root: HKLM32; Subkey: "SOFTWARE\PassThruSupport.04.04\WiCAN"; ValueType: dword; ValueName: "CAN"; ValueData: "1"
Root: HKLM32; Subkey: "SOFTWARE\PassThruSupport.04.04\WiCAN"; ValueType: dword; ValueName: "ISO15765"; ValueData: "1"
; TP 2.0 (1.8): the same value as ..\wican_j2534.reg. SAE J2534-2 names no
; registry entry for it; it reports the protocol through GET_DEVICE_INFO.
Root: HKLM32; Subkey: "SOFTWARE\PassThruSupport.04.04\WiCAN"; ValueType: dword; ValueName: "TP2_0"; ValueData: "1"
Root: HKLM32; Subkey: "SOFTWARE\WiCAN"; Flags: uninsdeletekeyifempty
Root: HKLM32; Subkey: "SOFTWARE\WiCAN\J2534"; Flags: uninsdeletekey
Root: HKLM32; Subkey: "SOFTWARE\WiCAN\J2534"; ValueType: string; ValueName: "Host"; ValueData: "{code:GetHost}"
Root: HKLM32; Subkey: "SOFTWARE\WiCAN\J2534"; ValueType: dword; ValueName: "Port"; ValueData: "{code:GetPort}"
Root: HKLM32; Subkey: "SOFTWARE\WiCAN\J2534"; ValueType: string; ValueName: "Transport"; ValueData: "{code:GetTransport}"
Root: HKLM32; Subkey: "SOFTWARE\WiCAN\J2534"; ValueType: string; ValueName: "ComPort"; ValueData: "auto"

; ---- native 64-bit view: for the (rarer) 64-bit J2534 tools. Kept in sync.
Root: HKLM64; Subkey: "SOFTWARE\PassThruSupport.04.04"; Flags: uninsdeletekeyifempty; Check: IsWin64
Root: HKLM64; Subkey: "SOFTWARE\PassThruSupport.04.04\WiCAN"; Flags: uninsdeletekey; Check: IsWin64
Root: HKLM64; Subkey: "SOFTWARE\PassThruSupport.04.04\WiCAN"; ValueType: string; ValueName: "Name"; ValueData: "WiCAN"; Check: IsWin64
Root: HKLM64; Subkey: "SOFTWARE\PassThruSupport.04.04\WiCAN"; ValueType: string; ValueName: "Vendor"; ValueData: "meatpi"; Check: IsWin64
Root: HKLM64; Subkey: "SOFTWARE\PassThruSupport.04.04\WiCAN"; ValueType: string; ValueName: "FunctionLibrary"; ValueData: "{app}\wican_j2534.dll"; Check: IsWin64
Root: HKLM64; Subkey: "SOFTWARE\PassThruSupport.04.04\WiCAN"; ValueType: dword; ValueName: "CAN"; ValueData: "1"; Check: IsWin64
Root: HKLM64; Subkey: "SOFTWARE\PassThruSupport.04.04\WiCAN"; ValueType: dword; ValueName: "ISO15765"; ValueData: "1"; Check: IsWin64
Root: HKLM64; Subkey: "SOFTWARE\PassThruSupport.04.04\WiCAN"; ValueType: dword; ValueName: "TP2_0"; ValueData: "1"; Check: IsWin64
Root: HKLM64; Subkey: "SOFTWARE\WiCAN"; Flags: uninsdeletekeyifempty; Check: IsWin64
Root: HKLM64; Subkey: "SOFTWARE\WiCAN\J2534"; Flags: uninsdeletekey; Check: IsWin64
Root: HKLM64; Subkey: "SOFTWARE\WiCAN\J2534"; ValueType: string; ValueName: "Host"; ValueData: "{code:GetHost}"; Check: IsWin64
Root: HKLM64; Subkey: "SOFTWARE\WiCAN\J2534"; ValueType: dword; ValueName: "Port"; ValueData: "{code:GetPort}"; Check: IsWin64
Root: HKLM64; Subkey: "SOFTWARE\WiCAN\J2534"; ValueType: string; ValueName: "Transport"; ValueData: "{code:GetTransport}"; Check: IsWin64
Root: HKLM64; Subkey: "SOFTWARE\WiCAN\J2534"; ValueType: string; ValueName: "ComPort"; ValueData: "auto"; Check: IsWin64

[Code]
const
  HostUsb = '192.168.82.1';
  HostAp = '192.168.80.1';
  DefaultPort = '6809';

var
  TransportPage: TInputOptionWizardPage;
  HostPage: TInputQueryWizardPage;

procedure InitializeWizard;
var
  ExistingHost: string;
  ExistingTransport: string;
  ExistingPort: Cardinal;
begin
  TransportPage := CreateInputOptionPage(wpSelectDir,
    'Connection to the WiCAN',
    'How is this PC connected to the WiCAN?',
    'This sets the IP address the driver uses to reach the WiCAN. ' +
    'You can change it later by re-running this installer.',
    True, False);
  TransportPage.Add('USB cable, network mode (recommended) - WiCAN in device/NCM mode, appears as a network adapter (192.168.82.1)');
  TransportPage.Add('USB cable, COM-port mode - WiCAN in device/cdc mode, appears as a serial port (found automatically)');
  TransportPage.Add('WiCAN WiFi hotspot - this PC joins the WiCAN''s own WiFi network (192.168.80.1)');
  TransportPage.Add('Other - the WiCAN is on my WiFi/LAN, I''ll enter its IP address');
  TransportPage.SelectedValueIndex := 0;

  HostPage := CreateInputQueryPage(TransportPage.ID,
    'WiCAN address',
    'Where can the driver reach the WiCAN?',
    'Enter the WiCAN''s IP address (shown on the Status screen of its web UI) ' +
    'and the J2534 TCP port (default ' + DefaultPort + ').');
  HostPage.Add('IP address:', False);
  HostPage.Add('Port:', False);
  HostPage.Values[0] := HostAp;
  HostPage.Values[1] := DefaultPort;

  { Re-run: pre-select from what's already registered. }
  if RegQueryStringValue(HKLM32, 'SOFTWARE\WiCAN\J2534', 'Transport', ExistingTransport) and
     (CompareText(ExistingTransport, 'serial') = 0) then
    TransportPage.SelectedValueIndex := 1
  else if RegQueryStringValue(HKLM32, 'SOFTWARE\WiCAN\J2534', 'Host', ExistingHost) and
     (ExistingHost <> '') then
  begin
    if ExistingHost = HostUsb then
      TransportPage.SelectedValueIndex := 0
    else if ExistingHost = HostAp then
      TransportPage.SelectedValueIndex := 2
    else
    begin
      TransportPage.SelectedValueIndex := 3;
      HostPage.Values[0] := ExistingHost;
      if RegQueryDWordValue(HKLM32, 'SOFTWARE\WiCAN\J2534', 'Port', ExistingPort) and
         (ExistingPort > 0) and (ExistingPort <= 65535) then
        HostPage.Values[1] := IntToStr(ExistingPort);
    end;
  end;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = HostPage.ID) and (TransportPage.SelectedValueIndex <> 3);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Port: Integer;
begin
  Result := True;
  if CurPageID = HostPage.ID then
  begin
    if Trim(HostPage.Values[0]) = '' then
    begin
      MsgBox('Please enter the WiCAN''s IP address.', mbError, MB_OK);
      Result := False;
      exit;
    end;
    Port := StrToIntDef(Trim(HostPage.Values[1]), -1);
    if (Port < 1) or (Port > 65535) then
    begin
      MsgBox('The port must be a number between 1 and 65535 (default ' +
             DefaultPort + ').', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

function GetHost(Param: string): string;
begin
  case TransportPage.SelectedValueIndex of
    0: Result := HostUsb;
    1: Result := HostAp;   { serial: Host is the unused TCP fallback }
    2: Result := HostAp;
  else
    Result := Trim(HostPage.Values[0]);
  end;
end;

function GetPort(Param: string): string;
begin
  if TransportPage.SelectedValueIndex = 3 then
    Result := IntToStr(StrToIntDef(Trim(HostPage.Values[1]), 6809))
  else
    Result := DefaultPort;
end;

function GetTransport(Param: string): string;
begin
  if TransportPage.SelectedValueIndex = 1 then
    Result := 'serial'
  else
    Result := 'tcp';
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo,
  MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: string): string;
var
  Where: string;
begin
  if TransportPage.SelectedValueIndex = 1 then
    Where := 'USB COM port (auto-detected by the driver)'
  else
    Where := GetHost('') + ':' + GetPort('');
  Result := MemoDirInfo + NewLine + NewLine +
    'WiCAN connection for the driver:' + NewLine +
    Space + Where + NewLine + NewLine +
    'Registry (32-bit and 64-bit views):' + NewLine +
    Space + 'PassThruSupport.04.04\WiCAN  (device registration)' + NewLine +
    Space + 'SOFTWARE\WiCAN\J2534  (Transport/Host/Port/ComPort)';
end;
