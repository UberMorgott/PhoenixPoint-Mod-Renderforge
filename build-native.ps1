# build-native.ps1 - configure + build RenderforgeNative.dll and dlss_probe.exe (Release x64), stage into build\out, run the probe.
# Exit code != 0 on any failure.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$sdk = Join-Path $root '..\refs\DLSS-sdk'
$buildDir = Join-Path $root 'build\native'
$outDir = Join-Path $root 'build\out'
$cmake = 'C:\Program Files\CMake\bin\cmake.exe'

if (-not (Test-Path $cmake)) { throw "cmake not found at $cmake" }
# Shipped DLSS runtime = the SDK's rel nvngx_dlss.dll slot, overwritten with the newest NVIDIA-signed build
# (310.7.129, DLSS Swapper manifest CDN, 2026-09-02). Signature is verified so a tampered/packed DLL never ships.
$ngxDll = Join-Path $sdk 'lib\Windows_x86_64\rel\nvngx_dlss.dll'
if (-not (Test-Path $ngxDll)) { throw "nvngx_dlss.dll not found at $ngxDll" }
$sig = Get-AuthenticodeSignature $ngxDll
if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'NVIDIA Corporation') { throw "nvngx_dlss.dll signature invalid: $ngxDll" }
Write-Host "nvngx_dlss.dll $((Get-Item $ngxDll).VersionInfo.FileVersion) from $ngxDll"

New-Item -ItemType Directory -Force $buildDir, $outDir | Out-Null

& $cmake -S (Join-Path $root 'native') -B $buildDir -G 'Visual Studio 17 2022' -A x64 "-DDLSS_SDK=$((Resolve-Path $sdk).Path)"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
& $cmake --build $buildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }

Copy-Item (Join-Path $buildDir 'Release\RenderforgeNative.dll') $outDir -Force
Copy-Item (Join-Path $buildDir 'Release\dlss_probe.exe') $outDir -Force
Copy-Item $ngxDll $outDir -Force

Push-Location $outDir
try {
    & (Join-Path $outDir 'dlss_probe.exe') $outDir
    $rc11 = $LASTEXITCODE
    & (Join-Path $outDir 'dlss_probe.exe') $outDir --d3d12
    $rc12 = $LASTEXITCODE
} finally { Pop-Location }
if ($rc11 -ne 0) { throw "dlss_probe (D3D11) failed ($rc11)" }
if ($rc12 -ne 0) { throw "dlss_probe (D3D12) failed ($rc12)" }
Write-Host "build-native: OK"
