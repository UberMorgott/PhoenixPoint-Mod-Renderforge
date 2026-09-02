# build\release.ps1 - assemble the Renderforge release artefacts.
#
# Produces, in build\release\:
#   Renderforge-Core-<v>.zip     managed DLL + native shim + meta.json + README + MIT + NIS licence
#   Renderforge-NVIDIA-<v>.zip   nvngx_dlss.dll (+ Streamline / nvngx_dlssg.dll with -WithFrameGen)
#   Renderforge-AMD-<v>.zip      amd_fidelityfx_*_dx12.dll
#   Renderforge-Intel-<v>.zip    libxess.dll (+ libxess_fg.dll, libxell.dll with -WithFrameGen)
#   Renderforge-Full-<v>.zip     the union of all four
#   SHA256SUMS.txt               one line per zip
#
# LAYOUT RULE: every zip carries ONE top-level "Renderforge/" folder, so extracting any of them -
# or several, in any order - into <Phoenix Point>\Mods\ merges into a single Mods\Renderforge\.
# A vendor zip contains only that pack's extra files; Core is what makes the folder usable.
#
# Every vendor DLL is Authenticode-asserted (signer subject must match the vendor) before it is
# packed; an unsigned, expired or third-party-signed binary fails the build. NVIDIA NGX DLLs are
# additionally compared against the pinned "newest known" versions below - NVIDIA ships newer NGX
# runtimes outside the SDKs, and shipping the stale SDK copy has already bitten us twice.
param(
    [string] $Configuration = 'Release',
    # Reference assemblies for the managed build (same default as deploy.ps1).
    [string] $PPRoot = 'D:\PP-Instance2',
    # Reuse bin\ and build\out\ as they are instead of running build-native.ps1 + dotnet build.
    [switch] $SkipBuild,
    # Add the Phase 5 frame-generation DLLs (Streamline, AMD FG, XeSS-FG + XeLL) to the vendor packs.
    [switch] $WithFrameGen,
    # Check sources, signatures and versions; pack nothing.
    [switch] $ValidateOnly
)

$ErrorActionPreference = 'Stop'
$root   = Split-Path $PSScriptRoot -Parent            # repo root: this script lives in build\
$refs   = Join-Path (Split-Path $root -Parent) 'refs' # E:\DEV\PhoenixPoint\refs
$outDir = Join-Path $root 'build\out'
$relDir = Join-Path $root 'build\release'
$stage  = Join-Path $relDir 'stage'

# --- Newest NVIDIA NGX runtimes we know of. Update together with refs\ after checking the
# --- TechPowerUp DLL databases; a mismatch is a WARNING, never a hard failure.
$NewestKnownNgx = @{
    'nvngx_dlss.dll'  = '310.7.129.0'
    'nvngx_dlssg.dll' = '310.7.129.0'
}

function Get-NormalVersion([string] $path) {
    # NVIDIA reports FileVersion with commas ("310,7,129,0"), AMD and Intel with dots.
    $v = (Get-Item $path).VersionInfo.FileVersion
    if (-not $v) { return '' }
    ($v -replace '[ ,]', '.').Trim()
}

function Assert-VendorSignature([string] $path, [string] $signer) {
    $sig = Get-AuthenticodeSignature $path
    if ($sig.Status -ne 'Valid') {
        throw "Authenticode: $path is not Valid (status $($sig.Status)). Refusing to ship it."
    }
    if ($sig.SignerCertificate.Subject -notmatch [regex]::Escape($signer)) {
        throw "Authenticode: $path is signed by '$($sig.SignerCertificate.Subject)', expected '$signer'."
    }
}

# --- The pack table. Src = absolute source path, Signer = required Authenticode subject
# --- (absent = our own file, no signature expected), Fg = only packed with -WithFrameGen,
# --- Licence = the notice file that covers it.
$version = (Get-Content (Join-Path $root 'meta.json') -Raw | ConvertFrom-Json).Version
if (-not $version) { throw "No Version in meta.json" }

$packs = [ordered]@{
    'Core' = @(
        @{ Src = Join-Path $root "bin\$Configuration\Renderforge\Renderforge.dll" }
        @{ Src = Join-Path $outDir 'RenderforgeNative.dll' }
        @{ Src = Join-Path $root 'meta.json' }
        @{ Src = Join-Path $root 'README.md' }
        @{ Src = Join-Path $root 'LICENSE';         Licence = 'LICENSE' }
        @{ Src = Join-Path $root 'LICENSE-NIS.txt'; Licence = 'LICENSE-NIS.txt' }
    )
    'NVIDIA' = @(
        @{ Src = Join-Path $refs 'DLSS-sdk\lib\Windows_x86_64\rel\nvngx_dlss.dll'; Signer = 'NVIDIA Corporation'; Licence = 'LICENSE-NVIDIA.txt' }
        @{ Src = Join-Path $refs 'Streamline\latest-dll\nvngx_dlssg.dll';          Signer = 'NVIDIA Corporation'; Licence = 'LICENSE-NVIDIA.txt'; Fg = $true }
        @{ Src = Join-Path $refs 'Streamline\bin\x64\sl.interposer.dll';           Signer = 'NVIDIA Corporation'; Licence = 'LICENSE-NVIDIA.txt'; Fg = $true }
        @{ Src = Join-Path $refs 'Streamline\bin\x64\sl.common.dll';               Signer = 'NVIDIA Corporation'; Licence = 'LICENSE-NVIDIA.txt'; Fg = $true }
        @{ Src = Join-Path $refs 'Streamline\bin\x64\sl.dlss.dll';                 Signer = 'NVIDIA Corporation'; Licence = 'LICENSE-NVIDIA.txt'; Fg = $true }
        @{ Src = Join-Path $refs 'Streamline\bin\x64\sl.dlss_g.dll';               Signer = 'NVIDIA Corporation'; Licence = 'LICENSE-NVIDIA.txt'; Fg = $true }
        @{ Src = Join-Path $refs 'Streamline\bin\x64\sl.reflex.dll';               Signer = 'NVIDIA Corporation'; Licence = 'LICENSE-NVIDIA.txt'; Fg = $true }
        @{ Src = Join-Path $refs 'Streamline\bin\x64\sl.pcl.dll';                  Signer = 'NVIDIA Corporation'; Licence = 'LICENSE-NVIDIA.txt'; Fg = $true }
        @{ Src = Join-Path $root 'LICENSE-NVIDIA.txt';                             Licence = 'LICENSE-NVIDIA.txt' }
    )
    'AMD' = @(
        @{ Src = Join-Path $refs 'FidelityFX-SDK\Kits\FidelityFX\signedbin\amd_fidelityfx_loader_dx12.dll';         Signer = 'Advanced Micro Devices'; Licence = 'LICENSE-AMD.txt' }
        @{ Src = Join-Path $refs 'FidelityFX-SDK\Kits\FidelityFX\signedbin\amd_fidelityfx_upscaler_dx12.dll';       Signer = 'Advanced Micro Devices'; Licence = 'LICENSE-AMD.txt' }
        @{ Src = Join-Path $refs 'FidelityFX-SDK\Kits\FidelityFX\signedbin\amd_fidelityfx_framegeneration_dx12.dll'; Signer = 'Advanced Micro Devices'; Licence = 'LICENSE-AMD.txt'; Fg = $true }
        @{ Src = Join-Path $root 'LICENSE-AMD.txt';                                                                  Licence = 'LICENSE-AMD.txt' }
    )
    'Intel' = @(
        @{ Src = Join-Path $refs 'XeSS-sdk\bin\libxess.dll';    Signer = 'Intel Corporation'; Licence = 'LICENSE-INTEL.txt' }
        @{ Src = Join-Path $refs 'XeSS-sdk\bin\libxess_fg.dll'; Signer = 'Intel Corporation'; Licence = 'LICENSE-INTEL.txt'; Fg = $true }
        @{ Src = Join-Path $refs 'XeSS-sdk\bin\libxell.dll';    Signer = 'Intel Corporation'; Licence = 'LICENSE-INTEL.txt'; Fg = $true }
        @{ Src = Join-Path $root 'LICENSE-INTEL.txt';           Licence = 'LICENSE-INTEL.txt' }
    )
}

# --- Build unless told not to -------------------------------------------------------------
if (-not $SkipBuild -and -not $ValidateOnly) {
    & (Join-Path $root 'build-native.ps1')
    if ($LASTEXITCODE -ne 0) { throw "build-native.ps1 failed (exit $LASTEXITCODE)." }
    dotnet build (Join-Path $root 'Renderforge.csproj') -c $Configuration /p:PPRoot="$PPRoot"
    if ($LASTEXITCODE -ne 0) { throw "dotnet build failed (exit $LASTEXITCODE)." }
}

# --- Validate every source file ------------------------------------------------------------
Write-Host "Renderforge $version - validating sources (WithFrameGen=$WithFrameGen)"
$selected = [ordered]@{}
foreach ($pack in $packs.Keys) {
    $files = @()
    foreach ($f in $packs[$pack]) {
        if ($f.Fg -and -not $WithFrameGen) { continue }
        if (-not (Test-Path $f.Src)) { throw "$pack pack: missing source $($f.Src)" }
        $name = Split-Path $f.Src -Leaf
        $ver  = Get-NormalVersion $f.Src
        if ($f.Signer) { Assert-VendorSignature $f.Src $f.Signer }
        if ($NewestKnownNgx.ContainsKey($name) -and $ver -ne $NewestKnownNgx[$name]) {
            Write-Warning ('{0} is {1}; newest known is {2}. Check the TechPowerUp DLL database and refresh refs\ before releasing, or update $NewestKnownNgx in this script.' -f $name, $ver, $NewestKnownNgx[$name])
        }
        $files += [pscustomobject]@{
            Name    = $name
            Src     = (Resolve-Path $f.Src).Path
            Version = $ver
            Bytes   = (Get-Item $f.Src).Length
            Sha256  = (Get-FileHash $f.Src -Algorithm SHA256).Hash.ToLowerInvariant()
            Signer  = if ($f.Signer) { $f.Signer } else { $null }
            Licence = if ($f.Licence) { $f.Licence } else { $null }
        }
        Write-Host ("  {0,-8} {1,-42} {2,-14} {3,12:N0} bytes" -f $pack, $name, $ver, (Get-Item $f.Src).Length)
    }
    $selected[$pack] = $files
}
Write-Host "validate: OK"
if ($ValidateOnly) { return }

