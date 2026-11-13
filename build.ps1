# Build the 32-bit PassThru DLL without installing it.
$ErrorActionPreference = 'Stop'
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmake) {
    $exe = $cmake.Source
} else {
    $exe = 'C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe'
}
if (-not (Test-Path $exe)) { throw 'CMake is not installed or on PATH' }
$build = Join-Path $PSScriptRoot 'build'
if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
    & $exe -S $PSScriptRoot -B $build -A Win32
    if ($LASTEXITCODE -ne 0) { throw 'Driver configure failed' }
}
& $exe --build $build --config Release
if ($LASTEXITCODE -ne 0) { throw 'Driver build failed' }
