# build-native.ps1 - configure + build RenderforgeNative.dll and dlss_probe.exe (Release x64), stage into build\out, run the probe.
# Exit code != 0 on any failure.
param(
    # The quality gate's build (qgate.json, pre-commit): its OWN intermediate + output dirs. The gate may build a tree
    # that is not the one being deployed (a partially staged commit, a stash-reduced tree); it must never leave its
    # objects where deploy.ps1 / build\release.ps1 link from.
    [switch] $Gate,
    # Throw the intermediate dir away first: every object is compiled from the sources on disk. build\release.ps1 uses it.
    [switch] $Clean
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
. (Join-Path $root 'build\stamp.ps1')
# qgate.json calls this script without arguments (its content is hash-trusted by the owner, `qgate trust`), so a run
# from inside the gate is recognised by the slot marker a -Full gate run exports to its checks (gate\check.ps1).
if ($env:QGATE_SLOT_HELD -eq '1') { $Gate = $true }
if ($Gate) { Write-Host 'build-native: gate build -> build\gate\ (never the dirs deploy.ps1 / release.ps1 link from)' }
$sdk = Join-Path $root '..\refs\DLSS-sdk'
$buildDir = Join-Path $root ($Gate ? 'build\gate\native' : 'build\native')
$outDir = Join-Path $root ($Gate ? 'build\gate\out' : 'build\out')
$cmake = 'C:\Program Files\CMake\bin\cmake.exe'

if (-not (Test-Path $cmake)) { throw "cmake not found at $cmake" }
# Shipped DLSS runtime = the SDK's rel nvngx_dlss.dll slot, overwritten with the newest NVIDIA-signed build
# (currently the SDK's own 310.9.1 build, DLSS SDK v310.9.1, 2026-09-28). Signature is verified so a tampered/packed DLL never ships.
$ngxDll = Join-Path $sdk 'lib\Windows_x86_64\rel\nvngx_dlss.dll'
if (-not (Test-Path $ngxDll)) { throw "nvngx_dlss.dll not found at $ngxDll" }
$sig = Get-AuthenticodeSignature $ngxDll
if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'NVIDIA Corporation') { throw "nvngx_dlss.dll signature invalid: $ngxDll" }
Write-Host "nvngx_dlss.dll $((Get-Item $ngxDll).VersionInfo.FileVersion) from $ngxDll"

# AMD FidelityFX SDK 2.3 signed binaries: the small loader, the upscaler DLL (FSR 4.1.1 ML + 3.1.5 fallback
# in one file) and the frame-generation DLL (FG 4.0.1 ML + 3.1.x analytical). All Authenticode-signed by AMD;
# a tampered or repacked DLL must never ship.
$ffxSdk = Join-Path $root '..\refs\FidelityFX-SDK'
$amdBin = Join-Path $ffxSdk 'Kits\FidelityFX\signedbin'
$amdDlls = @('amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll', 'amd_fidelityfx_framegeneration_dx12.dll') | ForEach-Object { Join-Path $amdBin $_ }
foreach ($dll in $amdDlls) {
    if (-not (Test-Path $dll)) { throw "AMD FidelityFX DLL not found at $dll" }
    $s = Get-AuthenticodeSignature $dll
    if ($s.Status -ne 'Valid' -or $s.SignerCertificate.Subject -notmatch 'Advanced Micro Devices') { throw "AMD DLL signature invalid: $dll" }
    Write-Host ("{0} {1} from {2}" -f (Split-Path $dll -Leaf), (Get-Item $dll).VersionInfo.FileVersion, $dll)
}

# Intel XeSS SDK 3.0.2: libxess.dll (D3D12, cross-vendor DP4a + Intel XMX) shipped verbatim, Intel-signed.
$xessSdk = Join-Path $root '..\refs\XeSS-sdk'
$xessDll = Join-Path $xessSdk 'bin\libxess.dll'
if (-not (Test-Path $xessDll)) { throw "libxess.dll not found at $xessDll" }
$xsig = Get-AuthenticodeSignature $xessDll
if ($xsig.Status -ne 'Valid' -or $xsig.SignerCertificate.Subject -notmatch 'Intel Corporation') { throw "libxess.dll signature invalid: $xessDll" }
Write-Host "libxess.dll $((Get-Item $xessDll).VersionInfo.FileVersion) from $xessDll"
if (-not (Test-Path (Join-Path $root 'LICENSE-INTEL.txt'))) { throw "LICENSE-INTEL.txt missing (copy refs\XeSS-sdk\LICENSE.txt)" }
# XeSS-FG + XeLL runtimes (FgXess.cpp: frame generation on the child HWND, XeLL mandatory), Intel-signed, delay-loaded.
$xessFgDll = Join-Path $xessSdk 'bin\libxess_fg.dll'
$xellDll   = Join-Path $xessSdk 'bin\libxell.dll'
foreach ($dll in $xessFgDll, $xellDll) {
    if (-not (Test-Path $dll)) { throw "$(Split-Path $dll -Leaf) not found at $dll" }
    $s = Get-AuthenticodeSignature $dll
    if ($s.Status -ne 'Valid' -or $s.SignerCertificate.Subject -notmatch 'Intel Corporation') { throw "$(Split-Path $dll -Leaf) signature invalid: $dll" }
    Write-Host ("{0} {1} from {2}" -f (Split-Path $dll -Leaf), (Get-Item $dll).VersionInfo.FileVersion, $dll)
}

# NVIDIA Streamline 2.14.1 (FgStreamline.cpp: DLSS-G / MFG, Reflex, PCL), all NVIDIA-signed, loaded at runtime only.
# nvngx_dlssg.dll: the SDK's bin\x64 copy can lag; ship the newest NVIDIA-signed build from latest-dll\ (310.9.1 = SL 2.14.1 bin\x64 copy).
$slSdk = Join-Path $root '..\refs\Streamline'
$slDlls = @('sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll', 'sl.reflex.dll', 'sl.pcl.dll') | ForEach-Object { Join-Path $slSdk "bin\x64\$_" }
$slDlls += Join-Path $slSdk 'latest-dll\nvngx_dlssg.dll'
foreach ($dll in $slDlls) {
    if (-not (Test-Path $dll)) { throw "Streamline DLL not found at $dll" }
    $s = Get-AuthenticodeSignature $dll
    if ($s.Status -ne 'Valid' -or $s.SignerCertificate.Subject -notmatch 'NVIDIA Corporation') { throw "Streamline DLL signature invalid: $dll" }
    Write-Host ("{0} {1} from {2}" -f (Split-Path $dll -Leaf), (Get-Item $dll).VersionInfo.FileVersion, $dll)
}
$dlssgVer = (Get-Item $slDlls[-1]).VersionInfo.FileVersion -replace '[ ,]+', '.'
if ($dlssgVer -notlike '310.9.*') { Write-Warning "nvngx_dlssg.dll is $dlssgVer, expected a 310.9.* build (refs\Streamline\latest-dll, 310.9.1 verified) - shipping it anyway" }

# --- Objects follow source CONTENT, not file time -------------------------------------------------------------------
# MSBuild rebuilds an object when an input is NEWER than it. A file whose content changed while its mtime did not move
# forward (restored by a copy that keeps timestamps, an archive, a sync tool) is invisible to it: the stale object is
# linked and nothing says so (2026-10-12, STALE-BUILD). So every build records the content hash of each native source
# next to the objects, and the next build RE-DATES every file whose content differs from that record. A build dir
# without a record is of unknown origin and is thrown away.
$manifest = Join-Path $buildDir 'rf-src-manifest.txt'
$wipe = $null
if ($Clean -and (Test-Path $buildDir)) { $wipe = '-Clean' }
elseif ((Test-Path $buildDir) -and -not (Test-Path $manifest)) { $wipe = 'no source manifest (objects of unknown origin)' }
if ($wipe) {
    if ((Split-Path $buildDir -Leaf) -ne 'native' -or -not $buildDir.StartsWith((Join-Path $root 'build'))) { throw "refusing to remove '$buildDir'" }
    Write-Host "build-native: removing $buildDir ($wipe)"
    [IO.Directory]::Delete($buildDir, $true)
}
New-Item -ItemType Directory -Force $buildDir, $outDir | Out-Null
$now = Get-RfSourceFiles 'native'
if (Test-Path $manifest) {
    $was = @{}
    foreach ($l in [IO.File]::ReadAllLines($manifest)) { if ($l.Length -gt 41) { $was[$l.Substring(41)] = $l.Substring(0, 40) } }
    $redate = [DateTime]::UtcNow
    foreach ($l in $now) {
        $path = $l.Substring(41)
        if ($was[$path] -ne $l.Substring(0, 40)) {
            (Get-Item -LiteralPath (Join-Path $root $path)).LastWriteTimeUtc = $redate
            Write-Host "build-native: content changed since the last build of this dir, re-dated: $path"
        }
    }
}
[IO.File]::WriteAllLines($manifest, [string[]]$now)
# The stamp the DLL carries (Rf_BuildStamp): commit + dirty flag + hash of exactly the list above.
& (Join-Path $root 'build\stamp.ps1') -WriteNative (Join-Path $buildDir 'rf_build_stamp.h')

& $cmake -S (Join-Path $root 'native') -B $buildDir -G 'Visual Studio 17 2022' -A x64 "-DDLSS_SDK=$((Resolve-Path $sdk).Path)" "-DFFX_SDK=$((Resolve-Path $ffxSdk).Path)" "-DXESS_SDK=$((Resolve-Path $xessSdk).Path)"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
& $cmake --build $buildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }

# The linked DLL must carry the stamp of the tree on disk: proves the stamp TU was compiled and linked in THIS build.
Assert-RfStamp (Join-Path $buildDir 'Release\RenderforgeNative.dll') 'native' | Out-Null
Copy-Item (Join-Path $buildDir 'Release\RenderforgeNative.dll') $outDir -Force
Copy-Item (Join-Path $buildDir 'Release\dlss_probe.exe') $outDir -Force
Copy-Item $ngxDll $outDir -Force
foreach ($dll in $amdDlls) { Copy-Item $dll $outDir -Force }
Copy-Item $xessDll $outDir -Force
Copy-Item $xessFgDll $outDir -Force
Copy-Item $xellDll $outDir -Force
foreach ($dll in $slDlls) { Copy-Item $dll $outDir -Force }

Push-Location $outDir
try {
    & (Join-Path $outDir 'dlss_probe.exe') $outDir
    $rc11 = $LASTEXITCODE
    & (Join-Path $outDir 'dlss_probe.exe') $outDir --d3d12
    $rc12 = $LASTEXITCODE
    & (Join-Path $outDir 'dlss_probe.exe') $outDir --fsr
    $rcFsr = $LASTEXITCODE
    & (Join-Path $outDir 'dlss_probe.exe') $outDir --xess
    $rcXess = $LASTEXITCODE
    & (Join-Path $outDir 'dlss_probe.exe') $outDir --fake=2
    $rcFake = $LASTEXITCODE
} finally { Pop-Location }
# Exit 3 = the machine cannot run that provider (probe could not init: no NVIDIA RTX, no DP4a, ...): the DLLs still
# ship, so warn only. Anything else non-zero is a real create/dispatch failure and gates the build.
if ($rc11 -eq 3) { Write-Warning "dlss_probe (D3D11): NGX unavailable on this GPU/driver - DLSS untested, build continues" }
elseif ($rc11 -ne 0) { throw "dlss_probe (D3D11) failed ($rc11)" }
if ($rc12 -eq 3) { Write-Warning "dlss_probe (D3D12): NGX unavailable on this GPU/driver - DLSS untested, build continues" }
elseif ($rc12 -ne 0) { throw "dlss_probe (D3D12) failed ($rc12)" }
if ($rcFsr -eq 3) { Write-Warning "dlss_probe (FSR): no D3D12 upscale provider on this machine - FSR untested, build continues" }
elseif ($rcFsr -ne 0) { throw "dlss_probe (FSR) failed ($rcFsr)" }
if ($rcXess -eq 3) { Write-Warning "dlss_probe (XeSS): this GPU/driver cannot run XeSS (SM 6.4 + DP4a) - XeSS untested, build continues" }
elseif ($rcXess -ne 0) { throw "dlss_probe (XeSS) failed ($rcXess)" }
# Post-only must work on EVERY GPU, so exit 3 is not tolerated here: --fake=2 replaces the NGX init call and is
# therefore deterministic on any device. (--fake=3|4 need a working NGX init, so they stay a manual run.)
if ($rcFake -ne 0) { throw "dlss_probe (--fake=2, post-only) failed ($rcFake)" }
Write-Host "build-native: OK"
