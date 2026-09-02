# Phase 6 — Packaging, licences, README matrix, GitHub + Workshop release — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the multi-vendor build into shippable artefacts — `build\release.ps1` producing `Renderforge-Core-<v>.zip`, `-NVIDIA-`, `-AMD-`, `-Intel-`, `-Full-`, a `manifest-<pack>.json` inside every zip and `SHA256SUMS.txt` beside them, every vendor DLL Authenticode-asserted and version-pinned — plus the rewritten README install matrix, `docs\RELEASING.md`, and the (user-gated) GitHub release and Steam Workshop item.

**Architecture:** One PowerShell script owns packaging. It reads the version from `meta.json`, walks an ordered pack table (`Core`, `NVIDIA`, `AMD`, `Intel`), asserts each vendor DLL's Authenticode signer subject and warns when an NVIDIA NGX DLL's FileVersion differs from a pinned "newest known" constant, stages each pack into `build\release\stage\<Pack>\Renderforge\`, and zips that folder with `includeBaseDirectory = $true` so **every** zip carries exactly one top-level `Renderforge/` folder. Extracting any zip — or several of them, in any order — into `<Phoenix Point>\Mods\` therefore merges into a single `Mods\Renderforge\`. `Full` is the union of all four staged folders. The managed mod already treats vendor DLLs as optional at runtime (`Upscalers.FsrDllsPresent`); this phase extends that so every missing-DLL reason names the pack to install.

**Tech Stack:** PowerShell 7 (`build\release.ps1`, `build-native.ps1`, `deploy.ps1`), .NET `System.IO.Compression.ZipFile` / `Get-FileHash` / `Get-AuthenticodeSignature`, C# `net472` (`Renderforge.csproj`), `gh` CLI for the GitHub release, PerkOracle's `workshop/steamugc/publish_ugc.py` (SteamworksPy) for the Workshop item.

**Depends on:** Phase 2 (DLSS on D3D12, shipped), Phase 3 (FSR, `build\out\amd_fidelityfx_*_dx12.dll` staged by `build-native.ps1`). Phase 4 (XeSS) and Phase 5 (frame generation) are **not** prerequisites: the Intel pack's `libxess.dll` is packed from `refs\XeSS-sdk\bin\` regardless of whether the shim uses it yet, and every frame-generation DLL sits behind the `-WithFrameGen` switch which stays **off** until Phase 5 lands.

---

## Grounding — facts this plan rests on (all read from disk, 2026-09-02)

### What exists today

- `E:\DEV\PhoenixPoint\Renderforge\` is its own inner repo (`UberMorgott/PhoenixPoint-Mod-Renderforge`, branch `main`). `gh release list` → one entry: `Renderforge 1.0.0  Latest  v1.0.0  2026-09-01T23:01:14Z`.
- The shipped `build\release\Renderforge-1.0.0.zip` (45 428 704 bytes) has **flat** entries — `LICENSE`, `LICENSE-NIS.txt`, `LICENSE-NVIDIA.txt`, `meta.json`, `nvngx_dlss.dll`, `README.md`, `Renderforge.dll`, `RenderforgeNative.dll` — i.e. it must be extracted *into* `Mods\Renderforge\`. **This plan changes that**: from 1.1.0 every zip carries a `Renderforge/` prefix and is extracted into `Mods\`, which is the only layout under which overlaying a vendor pack on top of Core can work. The README install section changes with it (Task 5).
- `.gitignore` (repo root) currently: `bin/`, `obj/`, `build/`, `*.user`, `.vs/`, `RenderforgeNative.dll`, `nvngx_dlss.dll`, `*.pdb`, `Console.log`, `.serena/`. `build/` ignores the whole directory, so a file placed at `build\release.ps1` would be untracked and a `!build/release.ps1` negation alone cannot rescue it (git never descends into an excluded directory). Task 1 replaces the line with `build/*` + a negation, which does work. Binaries stay ignored.
- Licence files present at repo root and tracked: `LICENSE` (MIT, 1064 B), `LICENSE-AMD.txt`, `LICENSE-NIS.txt`, `LICENSE-NVIDIA.txt`. **`LICENSE-INTEL.txt` does not exist yet** — Task 1 creates it from the XeSS SDK.
- `meta.json` → `"Version": "1.0.0"`; `Renderforge.csproj` → `<Version>`/`<AssemblyVersion>`/`<FileVersion>` = `1.0.0.0`. Managed output lands in `bin\$(Configuration)\Renderforge\Renderforge.dll` (the csproj pins `OutputPath` to that folder name because PPModLoader loads `<FolderName>\<FolderName>.dll`).
- `build-native.ps1` already stages into `build\out\`: `RenderforgeNative.dll`, `nvngx_dlss.dll`, `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`, `dlss_probe.exe`, plus NGX log files. It already asserts `NVIDIA Corporation` on the NGX DLL and `Advanced Micro Devices` on the two AMD DLLs.

### Vendor binaries on disk — signature and FileVersion, measured

`Get-AuthenticodeSignature` + `VersionInfo.FileVersion`, run 2026-09-02:

| File | Path (under `E:\DEV\PhoenixPoint\refs\`) | Status | FileVersion | Signer CN |
|---|---|---|---|---|
| `nvngx_dlss.dll` | `DLSS-sdk\lib\Windows_x86_64\rel\` | Valid | `310,7,129,0` | NVIDIA Corporation |
| `nvngx_dlssg.dll` | `Streamline\latest-dll\` | Valid | `310,7,129,0` | NVIDIA Corporation |
| `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.dlss.dll`, `sl.reflex.dll`, `sl.pcl.dll` | `Streamline\bin\x64\` | Valid | `2,12,0,0` | NVIDIA Corporation |
| `amd_fidelityfx_loader_dx12.dll` | `FidelityFX-SDK\Kits\FidelityFX\signedbin\` | Valid | `2.3.0.2740` | Advanced Micro Devices |
| `amd_fidelityfx_upscaler_dx12.dll` | same | Valid | `4.1.1.2740` | Advanced Micro Devices |
| `amd_fidelityfx_framegeneration_dx12.dll` | same | Valid | `4.0.1.2740` | Advanced Micro Devices |
| `libxess.dll` | `XeSS-sdk\bin\` | Valid | `2.0.2.68` | Intel Corporation |
| `libxess_fg.dll` | `XeSS-sdk\bin\` | Valid | `1.3.1.78` | Intel Corporation |
| `libxell.dll` | `XeSS-sdk\bin\` | Valid | `1.3.2.10` | Intel Corporation |

**Trap:** NVIDIA DLLs report FileVersion with **commas** (`310,7,129,0`), AMD/Intel with dots. Every version comparison in `release.ps1` therefore normalises `-replace '[ ,]', '.'` on both sides. Getting this wrong makes the stale-DLL guard fire on every build.

### Availability (`src\Availability.cs`) — what the missing-DLL path looks like now

- `internal enum Feature { Dlss, Fsr, Xess, FrameGen }`; `Reason(f) == null` means available. Strings go through `DlssConfig.Loc("<en>", "<ru>")`.
- `Feature.Fsr` already has `if (!Upscalers.FsrDllsPresent) return DlssConfig.Loc("DLL missing: amd_fidelityfx_upscaler_dx12.dll", "Нет файла: amd_fidelityfx_upscaler_dx12.dll");` — Task 4 appends the pack hint and adds the DLSS + XeSS equivalents.
- `Upscalers.FsrDllsPresent` (`src\Upscaler.cs:81-95`) is the pattern to copy: cached tri-state `int`, `RenderforgeMod.ModDir ?? "."`, `File.Exists(Path.Combine(dir, name))`. `src\Upscaler.cs` already has `using System.IO;` at line 1 and the private field `private static int fsrDlls;` at line 97.
- `RenderforgeMod.ModDir` is `ModEntry.Directory`; it is the mod folder, i.e. where the vendor DLLs are extracted to.

### Workshop publishing (reused from PerkOracle, `E:\DEV\PhoenixPoint\PerkOracle\docs\OPERATIONS.md`)

- Publisher: `PerkOracle\workshop\steamugc\publish_ugc.py`, SteamworksPy, appid **839770**, owner SteamID64 **76561197996210591**. Modes: `--create` ("Create a brand-new Workshop item, then upload content"), `--update`, `--localize-descriptions`; flags `--item`, `--changenote`, `--visibility`, `--tags`, `--gallery`.
- Module constants that must be pointed at Renderforge for a Renderforge publish: `CONTENT_FOLDER` (= `<workshop>/Dist`), `PREVIEW_FILE` (= `<repo>/image/steam_preview.jpg`), `TITLE`, `WORKSHOP_TAGS`. Valid Phoenix Point tags: Geoscape, Tactical, Difficulty, Gameplay, Overhaul, UI, Utility, Cheat, Localization.
- Hard prerequisites: the Steam client must be running and logged in as the owner; native deps in `workshop/steamugc/` are git-ignored and must be present; `steam_appid.txt` must contain `839770`; never report a publish done before `EResult.OK`.
- Renderforge has **no `publishedfileid` yet** — Task 8 creates the item and records the id.

---

## File structure

| File | Created / Modified | Responsibility |
|---|---|---|
| `E:\DEV\PhoenixPoint\Renderforge\.gitignore` | Modify | Track `build\release.ps1` while every binary under `build\` stays ignored |
| `E:\DEV\PhoenixPoint\Renderforge\LICENSE-INTEL.txt` | Create | Intel Simplified Software Licence + third-party notices shipped with `libxess*.dll` / `libxell.dll` |
| `E:\DEV\PhoenixPoint\Renderforge\meta.json` | Modify | Version `1.0.0` → `1.1.0` (single source of truth for the zip names) |
| `E:\DEV\PhoenixPoint\Renderforge\Renderforge.csproj` | Modify | Assembly version follows meta.json |
| `E:\DEV\PhoenixPoint\Renderforge\build\release.ps1` | Create | The whole packaging pipeline: validate → stage → zip → manifest → SHA256SUMS |
| `E:\DEV\PhoenixPoint\Renderforge\src\Upscaler.cs` | Modify | `NgxDllPresent`, `XessDllPresent` next to the existing `FsrDllsPresent` |
| `E:\DEV\PhoenixPoint\Renderforge\src\Availability.cs` | Modify | "DLL missing: `<name>` — install the `<Vendor>` pack" reasons (EN + RU) |
| `E:\DEV\PhoenixPoint\Renderforge\README.md` | Modify | Install matrix, renderer switch, per-feature table, licences, troubleshooting |
| `E:\DEV\PhoenixPoint\Renderforge\docs\DESIGN.md` | Modify | New `## Packaging` section + the Workshop id once it exists |
| `E:\DEV\PhoenixPoint\Renderforge\docs\RELEASING.md` | Create | The release checklist an operator follows |

---

### Task 1: Version bump, Intel licence, and a tracked `build\release.ps1` slot

**Files:**
- Modify: `E:\DEV\PhoenixPoint\Renderforge\.gitignore`
- Create: `E:\DEV\PhoenixPoint\Renderforge\LICENSE-INTEL.txt`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\meta.json`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\Renderforge.csproj:12-14`

- [ ] **Step 1: Prove that `build\release.ps1` would be ignored today**

Run:

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
git check-ignore -v build/release.ps1
```

Expected: `.gitignore:3:build/	build/release.ps1` (exit 0 — the file IS ignored).

- [ ] **Step 2: Replace the blanket `build/` ignore**

In `E:\DEV\PhoenixPoint\Renderforge\.gitignore`, replace the single line

```gitignore
build/
```

with

```gitignore
build/*
!build/release.ps1
```

Full resulting `.gitignore`:

```gitignore
bin/
obj/
build/*
!build/release.ps1
*.user
.vs/
RenderforgeNative.dll
nvngx_dlss.dll
*.pdb
Console.log
.serena/
```

- [ ] **Step 3: Verify the negation works and binaries stay ignored**

Run:

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
git check-ignore -v build/release.ps1; "exit=$LASTEXITCODE"
git check-ignore -v build/out/nvngx_dlss.dll
git check-ignore -v build/release/Renderforge-Core-1.1.0.zip
```

Expected: the first prints nothing and `exit=1` (no longer ignored); the second prints `.gitignore:8:nvngx_dlss.dll	build/out/nvngx_dlss.dll`; the third prints `.gitignore:3:build/*	build/release/Renderforge-Core-1.1.0.zip`.

- [ ] **Step 4: Create `LICENSE-INTEL.txt` from the XeSS SDK**

Run:

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
$sdk = 'E:\DEV\PhoenixPoint\refs\XeSS-sdk'
$header = @"
Intel(R) Xe Super Sampling (XeSS) SDK - licence and notices
===========================================================
Renderforge redistributes the unmodified Intel binaries libxess.dll, libxess_fg.dll and
libxell.dll under the licence reproduced below. Source: Intel XeSS SDK v3.0.2
(https://github.com/intel/xess). Intel and XeSS are trademarks of Intel Corporation.

-------------------------- LICENSE.txt --------------------------

"@
$parts = $header,
         (Get-Content (Join-Path $sdk 'LICENSE.txt') -Raw),
         "`r`n-------------------- third-party-programs.txt --------------------`r`n`r`n",
         (Get-Content (Join-Path $sdk 'third-party-programs.txt') -Raw)
Set-Content -Path 'LICENSE-INTEL.txt' -Value ($parts -join '') -Encoding utf8NoBOM
(Get-Item LICENSE-INTEL.txt).Length
```

Expected: a byte count printed, greater than 1000.

- [ ] **Step 5: Bump the version in `meta.json`**

Replace the `"Version"` line in `E:\DEV\PhoenixPoint\Renderforge\meta.json` so the file reads:

```json
{
  "ID": "com.morgott.Renderforge",
  "AssemblyName": "Renderforge.dll",
  "Version": "1.1.0",
  "Author": [
    { "Key": "English", "Value": "Morgott" }
  ],
  "Name": [
    { "Key": "English", "Value": "Renderforge" }
  ],
  "Description": [
    { "Key": "English", "Value": "DLSS / FSR / XeSS upscaling, DLAA, NIS sharpening, DirectX 12 renderer switch, frame-rate limit and a benchmark overlay for Phoenix Point." }
  ],
  "Dependencies": []
}
```

- [ ] **Step 6: Bump the assembly version**

In `E:\DEV\PhoenixPoint\Renderforge\Renderforge.csproj`, replace the three version lines

```xml
    <Version>1.0.0.0</Version>
    <AssemblyVersion>1.0.0.0</AssemblyVersion>
    <FileVersion>1.0.0.0</FileVersion>
```

with

```xml
    <Version>1.1.0.0</Version>
    <AssemblyVersion>1.1.0.0</AssemblyVersion>
    <FileVersion>1.1.0.0</FileVersion>
```

- [ ] **Step 7: Verify the version is readable the way `release.ps1` will read it**

Run:

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
(Get-Content meta.json -Raw | ConvertFrom-Json).Version
Select-String -Path Renderforge.csproj -Pattern '<FileVersion>'
```

Expected: `1.1.0`, then `    <FileVersion>1.1.0.0</FileVersion>`.

- [ ] **Step 8: Commit**

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
git add .gitignore LICENSE-INTEL.txt meta.json Renderforge.csproj
git commit -m "chore(release): bump to 1.1.0, add LICENSE-INTEL.txt, track build/release.ps1"
```

---

### Task 2: `build\release.ps1` — validation half (sources, Authenticode, stale-NVIDIA guard)

**Files:**
- Create: `E:\DEV\PhoenixPoint\Renderforge\build\release.ps1`

- [ ] **Step 1: Write the script (validation complete, packing added in Task 3)**

Create `E:\DEV\PhoenixPoint\Renderforge\build\release.ps1` with exactly this content:

```powershell
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
```

- [ ] **Step 2: Run the validation half and confirm it passes without frame generation**

Run:

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
.\build\release.ps1 -ValidateOnly
```

Expected: a table with `Core` (6 rows), `NVIDIA` (`nvngx_dlss.dll 310.7.129.0`, `LICENSE-NVIDIA.txt`), `AMD` (loader `2.3.0.2740`, upscaler `4.1.1.2740`, `LICENSE-AMD.txt`), `Intel` (`libxess.dll 2.0.2.68`, `LICENSE-INTEL.txt`), then `validate: OK`. No warnings.

If it fails on `bin\Release\Renderforge\Renderforge.dll` or `build\out\RenderforgeNative.dll`, build them once:

```powershell
.\deploy.ps1 -PPRoot 'D:\PP-Instance2' -AllowRunning
```

- [ ] **Step 3: Run the validation half WITH frame generation**

Run:

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
.\build\release.ps1 -ValidateOnly -WithFrameGen
```

Expected: additionally `nvngx_dlssg.dll 310.7.129.0`, six `sl.*.dll 2.12.0.0`, `amd_fidelityfx_framegeneration_dx12.dll 4.0.1.2740`, `libxess_fg.dll 1.3.1.78`, `libxell.dll 1.3.2.10`; still `validate: OK`, still no warnings.

- [ ] **Step 4: Prove the stale-NVIDIA guard actually fires**

Run (temporarily pins a version nothing on disk has):

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
$s = Get-Content build\release.ps1 -Raw
Set-Content build\release.ps1 ($s -replace "'nvngx_dlss.dll'  = '310.7.129.0'", "'nvngx_dlss.dll'  = '999.0.0.0'") -Encoding utf8NoBOM
.\build\release.ps1 -ValidateOnly
Set-Content build\release.ps1 $s -Encoding utf8NoBOM
```

Expected: `WARNING: nvngx_dlss.dll is 310.7.129.0; newest known is 999.0.0.0. ...` followed by `validate: OK` (a warning, not a failure). The last line restores the file — confirm with `git diff --stat build/release.ps1` printing nothing.

- [ ] **Step 5: Prove the Authenticode assertion actually fires**

Run (feeds an unsigned file through the same check):

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
pwsh -NoProfile -Command {
  . { function Assert-VendorSignature([string] $path, [string] $signer) {
        $sig = Get-AuthenticodeSignature $path
        if ($sig.Status -ne 'Valid') { throw "Authenticode: $path is not Valid (status $($sig.Status)). Refusing to ship it." }
        if ($sig.SignerCertificate.Subject -notmatch [regex]::Escape($signer)) { throw "Authenticode: $path is signed by '$($sig.SignerCertificate.Subject)', expected '$signer'." }
      } }
  try { Assert-VendorSignature 'E:\DEV\PhoenixPoint\Renderforge\build\out\RenderforgeNative.dll' 'NVIDIA Corporation' }
  catch { "CAUGHT: $($_.Exception.Message)" }
}
```

Expected: `CAUGHT: Authenticode: ...\RenderforgeNative.dll is not Valid (status NotSigned). Refusing to ship it.`

- [ ] **Step 6: Commit**

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
git add build/release.ps1
git commit -m "build(release): validate release sources - Authenticode + NGX version pins"
```

---

### Task 3: `build\release.ps1` — packing half (staging, zips, manifests, SHA256SUMS)

**Files:**
- Modify: `E:\DEV\PhoenixPoint\Renderforge\build\release.ps1` (append after `if ($ValidateOnly) { return }`)

- [ ] **Step 1: Append the packing half**

Append this to the end of `E:\DEV\PhoenixPoint\Renderforge\build\release.ps1`:

```powershell

# --- Stage ----------------------------------------------------------------------------------
# stage\<Pack>\Renderforge\ is zipped with includeBaseDirectory=$true, which names the single
# top-level entry after the folder - hence the fixed "Renderforge" leaf.
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage, $relDir | Out-Null

$full = @()
foreach ($pack in $selected.Keys) {
    $dir = Join-Path $stage "$pack\Renderforge"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    foreach ($f in $selected[$pack]) { Copy-Item $f.Src (Join-Path $dir $f.Name) -Force }
    $full += $selected[$pack]
}
$fullDir = Join-Path $stage 'Full\Renderforge'
New-Item -ItemType Directory -Force -Path $fullDir | Out-Null
foreach ($f in $full) { Copy-Item $f.Src (Join-Path $fullDir $f.Name) -Force }
$selected['Full'] = $full

# --- Manifest + zip per pack -----------------------------------------------------------------
$stamp = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
$zips = @()
foreach ($pack in $selected.Keys) {
    $dir      = Join-Path $stage "$pack\Renderforge"
    $manifest = [ordered]@{
        mod           = 'Renderforge'
        modId         = 'com.morgott.Renderforge'
        pack          = $pack
        version       = $version
        frameGen      = [bool] $WithFrameGen
        generatedUtc  = $stamp
        extractInto   = 'Mods\'
        files         = @($selected[$pack] | ForEach-Object {
            [ordered]@{
                name        = $_.Name
                fileVersion = $_.Version
                bytes       = $_.Bytes
                sha256      = $_.Sha256
                signer      = $_.Signer
                licence     = $_.Licence
            }
        })
    }
    # One manifest name per pack: a vendor zip is extracted ON TOP of Core, so a shared
    # "manifest.json" would silently overwrite the record of what else is installed.
    $manifest | ConvertTo-Json -Depth 5 |
        Set-Content -Path (Join-Path $dir ("manifest-" + $pack.ToLowerInvariant() + ".json")) -Encoding utf8NoBOM

    $zip = Join-Path $relDir "Renderforge-$pack-$version.zip"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    [IO.Compression.ZipFile]::CreateFromDirectory($dir, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
    $zips += Get-Item $zip
    Write-Host ("packed {0,-38} {1,14:N0} bytes" -f (Split-Path $zip -Leaf), (Get-Item $zip).Length)
}

# --- SHA256SUMS.txt ---------------------------------------------------------------------------
$sums = Join-Path $relDir 'SHA256SUMS.txt'
$zips | ForEach-Object { "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name } |
    Set-Content -Path $sums -Encoding utf8NoBOM
Write-Host "wrote $sums"
Write-Host "release: OK - $relDir"
```

- [ ] **Step 2: Produce the release set**

Run:

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
.\build\release.ps1 -SkipBuild
```

Expected tail:

```
packed Renderforge-Core-1.1.0.zip                    ... bytes
packed Renderforge-NVIDIA-1.1.0.zip                  ... bytes
packed Renderforge-AMD-1.1.0.zip                     ... bytes
packed Renderforge-Intel-1.1.0.zip                   ... bytes
packed Renderforge-Full-1.1.0.zip                    ... bytes
wrote E:\DEV\PhoenixPoint\Renderforge\build\release\SHA256SUMS.txt
release: OK - E:\DEV\PhoenixPoint\Renderforge\build\release
```

- [ ] **Step 3: Verify every zip has exactly one top-level `Renderforge/` folder**

Run:

```powershell
cd E:\DEV\PhoenixPoint\Renderforge\build\release
foreach ($z in Get-ChildItem *-1.1.0.zip) {
    $a = [IO.Compression.ZipFile]::OpenRead($z.FullName)
    $tops = $a.Entries.FullName | ForEach-Object { ($_ -split '/')[0] } | Sort-Object -Unique
    "{0,-34} roots=[{1}] entries={2}" -f $z.Name, ($tops -join ','), $a.Entries.Count
    $a.Dispose()
}
```

Expected: five lines, every one `roots=[Renderforge]`. Entry counts: Core 7, NVIDIA 3, AMD 4, Intel 3, Full 14 (each = its files + its own `manifest-<pack>.json`; the
folder itself is not an entry).

- [ ] **Step 4: Verify the overlay actually merges into one folder**

Run:

```powershell
$t = "C:\Temp\claude\rf-overlay"
Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $t | Out-Null
cd E:\DEV\PhoenixPoint\Renderforge\build\release
foreach ($n in 'Core','NVIDIA','AMD','Intel') {
    [IO.Compression.ZipFile]::ExtractToDirectory("Renderforge-$n-1.1.0.zip", $t, $true)
}
Get-ChildItem $t -Directory | Select-Object -ExpandProperty Name
Get-ChildItem "$t\Renderforge" -File | Select-Object Name
```

Expected: exactly one directory, `Renderforge`, containing `LICENSE`, `LICENSE-AMD.txt`, `LICENSE-INTEL.txt`, `LICENSE-NIS.txt`, `LICENSE-NVIDIA.txt`, `README.md`, `Renderforge.dll`, `RenderforgeNative.dll`, `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`, `libxess.dll`, `manifest-amd.json`, `manifest-core.json`, `manifest-intel.json`, `manifest-nvidia.json`, `meta.json`, `nvngx_dlss.dll` — 17 files.

- [ ] **Step 5: Verify a manifest and the checksums**

Run:

```powershell
cd E:\DEV\PhoenixPoint\Renderforge\build\release
Get-Content "C:\Temp\claude\rf-overlay\Renderforge\manifest-nvidia.json"
Get-Content SHA256SUMS.txt
(Get-FileHash Renderforge-Core-1.1.0.zip -Algorithm SHA256).Hash.ToLowerInvariant()
```

Expected: the NVIDIA manifest lists `nvngx_dlss.dll` with `"fileVersion": "310.7.129.0"`, `"signer": "NVIDIA Corporation"`, `"licence": "LICENSE-NVIDIA.txt"` and a 64-hex `sha256`; `SHA256SUMS.txt` has five `<hash>  <zipname>` lines; the printed Core hash matches its line.

- [ ] **Step 6: Clean up the scratch extraction**

Run:

```powershell
Remove-Item "C:\Temp\claude\rf-overlay" -Recurse -Force
```

Expected: no output.

- [ ] **Step 7: Commit**

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
git add build/release.ps1
git commit -m "build(release): per-vendor zips, manifests and SHA256SUMS"
```

---

### Task 4: The mod names the missing vendor pack

**Files:**
- Modify: `E:\DEV\PhoenixPoint\Renderforge\src\Upscaler.cs:81-97`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\src\Availability.cs:40-65`

- [ ] **Step 1: Add the two missing DLL-presence probes**

In `E:\DEV\PhoenixPoint\Renderforge\src\Upscaler.cs`, replace lines 81-97 (the `FsrDllsPresent` property and the `fsrDlls` field) with:

```csharp
        /// <summary>Both AMD DLLs present next to the mod? Cheap, cached — it is asked on every options repaint.</summary>
        internal static bool FsrDllsPresent
        {
            get
            {
                if (fsrDlls == 0)
                {
                    string dir = RenderforgeMod.ModDir ?? ".";
                    bool ok = File.Exists(Path.Combine(dir, "amd_fidelityfx_loader_dx12.dll"))
                           && File.Exists(Path.Combine(dir, "amd_fidelityfx_upscaler_dx12.dll"));
                    fsrDlls = ok ? 1 : -1;
                }
                return fsrDlls == 1;
            }
        }

        /// <summary>nvngx_dlss.dll next to the mod? Absent = the NVIDIA pack was never extracted.</summary>
        internal static bool NgxDllPresent
        {
            get
            {
                if (ngxDll == 0)
                    ngxDll = File.Exists(Path.Combine(RenderforgeMod.ModDir ?? ".", "nvngx_dlss.dll")) ? 1 : -1;
                return ngxDll == 1;
            }
        }

        /// <summary>libxess.dll next to the mod? Absent = the Intel pack was never extracted.</summary>
        internal static bool XessDllPresent
        {
            get
            {
                if (xessDll == 0)
                    xessDll = File.Exists(Path.Combine(RenderforgeMod.ModDir ?? ".", "libxess.dll")) ? 1 : -1;
                return xessDll == 1;
            }
        }

        private static int fsrDlls;
        private static int ngxDll;
        private static int xessDll;
```

- [ ] **Step 2: Name the pack in every missing-DLL reason**

In `E:\DEV\PhoenixPoint\Renderforge\src\Availability.cs`, replace the whole `switch (feature)` body (lines 38-68) with:

```csharp
            switch (feature)
            {
                case Feature.Dlss:
                    if (!IsD3D11 && !IsD3D12)
                        return DlssConfig.Loc("Requires DirectX 11 or DirectX 12", "Требуется DirectX 11 или DirectX 12");
                    if (!IsNvidia)
                        return DlssConfig.Loc("Requires an NVIDIA RTX GPU", "Требуется видеокарта NVIDIA RTX");
                    if (RenderforgeMod.Available) return null;
                    if (NeedsRestart) return RestartReason;
                    if (!Upscalers.NgxDllPresent)
                        return DlssConfig.Loc("DLL missing: nvngx_dlss.dll — install the NVIDIA pack",
                                              "Нет файла: nvngx_dlss.dll — установите пакет NVIDIA");
                    return RenderforgeMod.InitCode == Native.DLSS_ERR_NOT_AVAILABLE   // NVIDIA without tensor cores (GTX)
                        ? DlssConfig.Loc("Requires an NVIDIA RTX GPU", "Требуется NVIDIA RTX")
                        : DlssConfig.Loc("DLSS init failed — see the log", "Не удалось инициализировать DLSS — смотрите лог");
                case Feature.Fsr:
                    if (!IsD3D12)
                        return DlssConfig.Loc("Requires DirectX 12 — switch Renderer", "Требуется DirectX 12 — переключите рендерер");
                    if (NeedsRestart) return RestartReason;
                    if (!Upscalers.FsrDllsPresent)
                        return DlssConfig.Loc("DLL missing: amd_fidelityfx_upscaler_dx12.dll — install the AMD pack",
                                              "Нет файла: amd_fidelityfx_upscaler_dx12.dll — установите пакет AMD");
                    if (Upscalers.Running == UpscalerKind.FSR && !RenderforgeMod.Available)
                        return DlssConfig.Loc("FSR init failed — see the log", "Не удалось инициализировать FSR — смотрите лог");
                    return null;
                case Feature.Xess:
                    if (!IsD3D12)
                        return DlssConfig.Loc("Requires DirectX 12 — switch Renderer", "Требуется DirectX 12 — переключите рендерер");
                    if (NeedsRestart) return RestartReason;
                    if (!Upscalers.XessDllPresent)
                        return DlssConfig.Loc("DLL missing: libxess.dll — install the Intel pack",
                                              "Нет файла: libxess.dll — установите пакет Intel");
                    return DlssConfig.Loc("Not implemented yet", "Пока не реализовано");
                case Feature.FrameGen:
                    return NeedsRestart ? RestartReason
                        : IsD3D12
                        ? DlssConfig.Loc("Not implemented yet", "Пока не реализовано")
                        : DlssConfig.Loc("Requires DirectX 12 — switch Renderer", "Требуется DirectX 12 — переключите рендерер");
                default:
                    return null;
            }
```

- [ ] **Step 3: Build the managed mod**

Run:

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
dotnet build Renderforge.csproj -c Release /p:PPRoot="D:\PP-Instance2"
```

Expected: `Build succeeded.` with `0 Error(s)`.

- [ ] **Step 4: Verify the strings in the compiled assembly**

Run:

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
$b = [IO.File]::ReadAllBytes('bin\Release\Renderforge\Renderforge.dll')
$s = [Text.Encoding]::Unicode.GetString($b)
foreach ($needle in 'install the NVIDIA pack', 'install the AMD pack', 'install the Intel pack',
                    'установите пакет NVIDIA', 'установите пакет AMD', 'установите пакет Intel') {
    "{0,-26} {1}" -f $needle, $s.Contains($needle)
}
```

Expected: all six lines end in `True`.

- [ ] **Step 5: Verify in-game that a missing pack greys the row with the right tooltip**

Run (Instance2, which has the AMD DLLs deployed — move them aside, launch, look, put them back):

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
.\deploy.ps1 -PPRoot 'D:\PP-Instance2'
$m = 'D:\PP-Instance2\Mods\Renderforge'
Move-Item "$m\amd_fidelityfx_upscaler_dx12.dll" "$m\_amd_upscaler.bak" -Force
cd E:\DEV\PhoenixPoint\PPCLI
.\ppcli.ps1 run '{"args":["-force-d3d12","-mods"]}'
.\ppcli.ps1 connect screenshot '{"path":"E:\\DEV\\PhoenixPoint\\Renderforge\\docs\\shots\\pack-missing-tooltip.png"}'
```

Then open Options → Graphics, hover the greyed `FSR` value in the UPSCALER row, take the screenshot, and confirm the tooltip reads `DLL missing: amd_fidelityfx_upscaler_dx12.dll — install the AMD pack`. Restore:

```powershell
$m = 'D:\PP-Instance2\Mods\Renderforge'
Move-Item "$m\_amd_upscaler.bak" "$m\amd_fidelityfx_upscaler_dx12.dll" -Force
```

Expected: the screenshot file exists and shows that tooltip; the restore leaves the folder as `deploy.ps1` left it (`Get-ChildItem $m -Filter amd_*` lists both AMD DLLs, no `.bak`).

- [ ] **Step 6: Commit**

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
git add src/Upscaler.cs src/Availability.cs docs/shots/pack-missing-tooltip.png
git commit -m "feat(availability): missing vendor DLL names the pack to install"
```

---

### Task 5: README rewrite — install matrix, feature table, licences, troubleshooting

**Files:**
- Modify: `E:\DEV\PhoenixPoint\Renderforge\README.md` (full replacement)

- [ ] **Step 1: Replace `README.md` with this content**

```markdown
# Renderforge

Upscaling, anti-aliasing, sharpening, a DirectX 12 renderer switch, frame-rate control and a
benchmark overlay for Phoenix Point.

## Features

- **NVIDIA DLSS Super Resolution and DLAA** (DLSS SDK 310.7, shipped runtime `nvngx_dlss.dll`
  310.7.129, NVIDIA-signed, transformer model) on DirectX 11 **and** DirectX 12.
- **AMD FidelityFX Super Resolution** — one signed AMD runtime that carries FSR 4.1.1 (ML, RDNA 3/4)
  and the FSR 3.1.5 fallback for every other GPU. DirectX 12 only.
- **Intel XeSS** 2.0.2 (XeSS SDK 3.0.2), cross-vendor. DirectX 12 only.
- **Renderer switch** between DirectX 11 and DirectX 12 from inside the options screen.
- Quality modes per provider: Off, Auto, DLAA / Native AA, Quality, Balanced, Performance,
  Ultra Performance.
- Auto picks by output height — 1200p or lower: DLAA; 1201p–1600p: Quality; above 1600p: Performance.
- Change modes while playing; only the **renderer** and the **upscaler vendor** need a restart.
- Native pickers in **Options → Graphics** (RENDERER, UPSCALER, FRAME GENERATION, QUALITY, SHARPNESS).
- Anything that cannot run right now is greyed out and says why when you hover it.
- Automatically disables SMAA while an upscaler is active.
- Automatic texture mip bias in upscaling modes: `log2(render resolution / output resolution)`.
- NVIDIA Image Scaling sharpen-only pass, 0–100.
- Removes Phoenix Point's fixed 60 FPS cap; optional 30–300 FPS limit, mutually exclusive with VSync.
- Benchmark overlay: renderer, upscaler, mode, render → output resolution, anti-aliasing, FPS, frame time.
- Stays dormant and hides its UI when nothing it offers can run.

Renderforge affects the main tactical and geoscape camera. The interface stays at native output resolution.

## Requirements

- Windows
- Phoenix Point for Steam; other stores are currently untested
- For DLSS: an NVIDIA GeForce RTX 20-series or newer, on a recent Game Ready driver
- For FSR and XeSS: DirectX 12 (`Renderer` → `DirectX 12`), any modern GPU
- FSR 4.1.1's ML path additionally needs a Radeon RX 7000/9000-class GPU; everything else falls back
  to FSR 3.1.5 automatically

## Install — which zip do I need?

Every zip extracts into `<Phoenix Point>\Mods\` and contains a single `Renderforge` folder, so you can
extract **Core plus any vendor packs** on top of each other in any order.

| Your GPU | Download |
|---|---|
| NVIDIA GeForce RTX | `Renderforge-Core-<v>.zip` + `Renderforge-NVIDIA-<v>.zip` |
| AMD Radeon | `Renderforge-Core-<v>.zip` + `Renderforge-AMD-<v>.zip` |
| Intel Arc | `Renderforge-Core-<v>.zip` + `Renderforge-Intel-<v>.zip` |
| Anything else, or you want them all | `Renderforge-Full-<v>.zip` (Core + all three vendors) |

`Renderforge-Core-<v>.zip` on its own still gives you the renderer switch, the FPS limit, the mip bias
and the overlay — it just has no upscaler to run. A vendor pack on its own does nothing without Core.
Checksums for every asset are in `SHA256SUMS.txt` on the release page, and each zip carries a
`manifest-<pack>.json` listing every DLL's version, SHA-256 and licence.

### Steam Workshop

1. [Subscribe on Steam Workshop](STEAM_WORKSHOP_URL) — the Workshop item is the **Full** bundle.
2. Start Phoenix Point.
3. Open **Main menu → Mods** and enable **Renderforge**.
4. Restart the game once.

### Manual installation

1. Download the zips from the [releases page](https://github.com/UberMorgott/PhoenixPoint-Mod-Renderforge/releases).
2. Extract each of them into:

   ```text
   <Phoenix Point>\Mods\
   ```

3. Confirm the result, e.g. for Core + NVIDIA:

   ```text
   Mods\Renderforge\
     Renderforge.dll
     RenderforgeNative.dll
     nvngx_dlss.dll
     meta.json
     README.md
     LICENSE
     LICENSE-NIS.txt
     LICENSE-NVIDIA.txt
     manifest-core.json
     manifest-nvidia.json
   ```

4. Start Phoenix Point.
5. Open **Main menu → Mods** and enable **Renderforge**.
6. Restart the game once.

Do not add an extra nested `Renderforge` directory. On first launch the mod copies
`RenderforgeNative.dll` into `PhoenixPointWin64_Data\Plugins\x86_64\` — Unity only wires its DirectX 12
interface for plugins loaded from there — which is why that one restart is needed.

## What runs where

| Feature | DirectX 11 | DirectX 12 | Pack |
|---|---|---|---|
| DLSS Super Resolution / DLAA | yes | yes | NVIDIA |
| FSR 4.1.1 (ML) / 3.1.5 | no | yes | AMD |
| XeSS 2.0.2 | no | yes | Intel |
| NVIDIA Image Scaling sharpening | yes | yes | Core |
| Texture mip bias | yes | yes | Core |
| Frame-rate limit, uncapped FPS, overlay | yes | yes | Core |
| DLSS Frame Generation (Streamline) | no | planned | NVIDIA |
| FSR Frame Generation | no | planned | AMD |
| XeSS Frame Generation | no | planned | Intel |
| Ray Reconstruction | no | no | — |

"planned" = the FRAME GENERATION row is present but greyed with `Not implemented yet`.

## Settings

### Renderer

`Renderer` (Options → Graphics, or Mods → Renderforge) picks the graphics API: `DirectX 11` (default)
or `DirectX 12 (experimental)`. FSR, XeSS and frame generation need DirectX 12 — on DirectX 11 those
entries stay greyed and say so when you hover them.

Changing it needs a restart, because Unity picks the API from the command line. Press APPLY and answer
`Yes`: the game closes and relaunches itself with `-force-d3d12` added to whatever it was started with.
Answer `No` and the row shows "(restart pending)" until you restart yourself. If you launch from Steam,
the mod offers the same restart once per session — or set it permanently in Steam → Library →
right-click Phoenix Point → Properties → Launch Options:

```
-force-d3d12 -mods
```

DirectX 12 is experimental. Ambient occlusion runs in SAO mode there and HDR colour grading uses a 2D
LUT (the game's 3D-LUT compute shader has no DirectX 12 build). To go back, set `Renderer` to
`DirectX 11` and restart (or remove `-force-d3d12` from your launch options). Report any crash with
your `Player.log`.

### Upscaler

`Upscaler` is `Auto` / `Off` / `DLSS` / `FSR` / `XeSS`. `Auto` resolves from your hardware: on
DirectX 11, DLSS on NVIDIA and nothing otherwise; on DirectX 12, DLSS on NVIDIA, else FSR when the AMD
pack is installed. The shim latches the vendor at start-up, so **changing the upscaler needs a restart**;
switching quality mode does not.

| Setting | Location | Default |
|---|---|---:|
| Renderer | Options → Graphics; Mods → Renderforge | Auto (= DirectX 11) |
| Upscaler | Options → Graphics; Mods → Renderforge | Auto |
| Quality mode | Options → Graphics; Mods → Renderforge | Auto |
| Frame generation | Options → Graphics; Mods → Renderforge | Off |
| Sharpness | Options → Graphics; Mods → Renderforge | 40 |
| Frame rate limit | Options → Screen; Mods → Renderforge | Off |
| Max FPS | Options → Screen; Mods → Renderforge | 60 |
| Show DLSS in Graphics options | Mods → Renderforge | On |
| DLSS on/off key | Mods → Renderforge | U |
| Overlay key | Mods → Renderforge | O |
| Show benchmark overlay | Mods → Renderforge | Off |
| Overlay position | Mods → Renderforge | Top Center |
| Overlay scale | Mods → Renderforge | 1.0 |
| Debug view | Mods → Renderforge | None |

The Sharpness control is disabled when the upscaler is Off. A value of 0 skips sharpening.

With **Frame rate limit** disabled the game is uncapped. Enabling the limit disables VSync; enabling
VSync disables the Renderforge limit. **Max FPS** accepts 30–300.

## Hotkeys

The modifiers are fixed; the letter can be changed under **Mods → Renderforge**.

| Action | Default hotkey |
|---|---|
| Toggle the upscaler Off/on | `Ctrl+Alt+U` |
| Show/hide benchmark overlay | `Ctrl+Alt+O` |

The toggle restores the last active mode. If the game starts Off with no previous mode remembered, it
restores Auto.

## Benchmarking tip

Press `Ctrl+Alt+O` for the overlay. It reports the renderer, the active upscaler and its version, the
mode, render → output resolution, the anti-aliasing method, average FPS and frame time. For useful
comparisons, stand in the same scene and camera position, let the reading settle, then capture Off,
DLAA and one upscaling mode at the same output resolution and graphics settings.

## Known limitations

- Bloom, depth of field, screen-space reflections and ambient occlusion run at the lower render
  resolution, before upscaling.
- Particles and shader-animated vegetation can ghost because they provide no reliable motion vectors.
- NVIDIA NGX writes several log files into `Mods\Renderforge`.
- DirectX 12 loses ACES 3D-LUT colour grading (2D LUT instead) and runs SAO instead of MSVO ambient
  occlusion.

## Troubleshooting

### "Native plugin staged — restart the game"

Expected once, after installing or updating. The mod copied `RenderforgeNative.dll` into
`PhoenixPointWin64_Data\Plugins\x86_64\`; Unity only loads it from there at start-up. Restart.

### A row is greyed and says "DLL missing: … — install the … pack"

You installed Core without that vendor's pack (or extracted the pack somewhere else). Download the
named pack from the releases page and extract it into `<Phoenix Point>\Mods\`, so its files land next
to `Renderforge.dll`.

### A row says "Requires DirectX 12 — switch Renderer"

FSR, XeSS and frame generation are DirectX 12 only. Set `Renderer` → `DirectX 12`, APPLY, restart.

### Renderforge is dormant

Confirm the mod is enabled, the game was restarted after installation, and that your GPU/driver can run
at least one of the packs you installed. For DLSS, check:

```text
<Phoenix Point>\Mods\Renderforge\nvsdk_ngx.log
```

Unsupported hardware or an outdated driver makes Renderforge log one availability message, hide its UI
and otherwise leave the game unchanged.

### Black screen after enabling an upscaler

Update to the latest Renderforge release and the latest GPU driver. You can also press `Ctrl+Alt+U` to
switch the upscaler off.

### FPS is still limited to 60

In **Options → Screen**, disable VSync and either disable **Frame rate limit** or set **Max FPS** above
60. External driver or overlay limiters may impose their own cap.

### The pickers are missing

Open **Mods → Renderforge** and enable **Show DLSS in Graphics options**. Also confirm the mod is
enabled and the game was restarted after installation. For a manual installation, verify that
`Renderforge.dll`, `RenderforgeNative.dll`, `meta.json` and at least one vendor DLL are directly inside
`Mods\Renderforge`.

### DirectX 12 crashes or looks wrong

DirectX 12 is experimental in Unity 2019.4. Switch `Renderer` back to `DirectX 11` and attach your
`Player.log` to a GitHub issue.

## Building from source

Requirements:

- Windows
- [.NET SDK 8](https://dotnet.microsoft.com/download/dotnet/8.0)
- Visual Studio 2022 Build Tools with the C++ x64 toolchain
- [CMake](https://cmake.org/)
- Phoenix Point installation
- Vendor SDKs beside the repo, in `..\refs\`: [NVIDIA DLSS](https://github.com/NVIDIA/DLSS) →
  `refs\DLSS-sdk`, [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) →
  `refs\FidelityFX-SDK`, [Intel XeSS](https://github.com/intel/xess) → `refs\XeSS-sdk`,
  [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline) → `refs\Streamline`

Vendor runtimes are not stored in this repository; the build copies them from `refs\` and verifies each
one's Authenticode signature first. NVIDIA Image Scaling headers are vendored under `native\nis\`.

```powershell
# managed component
dotnet build Renderforge.csproj -c Release /p:PPRoot="<Phoenix Point install>"

# native shim + its offline NGX/FSR probe
.\build-native.ps1

# build + deploy the complete bundle into <Phoenix Point install>\Mods\Renderforge
.\deploy.ps1 -PPRoot "<Phoenix Point install>"

# build the release zips into build\release\
.\build\release.ps1
```

`native\probe\dlss_probe.exe` is an offline self-test: it creates a D3D11 or D3D12 device and checks
NGX initialisation, DLSS feature creation, NIS sharpening and FSR context creation without starting
Phoenix Point.

Release procedure: `docs\RELEASING.md`.

Source repository: [UberMorgott/PhoenixPoint-Mod-Renderforge](https://github.com/UberMorgott/PhoenixPoint-Mod-Renderforge)

Mod ID: `com.morgott.Renderforge`

## Credits and licences

Renderforge is developed by Morgott.

- Renderforge code is licensed under the [MIT License](LICENSE).
- `nvngx_dlss.dll`, `nvngx_dlssg.dll`, the `sl.*.dll` Streamline runtimes and the NGX/Streamline headers
  are covered by the NVIDIA RTX SDKs licence in [LICENSE-NVIDIA.txt](LICENSE-NVIDIA.txt) and are
  redistributed unmodified as permitted. "This software contains source code provided by NVIDIA
  Corporation."
- NVIDIA Image Scaling is licensed under the MIT License in [LICENSE-NIS.txt](LICENSE-NIS.txt).
- `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll` and
  `amd_fidelityfx_framegeneration_dx12.dll` are AMD FidelityFX SDK binaries, redistributed unmodified
  under [LICENSE-AMD.txt](LICENSE-AMD.txt).
- `libxess.dll`, `libxess_fg.dll` and `libxell.dll` are Intel XeSS SDK binaries, redistributed
  unmodified under [LICENSE-INTEL.txt](LICENSE-INTEL.txt).
- NVIDIA, DLSS, GeForce, RTX; AMD, Radeon, FidelityFX, FSR; Intel, Arc, XeSS — and their respective
  logos — are trademarks of their respective owners. Renderforge is not affiliated with, endorsed by or
  sponsored by NVIDIA, AMD, Intel or Snapshot Games.
```

- [ ] **Step 2: Verify every licence file the README references exists**

Run:

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
foreach ($f in 'LICENSE','LICENSE-NIS.txt','LICENSE-NVIDIA.txt','LICENSE-AMD.txt','LICENSE-INTEL.txt','docs\RELEASING.md') {
    "{0,-22} {1}" -f $f, (Test-Path $f)
}
```

Expected: the five licence files `True`; `docs\RELEASING.md` is `False` until Task 6 — that is the only permitted `False` here.

- [ ] **Step 3: Repack and confirm the README inside the zips is the new one**

Run:

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
.\build\release.ps1 -SkipBuild
$a = [IO.Compression.ZipFile]::OpenRead('build\release\Renderforge-Core-1.1.0.zip')
$e = $a.GetEntry('Renderforge/README.md')
$r = New-Object IO.StreamReader($e.Open())
($r.ReadToEnd() -split "`n" | Select-String -Pattern 'Install — which zip do I need').Count
$r.Dispose(); $a.Dispose()
```

Expected: `1`.

- [ ] **Step 4: Commit**

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
git add README.md
git commit -m "docs(readme): install matrix, per-feature table, licences and troubleshooting"
```

---

### Task 6: `docs\RELEASING.md` and the DESIGN.md packaging section

**Files:**
- Create: `E:\DEV\PhoenixPoint\Renderforge\docs\RELEASING.md`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\docs\DESIGN.md` (insert a `## Packaging` section before `## Idea backlog`)

- [ ] **Step 1: Create `docs\RELEASING.md`**

```markdown
# Releasing Renderforge

Checklist for cutting a release. Everything below runs from `E:\DEV\PhoenixPoint\Renderforge`.
Steps marked **USER-GATED** publish to the outside world and are never run without the owner's
explicit go-ahead in chat.

## 1. Bump the version

- `meta.json` → `"Version": "<x.y.z>"` (this is what `build\release.ps1` names the zips after).
- `Renderforge.csproj` → `<Version>` / `<AssemblyVersion>` / `<FileVersion>` = `<x.y.z>.0`.
- Commit: `chore(release): bump to <x.y.z>`.

## 2. Refresh the vendor runtimes

For every `nvngx_*.dll`, compare the FileVersion on disk against the TechPowerUp DLL databases and
replace it with the newest NVIDIA-signed build if there is one. `build\release.ps1` warns when a DLL
differs from `$NewestKnownNgx`; after refreshing `refs\`, update that constant in the script too.
AMD and Intel binaries come from their SDK releases — bump the SDK, not individual DLLs.

## 3. Build and validate

```powershell
.\build\release.ps1 -ValidateOnly
```

Expect a source table and `validate: OK`, with no warnings. Any Authenticode failure is fatal by
design: fix the source, never bypass the check.

## 4. Pack

```powershell
.\build\release.ps1
```

Produces in `build\release\`: `Renderforge-{Core,NVIDIA,AMD,Intel,Full}-<x.y.z>.zip`, each with a
`Renderforge/` root and a `manifest-<pack>.json`, plus `SHA256SUMS.txt`.
Add `-WithFrameGen` once frame generation ships to include Streamline, AMD FG and XeSS-FG/XeLL.

## 5. Smoke-test the artefacts

```powershell
$t = "C:\Temp\claude\rf-release-check"
Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force $t | Out-Null
cd build\release
foreach ($n in 'Core','NVIDIA','AMD','Intel') { [IO.Compression.ZipFile]::ExtractToDirectory("Renderforge-$n-<x.y.z>.zip", $t, $true) }
Get-ChildItem $t -Directory   # must be exactly one folder: Renderforge
```

Then deploy the extracted folder over a clean install and launch once: the mod must load, the pickers
must appear, and the overlay must name the renderer and the upscaler.

## 6. Tag and push

```powershell
git tag v<x.y.z>
git push origin main --tags        # USER-GATED - pushing is never automatic
```

## 7. GitHub release — **USER-GATED**

```powershell
gh release create v<x.y.z> `
  build\release\Renderforge-Core-<x.y.z>.zip `
  build\release\Renderforge-NVIDIA-<x.y.z>.zip `
  build\release\Renderforge-AMD-<x.y.z>.zip `
  build\release\Renderforge-Intel-<x.y.z>.zip `
  build\release\Renderforge-Full-<x.y.z>.zip `
  build\release\SHA256SUMS.txt `
  --title "Renderforge <x.y.z>" --notes-file docs\release-notes-<x.y.z>.md
```

## 8. Steam Workshop — **USER-GATED**

The Workshop item ships the **Full** zip's content. Procedure and prerequisites:
`E:\DEV\PhoenixPoint\PerkOracle\docs\OPERATIONS.md` (SteamworksPy publisher, appid 839770, Steam client
running and logged in as the owner). Renderforge's `publishedfileid` is recorded in `docs\DESIGN.md`
under "Packaging".
```

Replace every `<x.y.z>` in the file above with nothing — it is a template and the placeholders are
intentional prose, not plan placeholders.

- [ ] **Step 2: Insert the Packaging section into DESIGN.md**

In `E:\DEV\PhoenixPoint\Renderforge\docs\DESIGN.md`, insert the following **immediately before** the
line `## Idea backlog (user, not scheduled)`:

```markdown
## Packaging (Phase 6, 2026-09-02)

- `build\release.ps1` is the only packaging path. It reads the version from `meta.json`, walks an
  ordered pack table, and emits `Renderforge-{Core,NVIDIA,AMD,Intel,Full}-<v>.zip` plus
  `SHA256SUMS.txt` into `build\release\`.
- **Layout rule:** every zip has ONE top-level `Renderforge/` folder and is extracted into
  `<Phoenix Point>\Mods\`. Core + any vendor packs overlay into a single `Mods\Renderforge\` in any
  order. (1.0.0's zip was flat and extracted INTO `Mods\Renderforge\`; 1.1.0 changed it, because an
  overlay cannot work without the prefix.)
- Pack contents: **Core** = `Renderforge.dll`, `RenderforgeNative.dll`, `meta.json`, `README.md`,
  `LICENSE`, `LICENSE-NIS.txt`. **NVIDIA** = `nvngx_dlss.dll` + `LICENSE-NVIDIA.txt` (with
  `-WithFrameGen`: `nvngx_dlssg.dll` 310.7.129 and `sl.{interposer,common,dlss,dlss_g,reflex,pcl}.dll`
  2.12.0). **AMD** = `amd_fidelityfx_{loader,upscaler}_dx12.dll` + `LICENSE-AMD.txt` (with
  `-WithFrameGen`: `amd_fidelityfx_framegeneration_dx12.dll`). **Intel** = `libxess.dll` +
  `LICENSE-INTEL.txt` (with `-WithFrameGen`: `libxess_fg.dll`, `libxell.dll`). **Full** = the union.
- Each zip also carries `manifest-<pack>.json`: mod id, version, generation timestamp, and per file
  the name, FileVersion, byte size, SHA-256, required Authenticode signer and licence file. Per-pack
  names, not one shared `manifest.json`, because vendor packs are extracted on top of Core.
- Every vendor DLL is Authenticode-asserted before packing — subject must contain `NVIDIA Corporation`
  / `Advanced Micro Devices` / `Intel Corporation`, status must be `Valid`; anything else fails the
  build. NVIDIA reports FileVersion with commas (`310,7,129,0`), so all comparisons normalise
  `-replace '[ ,]', '.'`.
- Stale-DLL guard: `$NewestKnownNgx` pins `nvngx_dlss.dll` and `nvngx_dlssg.dll` at `310.7.129.0`;
  a mismatch is a WARNING telling the operator to check the TechPowerUp DLL databases. Warning, not
  error — a newer DLL is legitimate, it just has to be a deliberate choice.
- Missing vendor DLLs are a supported state at runtime: `Availability.Reason` returns
  `DLL missing: <name> — install the <Vendor> pack` (RU: `Нет файла: <name> — установите пакет
  <Vendor>`), the row greys out, and everything else keeps working.
- Release checklist: `docs\RELEASING.md`. GitHub release and Steam Workshop upload are user-gated.
- Steam Workshop item: **TBD — filled in by the Workshop publish task**. Uses PerkOracle's
  SteamworksPy publisher (`PerkOracle\docs\OPERATIONS.md`), appid 839770, content = the Full pack.
```

- [ ] **Step 3: Verify both docs**

Run:

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
Test-Path docs\RELEASING.md
Select-String -Path docs\DESIGN.md -Pattern '^## Packaging|^## Idea backlog' | ForEach-Object { "$($_.LineNumber): $($_.Line)" }
```

Expected: `True`, then two lines with the `## Packaging (Phase 6, 2026-09-02)` line number **smaller**
than the `## Idea backlog` one.

- [ ] **Step 4: Commit**

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
git add docs/RELEASING.md docs/DESIGN.md
git commit -m "docs: packaging section + release checklist"
```

---

### Task 7: GitHub release v1.1.0 — dry run now, publish USER-GATED

**Files:**
- Create: `E:\DEV\PhoenixPoint\Renderforge\docs\release-notes-1.1.0.md`

- [ ] **Step 1: Write the release notes**

Create `E:\DEV\PhoenixPoint\Renderforge\docs\release-notes-1.1.0.md`:

```markdown
## Renderforge 1.1.0

Multi-vendor upscaling and a DirectX 12 renderer.

### New

- **DirectX 12 renderer switch** in Options → Graphics. Picks the API, offers the game's own restart
  dialog, and relaunches with `-force-d3d12`. Experimental.
- **DLSS on DirectX 12** as well as DirectX 11 (`nvngx_dlss.dll` 310.7.129, transformer model).
- **AMD FSR** — FSR 4.1.1 (ML) with the FSR 3.1.5 fallback, from one signed AMD runtime. DirectX 12.
- **Intel XeSS 2.0.2**, cross-vendor. DirectX 12.
- **UPSCALER / FRAME GENERATION pickers**; anything that cannot run is greyed with a tooltip saying why.
- DirectX 12 post-processing repair: SAO ambient occlusion + 2D-LUT colour grading, so fog of war and
  scene darkness match DirectX 11.

### Packaging change — read before updating

Zips now extract into `<Phoenix Point>\Mods\`, **not** into `Mods\Renderforge\`: each one carries its
own `Renderforge` folder so Core and the vendor packs can be layered.

| Your GPU | Download |
|---|---|
| NVIDIA GeForce RTX | `Renderforge-Core-1.1.0.zip` + `Renderforge-NVIDIA-1.1.0.zip` |
| AMD Radeon | `Renderforge-Core-1.1.0.zip` + `Renderforge-AMD-1.1.0.zip` |
| Intel Arc | `Renderforge-Core-1.1.0.zip` + `Renderforge-Intel-1.1.0.zip` |
| All of them | `Renderforge-Full-1.1.0.zip` |

Verify downloads against `SHA256SUMS.txt`. Every zip contains a `manifest-<pack>.json` with each DLL's
version, SHA-256 and licence.

### Not in this release

Frame generation (DLSS-G/MFG, FSR-FG, XeSS-FG) — the picker is present but greyed.

### Licences

Vendor runtimes are redistributed unmodified under `LICENSE-NVIDIA.txt`, `LICENSE-AMD.txt` and
`LICENSE-INTEL.txt`; Renderforge's own code is MIT.
```

- [ ] **Step 2: Dry run — confirm the tag is free, `gh` is authenticated, and every asset exists**

Run:

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
gh auth status
gh release list
gh release view v1.1.0 2>&1 | Select-Object -First 1
$assets = @(
  'build\release\Renderforge-Core-1.1.0.zip',
  'build\release\Renderforge-NVIDIA-1.1.0.zip',
  'build\release\Renderforge-AMD-1.1.0.zip',
  'build\release\Renderforge-Intel-1.1.0.zip',
  'build\release\Renderforge-Full-1.1.0.zip',
  'build\release\SHA256SUMS.txt',
  'docs\release-notes-1.1.0.md')
$assets | ForEach-Object { "{0,-46} {1,14:N0}" -f $_, (Get-Item $_ -ErrorAction SilentlyContinue).Length }
```

Expected: `Logged in to github.com account UberMorgott`; the release list showing only `v1.1.0` absent
and `v1.0.0` present; `release not found` from the `view`; all seven assets listed with non-empty sizes.

- [ ] **Step 3: USER-GATED — publish the release**

**Do not run this step without the owner's explicit approval in chat.** Present the command and the
asset list, wait for a yes, then run:

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
git tag v1.1.0
git push origin main --tags
gh release create v1.1.0 `
  build\release\Renderforge-Core-1.1.0.zip `
  build\release\Renderforge-NVIDIA-1.1.0.zip `
  build\release\Renderforge-AMD-1.1.0.zip `
  build\release\Renderforge-Intel-1.1.0.zip `
  build\release\Renderforge-Full-1.1.0.zip `
  build\release\SHA256SUMS.txt `
  --title "Renderforge 1.1.0" --notes-file docs\release-notes-1.1.0.md
```

Expected: the release URL printed, and `gh release view v1.1.0 --json assets --jq '.assets[].name'`
listing all six asset files.

- [ ] **Step 4: Commit the notes (independent of the gated publish)**

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
git add docs/release-notes-1.1.0.md
git commit -m "docs: release notes for 1.1.0"
```

---

### Task 8: Steam Workshop item — create and record the id (USER-GATED)

**Files:**
- Create: `E:\DEV\PhoenixPoint\Renderforge\workshop\Dist\` (staged content, gitignored — it is the Full pack's `Renderforge` folder)
- Create: `E:\DEV\PhoenixPoint\Renderforge\workshop\description.txt`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\docs\DESIGN.md` (the `Steam Workshop item: TBD` line from Task 6)
- Modify: `E:\DEV\PhoenixPoint\Renderforge\README.md` (the `STEAM_WORKSHOP_URL` link)

- [ ] **Step 1: Stage the Workshop content from the Full pack**

The Workshop item ships exactly the Full zip's content. Run:

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
$dist = 'E:\DEV\PhoenixPoint\Renderforge\workshop\Dist'
Remove-Item $dist -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $dist | Out-Null
[IO.Compression.ZipFile]::ExtractToDirectory('build\release\Renderforge-Full-1.1.0.zip', $env:TEMP + '\rf-full', $true)
Copy-Item ($env:TEMP + '\rf-full\Renderforge\*') $dist -Force
Remove-Item ($env:TEMP + '\rf-full') -Recurse -Force
Get-ChildItem $dist -File | ForEach-Object { "  {0,-42} {1,12:N0}" -f $_.Name, $_.Length }
```

Expected: 17 files — `Renderforge.dll`, `RenderforgeNative.dll`, `meta.json`, `README.md`, five
`LICENSE*`, `nvngx_dlss.dll`, both `amd_fidelityfx_*`, `libxess.dll`, and four `manifest-*.json`.
The Workshop copy is flat (no nested `Renderforge` folder) because Steam creates the mod folder itself.

- [ ] **Step 2: Add `workshop/` to `.gitignore` — the staged binaries must never be committed**

Append to `E:\DEV\PhoenixPoint\Renderforge\.gitignore`:

```gitignore
workshop/Dist/
```

Verify:

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
git check-ignore -v workshop/Dist/nvngx_dlss.dll
```

Expected: `.gitignore:12:workshop/Dist/	workshop/Dist/nvngx_dlss.dll`.

- [ ] **Step 3: Write the Workshop description**

Create `E:\DEV\PhoenixPoint\Renderforge\workshop\description.txt` (Steam BBCode, must stay under
8000 UTF-8 bytes — the PerkOracle publisher validates that limit):

```text
[h1]Renderforge[/h1]
Upscaling, anti-aliasing, sharpening, a DirectX 12 renderer switch, frame-rate control and a benchmark overlay for Phoenix Point.

[h2]What it does[/h2]
[list]
[*][b]NVIDIA DLSS Super Resolution and DLAA[/b] on DirectX 11 and DirectX 12 (DLSS 310.7.129, transformer model).
[*][b]AMD FidelityFX Super Resolution[/b] - FSR 4.1.1 on RDNA 3/4, FSR 3.1.5 everywhere else. DirectX 12.
[*][b]Intel XeSS 2.0.2[/b], cross-vendor. DirectX 12.
[*][b]Renderer switch[/b] between DirectX 11 and DirectX 12, right in the options screen.
[*]NVIDIA Image Scaling sharpening, automatic texture mip bias, SMAA turned off while upscaling.
[*]Removes the fixed 60 FPS cap; optional 30-300 FPS limit.
[*]Benchmark overlay: renderer, upscaler, mode, render and output resolution, FPS and frame time.
[*]Anything your hardware or renderer cannot run is greyed out and tells you why.
[/list]

[h2]How to use[/h2]
Subscribe, start Phoenix Point, open Main menu - Mods, enable Renderforge, restart once. The first launch copies the native plugin into the game's plugin folder, which is what the restart is for.
Then open Options - Graphics: RENDERER, UPSCALER, QUALITY and SHARPNESS are there. Changing the renderer or the upscaler needs a restart; quality and sharpness apply instantly.

[h2]Requirements[/h2]
Windows. DLSS needs a GeForce RTX 20-series or newer on a current driver. FSR and XeSS need the DirectX 12 renderer. DirectX 12 is experimental in this engine version.

[h2]Not included[/h2]
Frame generation is not in this release - the row is visible but greyed.

[h2]Source, manual downloads and issues[/h2]
https://github.com/UberMorgott/PhoenixPoint-Mod-Renderforge

[h2]Licences[/h2]
Renderforge's own code is MIT. The NVIDIA, AMD and Intel runtimes are redistributed unmodified under their respective licences, included in the mod folder as LICENSE-NVIDIA.txt, LICENSE-AMD.txt and LICENSE-INTEL.txt. NVIDIA, AMD and Intel trademarks belong to their owners; this mod is not affiliated with or endorsed by them, or by Snapshot Games.
```

- [ ] **Step 4: Confirm the publisher's prerequisites before asking for the gate**

Run:

```powershell
cd E:\DEV\PhoenixPoint\PerkOracle
Get-Process steam -ErrorAction SilentlyContinue | Select-Object Id, ProcessName
Get-Content workshop\steamugc\steam_appid.txt
Get-ChildItem workshop\steamugc -Filter *.dll | Select-Object Name
python workshop\steamugc\init_test.py
(Get-Item E:\DEV\PhoenixPoint\Renderforge\workshop\description.txt).Length
```

Expected: a running `steam` process; `839770`; the SteamworksPy native DLLs present; `init_test.py`
reporting the binding is ready for appid 839770 as SteamID64 76561197996210591; the description size
under 8000.

- [ ] **Step 5: USER-GATED — create the Workshop item**

**Do not run this without the owner's explicit approval in chat.** It creates a brand-new public
Workshop item under the owner's account. The publisher's module constants (`CONTENT_FOLDER`,
`PREVIEW_FILE`, `TITLE`, `WORKSHOP_TAGS`) are PerkOracle-specific, so pass Renderforge's paths through
a copy rather than editing PerkOracle's file in place — see `PerkOracle\docs\OPERATIONS.md` for the
full procedure and failure modes. A preview image is mandatory: prepare
`E:\DEV\PhoenixPoint\Renderforge\image\steam_preview.jpg` first.

```powershell
cd E:\DEV\PhoenixPoint\PerkOracle
python workshop\steamugc\publish_ugc.py --create `
  --changenote "v1.1.0 initial Workshop release" `
  --visibility public --tags "Utility,UI"
```

Expected: `SubmitItemUpdate ... EResult.OK` and a printed `publishedfileid`. Do not report success on
anything short of `EResult.OK`. If it reports the Workshop legal-agreement flag, open the item URL once
in a browser, accept, and re-run with `--update --item <id>`.

- [ ] **Step 6: USER-GATED — record the id**

Once Step 5 prints the id, replace in `E:\DEV\PhoenixPoint\Renderforge\docs\DESIGN.md` the line

```markdown
- Steam Workshop item: **TBD — filled in by the Workshop publish task**. Uses PerkOracle's
```

with

```markdown
- Steam Workshop item: **publishedfileid `<id>`** (https://steamcommunity.com/sharedfiles/filedetails/?id=<id>),
  appid 839770. Uses PerkOracle's
```

and in `E:\DEV\PhoenixPoint\Renderforge\README.md` replace

```markdown
1. [Subscribe on Steam Workshop](STEAM_WORKSHOP_URL) — the Workshop item is the **Full** bundle.
```

with

```markdown
1. [Subscribe on Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=<id>) — the Workshop item is the **Full** bundle.
```

Verify:

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
Select-String -Path README.md, docs\DESIGN.md -Pattern 'STEAM_WORKSHOP_URL|publishedfileid'
```

Expected: no `STEAM_WORKSHOP_URL` hit; one `publishedfileid` hit in DESIGN.md.

- [ ] **Step 7: Commit**

```powershell
cd E:\DEV\PhoenixPoint\Renderforge
git add .gitignore workshop/description.txt README.md docs/DESIGN.md
git commit -m "chore(workshop): description, staged content, publishedfileid recorded"
```

---

## Self-review notes

- **Spec coverage.** Spec §Packaging asks for `build\release.ps1` with Core/NVIDIA/AMD/Intel/Full zips,
  `SHA256SUMS.txt` and a build-time Authenticode check for every signed vendor DLL — Tasks 2 and 3.
  The brief's extras (per-zip `manifest.json`, version from `meta.json`, stale-NVIDIA guard, overlay
  layout) are Tasks 2-3; missing-DLL tolerance is Task 4; README matrix Task 5; DESIGN + RELEASING
  Task 6; GitHub release Task 7; Workshop Task 8.
- **Deviation from the brief, deliberate:** the per-zip manifest is `manifest-<pack>.json`, not
  `manifest.json`. Vendor packs are extracted on top of Core into the same folder, so a single shared
  filename would overwrite itself and lose the record of what else is installed.
- **Frame-generation files are behind `-WithFrameGen`, default off.** They exist on disk today but the
  shim cannot use them until Phase 5; shipping 8 unusable NVIDIA DLLs would be a 10 MB lie.
- **Unverified:** the `libxess.dll` entry assumes Phase 4 has landed by release time. If it has not,
  the Intel pack ships a DLL nothing calls — decide at release time whether to publish it at all
  (drop the `Intel` entry from `$packs` and the row from the README table). The in-game verification in
  Task 4 Step 5 has not been run; it needs Instance2 idle.
</content>
</invoke>
