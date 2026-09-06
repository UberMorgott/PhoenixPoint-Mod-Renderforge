<#
.SYNOPSIS
    SteamCMD FALLBACK update path for the Renderforge Steam Workshop item.

    The primary path is the headless publisher (see WORKSHOP.md):
        python workshop\steamugc\publish_ugc.py --update --item <id> --changenote "..."
    Use this script only when SteamworksPy cannot be provisioned. It packs
    workshop\Dist, then uploads via SteamCMD's workshop_build_item using
    renderforge.vdf.

.DESCRIPTION
    Steps:
      1. Verify SteamCMD is available (PATH or known locations). If not, print an
         install hint and stop.
      2. Verify renderforge.vdf has a real publishedfileid (not "0" - the item
         must already exist; publish_ugc.py --create stamps the id here).
      3. Run pack-dist.ps1 to (re)build workshop\Dist.
      4. Stamp the changenote into the vdf, then run:
             steamcmd +login <user> +workshop_build_item <abs vdf> +quit

    SECURITY: credentials are NEVER stored or hardcoded here. The username is a
    parameter; SteamCMD prompts for the password and Steam Guard / 2FA code
    interactively in its own console.

.PARAMETER ChangeNote
    Required. The change note recorded in the item's history.

.PARAMETER SteamUser
    Optional Steam account name. If omitted, you'll be prompted, and SteamCMD
    will still prompt for password + 2FA.

.PARAMETER SteamCmd
    Optional explicit path to steamcmd.exe.

.EXAMPLE
    ./workshop/update.ps1 -ChangeNote "v1.5.0 - FSR 3" -SteamUser mysteamname
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $ChangeNote,

    [string] $SteamUser,

    [string] $SteamCmd
)

$ErrorActionPreference = 'Stop'

$here = $PSScriptRoot
$vdf  = Join-Path $here 'renderforge.vdf'
$pack = Join-Path $here 'pack-dist.ps1'

# --- 1. Locate SteamCMD ---------------------------------------------------
function Resolve-SteamCmd {
    param([string] $Explicit)
    if ($Explicit) {
        if (Test-Path $Explicit) { return (Resolve-Path $Explicit).Path }
        throw "steamcmd not found at -SteamCmd '$Explicit'."
    }
    $cmd = Get-Command steamcmd, steamcmd.exe -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        'C:\steamcmd\steamcmd.exe',
        "$env:ProgramFiles\steamcmd\steamcmd.exe",
        "${env:ProgramFiles(x86)}\Steam\steamcmd.exe"
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    return $null
}

$steam = Resolve-SteamCmd -Explicit $SteamCmd
if (-not $steam) {
    Write-Error @"
SteamCMD not found.

Install: download https://steamcdn-a.akamaihd.net/client/installer/steamcmd.zip,
extract to C:\steamcmd, then re-run (or pass -SteamCmd C:\steamcmd\steamcmd.exe).
"@
    exit 1
}
Write-Host "SteamCMD: $steam" -ForegroundColor Cyan

# --- 2. Verify publishedfileid is set -------------------------------------
if (-not (Test-Path $vdf)) { throw "VDF not found: $vdf" }
$vdfText = Get-Content -Raw $vdf
if ($vdfText -match '"publishedfileid"\s*"0"') {
    Write-Error @"
renderforge.vdf still has publishedfileid "0" (no item created yet).

Create the item first with the headless publisher (it stamps the id into the vdf):

    python workshop\steamugc\publish_ugc.py --create --changenote "..." --visibility hidden

Then re-run this script.
"@
    exit 1
}

# --- 3. Build clean /Dist -------------------------------------------------
Write-Host "Packing Dist..." -ForegroundColor Cyan
& $pack

# --- 4. Stamp changenote into the vdf -------------------------------------
# Escape backslashes and quotes for VDF, then replace the changenote value.
$escaped = $ChangeNote -replace '\\', '\\\\' -replace '"', '\"'
$vdfText = [regex]::Replace(
    $vdfText,
    '("changenote"\s*")[^"]*(")',
    { param($m) $m.Groups[1].Value + $escaped + $m.Groups[2].Value }
)
Set-Content -Path $vdf -Value $vdfText -Encoding UTF8 -NoNewline

# --- 5. Upload via SteamCMD ----------------------------------------------
if (-not $SteamUser) {
    $SteamUser = Read-Host "Steam account name"
}
Write-Host "Uploading to Steam Workshop as '$SteamUser'..." -ForegroundColor Cyan
Write-Host "(SteamCMD will prompt for password + Steam Guard/2FA in its console.)"

& $steam +login $SteamUser +workshop_build_item $vdf +quit
$code = $LASTEXITCODE
if ($code -ne 0) {
    Write-Error "SteamCMD exited with code $code. Check its output above."
    exit $code
}
Write-Host "`nDone. Verify the item on the Workshop page." -ForegroundColor Green
