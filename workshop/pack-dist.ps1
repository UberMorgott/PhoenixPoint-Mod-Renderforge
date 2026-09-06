<#
.SYNOPSIS
    Assembles the Steam Workshop content folder workshop\Dist\ from the Full
    release stage build\release\stage\Full\Renderforge\ (what players get in
    Renderforge-Full-<v>.zip: managed DLL, RenderforgeNative.dll, meta.json,
    vendor DLLs, LICENSE files, README, exposure bundle, manifest-full.json).

    Dist\ holds the CONTENTS of that folder, flat - no nested Renderforge\ -
    because a Workshop item is the mod folder itself (PerkOracle: Dist\Oracle.dll
    + meta.json at the root). Idempotent: cleans Dist\ then re-copies.

.NOTES
    Does NOT build. Run build\release.ps1 first; this script refuses a missing
    stage or a stage whose meta.json version differs from the repo's meta.json.
    Dist\ is gitignored.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot                       # ...\Renderforge
$stage    = Join-Path $repoRoot 'build\release\stage\Full\Renderforge'
$dist     = Join-Path $PSScriptRoot 'Dist'                         # workshop\Dist

if (-not (Test-Path (Join-Path $stage 'meta.json'))) {
    throw "Release stage not found: $stage`nRun build\release.ps1 first."
}
$repoVersion  = (Get-Content (Join-Path $repoRoot 'meta.json') -Raw | ConvertFrom-Json).Version
$stageVersion = (Get-Content (Join-Path $stage 'meta.json') -Raw | ConvertFrom-Json).Version
if ($repoVersion -ne $stageVersion) {
    throw "Stage is $stageVersion but meta.json is $repoVersion. Re-run build\release.ps1 before packing."
}
foreach ($must in 'Renderforge.dll', 'RenderforgeNative.dll') {
    if (-not (Test-Path (Join-Path $stage $must))) { throw "Stage is missing $must" }
}

if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Force -Path $dist | Out-Null
Copy-Item (Join-Path $stage '*') $dist -Recurse -Force

$files = Get-ChildItem $dist -Recurse -File
$files | ForEach-Object { $_.FullName.Substring($dist.Length).TrimStart('\') } | Sort-Object |
    ForEach-Object { Write-Host "  $_" }
$total = ($files | Measure-Object Length -Sum).Sum
Write-Host ("Dist {0}: {1} files, {2:N1} MB at {3}" -f $stageVersion, $files.Count, ($total / 1MB), $dist) -ForegroundColor Green
