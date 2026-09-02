param(
    # Deploy TARGET + reference assemblies. Default = the automation copy. The game you actually play is
    # D:\Steam\steamapps\common\Phoenix Point - name it explicitly if you mean it.
    [string] $PPRoot = 'D:\PP-Instance2',
    [string] $Configuration = 'Release',
    # Reuse build\out\*.dll instead of running build-native.ps1 (cmake + probe).
    [switch] $SkipNative,
    # Stage files even though that install's game is running (they load on the NEXT launch).
    [switch] $AllowRunning
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

if (-not (Test-Path (Join-Path $PPRoot 'PhoenixPointWin64.exe'))) {
    throw "No Phoenix Point at $PPRoot (no PhoenixPointWin64.exe there). Pass -PPRoot '<install folder>'."
}

# A running game holds the DLL it loaded at startup; matched by executable PATH so the other install never blocks.
$mine    = (Get-Item (Join-Path $PPRoot 'PhoenixPointWin64.exe')).FullName
$running = @(Get-CimInstance Win32_Process -Filter "Name='PhoenixPointWin64.exe'" |
             Where-Object { $_.ExecutablePath -and (Get-Item $_.ExecutablePath).FullName -eq $mine })
if ($running.Count -gt 0) {
    $what = "'$PPRoot' has Phoenix Point running (PID " + ($running.ProcessId -join ', ') + "), holding the build it loaded at startup."
    if ($AllowRunning) { Write-Warning "$what Continuing (-AllowRunning) - files are staged for the NEXT launch." }
    else { throw "REFUSED: $what Close the game, or stage for its next launch with: .\deploy.ps1 -PPRoot '$PPRoot' -AllowRunning" }
}

if (-not $SkipNative) {
    $native = Join-Path $root 'build-native.ps1'
    if (-not (Test-Path $native)) { throw "build-native.ps1 not found at $native" }
    & $native
    if ($LASTEXITCODE -ne 0) { throw "build-native.ps1 failed (exit $LASTEXITCODE)." }
}
$nativeDll = Join-Path $root 'build\out\RenderforgeNative.dll'
$ngxDll    = Join-Path $root 'build\out\nvngx_dlss.dll'
# AMD FidelityFX (FSR): the loader plus the upscaler DLL that contains 4.1.1 ML and the 3.1.5 fallback.
$amdLoader   = Join-Path $root 'build\out\amd_fidelityfx_loader_dx12.dll'
$amdUpscaler = Join-Path $root 'build\out\amd_fidelityfx_upscaler_dx12.dll'
$amdFrameGen = Join-Path $root 'build\out\amd_fidelityfx_framegeneration_dx12.dll'
# Intel XeSS (D3D12, cross-vendor DP4a + Intel XMX): one runtime DLL, delay-loaded by the shim.
$xessDll     = Join-Path $root 'build\out\libxess.dll'
# Intel XeSS-FG + XeLL runtimes: frame generation on the child HWND (FgXess.cpp), both delay-loaded by the shim.
$xessFgDll   = Join-Path $root 'build\out\libxess_fg.dll'
$xellDll     = Join-Path $root 'build\out\libxell.dll'
foreach ($f in $nativeDll, $ngxDll, $amdLoader, $amdUpscaler, $amdFrameGen, $xessDll, $xessFgDll, $xellDll) { if (-not (Test-Path $f)) { throw "missing $f - run build-native.ps1" } }

dotnet build (Join-Path $root 'Renderforge.csproj') -c $Configuration /p:PPRoot="$PPRoot"
if ($LASTEXITCODE -ne 0) { throw "dotnet build failed (exit $LASTEXITCODE)." }

$out  = Join-Path $root "bin\$Configuration\Renderforge"
$dest = Join-Path $PPRoot 'Mods\Renderforge'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
foreach ($file in (Join-Path $out 'Renderforge.dll'), (Join-Path $root 'meta.json'), $nativeDll, $ngxDll, $amdLoader, $amdUpscaler, $amdFrameGen, $xessDll, $xessFgDll, $xellDll,
                  (Join-Path $root 'LICENSE-NVIDIA.txt'), (Join-Path $root 'LICENSE-NIS.txt'), (Join-Path $root 'LICENSE-AMD.txt'), (Join-Path $root 'LICENSE-INTEL.txt'),
                  (Join-Path $root 'LICENSE'), (Join-Path $root 'README.md')) {
    Copy-Item $file $dest -Force
}

# Unity only calls UnityPluginLoad for plugins it resolves out of its own Plugins folder; the D3D12 backend
# needs IUnityInterfaces, so the shim is staged there too and Native.Load prefers that copy.
$plugins = Join-Path $PPRoot 'PhoenixPointWin64_Data\Plugins\x86_64'
New-Item -ItemType Directory -Force -Path $plugins | Out-Null
Copy-Item $nativeDll $plugins -Force
Write-Host "Staged RenderforgeNative.dll into $plugins (Unity plugin folder, for UnityPluginLoad)"

Write-Host "Deployed Renderforge to $dest"
Get-ChildItem $dest -File | ForEach-Object { Write-Host ("  {0,-20} {1,12:N0} bytes" -f $_.Name, $_.Length) }
Write-Host "Activation is separate: 'com.morgott.Renderforge' must be in MOD_ACTIVATED of the profile's Options.jopt"
Write-Host "  (%USERPROFILE%\AppData\LocalLow\Snapshot Games Inc\Phoenix Point\Steam\<SteamID64>\Options.jopt)."
Write-Host "  Launch $PPRoot once with -mods, enable Renderforge in the in-game mod manager, quit. This script never edits that file."
