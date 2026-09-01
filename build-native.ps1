# build-native.ps1 - configure + build DlssNative.dll and dlss_probe.exe (Release x64), stage into build\out, run the probe.
# Exit code != 0 on any failure.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$sdk = Join-Path $root '..\refs\DLSS-sdk'
$buildDir = Join-Path $root 'build\native'
$outDir = Join-Path $root 'build\out'
$cmake = 'C:\Program Files\CMake\bin\cmake.exe'

if (-not (Test-Path $cmake)) { throw "cmake not found at $cmake" }
$ngxDll = Join-Path $sdk 'lib\Windows_x86_64\rel\nvngx_dlss.dll'
if (-not (Test-Path $ngxDll)) { throw "nvngx_dlss.dll not found at $ngxDll" }

New-Item -ItemType Directory -Force $buildDir, $outDir | Out-Null

& $cmake -S (Join-Path $root 'native') -B $buildDir -G 'Visual Studio 17 2022' -A x64 "-DDLSS_SDK=$((Resolve-Path $sdk).Path)"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
& $cmake --build $buildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }

Copy-Item (Join-Path $buildDir 'Release\DlssNative.dll') $outDir -Force
Copy-Item (Join-Path $buildDir 'Release\dlss_probe.exe') $outDir -Force
Copy-Item $ngxDll $outDir -Force

Push-Location $outDir
try {
    & (Join-Path $outDir 'dlss_probe.exe') $outDir
    $rc = $LASTEXITCODE
} finally { Pop-Location }
if ($rc -ne 0) { throw "dlss_probe failed ($rc)" }
Write-Host "build-native: OK"
