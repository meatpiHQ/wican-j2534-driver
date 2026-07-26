# Builds the WiCAN J2534 driver installer (setup.exe) from a built DLL.
#
#   .\build_installer.ps1 -DllPath C:\path\to\wican_j2534.dll [-Version 1.0.0]
#
# Needs Inno Setup 6 (winget install JRSoftware.InnoSetup). The DLL must be
# 32-bit (see ..\README.md for the cmake build); this script refuses a 64-bit
# one, since most OEM J2534 apps load a 32-bit driver.
#
# Output: .\Output\wican_j2534_setup_<version>.exe

param(
    [string]$DllPath,
    [string]$Version = "1.0.0"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# --- locate the DLL -------------------------------------------------------
if (-not $DllPath) {
    $candidates = @(
        (Join-Path $root "..\build\Release\wican_j2534.dll"),
        (Join-Path $root "..\build\wican_j2534.dll")
    )
    $DllPath = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $DllPath) {
        throw "No wican_j2534.dll found in ..\build. Build it first (see ..\README.md) or pass -DllPath."
    }
}
$DllPath = (Resolve-Path $DllPath).Path

# --- refuse a 64-bit DLL --------------------------------------------------
$bytes = [System.IO.File]::ReadAllBytes($DllPath)
$peOff = [BitConverter]::ToInt32($bytes, 0x3C)
$machine = [BitConverter]::ToUInt16($bytes, $peOff + 4)
if ($machine -ne 0x014C) {
    throw ("{0} is not a 32-bit x86 DLL (PE machine 0x{1:X4}). Most J2534 apps need 32-bit - build with 'cmake -A Win32'." -f $DllPath, $machine)
}

# --- locate ISCC ----------------------------------------------------------
$iscc = $null
$cmd = Get-Command iscc -ErrorAction SilentlyContinue
if ($cmd) { $iscc = $cmd.Source }
if (-not $iscc) {
    $isccCandidates = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    )
    $iscc = $isccCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $iscc) {
    throw "Inno Setup 6 not found. Install it:  winget install JRSoftware.InnoSetup"
}

# --- render the user manual to PDF (node md->html, Edge html->pdf) --------
$outDir = Join-Path $root "Output"
New-Item -ItemType Directory -Force $outDir | Out-Null
$manualMd = Join-Path $root "..\USER_MANUAL.md"
$manualHtml = Join-Path $outDir "USER_MANUAL.html"
$manualPdf = Join-Path $outDir "USER_MANUAL.pdf"
$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) { throw "node.exe not found - needed to render USER_MANUAL.pdf" }
& $node.Source (Join-Path $root "md2html.mjs") $manualMd $manualHtml
if ($LASTEXITCODE -ne 0) { throw "md2html.mjs failed" }
$edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
          "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw "msedge.exe not found - needed to render USER_MANUAL.pdf" }
Remove-Item $manualPdf -ErrorAction SilentlyContinue
# Start-Process, not direct invocation: Edge logs to stderr on success and
# PS 5.1 wraps redirected native stderr into a fake NativeCommandError.
Start-Process -FilePath $edge -ArgumentList "--headless", "--disable-gpu",
    "--no-pdf-header-footer", "--print-to-pdf=$manualPdf",
    "file:///$($manualHtml -replace '\\','/')" -Wait -WindowStyle Hidden
if (-not (Test-Path $manualPdf)) { throw "Edge did not produce $manualPdf" }
$pdfHead = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($manualPdf)[0..4])
if ($pdfHead -ne "%PDF-") { throw "$manualPdf is not a valid PDF" }
Write-Host "PDF:  $manualPdf ($((Get-Item $manualPdf).Length) bytes)"

# --- compile --------------------------------------------------------------
Write-Host "DLL:  $DllPath"
Write-Host "ISCC: $iscc"
& $iscc "/DDllPath=$DllPath" "/DAppVersion=$Version" (Join-Path $root "wican_j2534_installer.iss")
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }

$out = Join-Path $root "Output\wican_j2534_setup_$Version.exe"
Write-Host ""
Write-Host "Installer: $out"
