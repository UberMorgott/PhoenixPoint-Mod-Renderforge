# build\stamp.ps1 - the build stamp: which commit and which source CONTENT a binary was built from.
#
# Why it exists (2026-10-12): MSBuild decides "up to date" by file TIME. A partial tree was built into the shared
# intermediate dir by the pre-commit gate, then the full sources were restored by a copy that kept their OLD mtime -
# every later incremental build linked objects of the partial tree, and nothing said so. Content is the only proof:
#   - build-native.ps1 keeps a content manifest per build dir and re-dates every file whose content moved,
#   - both DLLs carry `RFSTAMP|kind=..|commit=..|dirty=..|src=..|END` (src = hash of the source tree they were built from),
#   - deploy.ps1 / build\release.ps1 read the stamp back out of the binary and refuse a mismatch with the tree.
#
# Dot-source for the functions, or run:
#   stamp.ps1 -WriteNative  <header.h>   C header with RF_BUILD_STAMP (rewritten only when it changes)
#   stamp.ps1 -WriteManaged <file.cs>    [assembly: AssemblyMetadata("RfStamp", ...)] + Renderforge.BuildStamp
#   stamp.ps1 -Read <dll>                print the stamp found in a binary
param([string] $WriteNative, [string] $WriteManaged, [string] $Read)

$ErrorActionPreference = 'Stop'
$script:RfRoot = Split-Path $PSScriptRoot -Parent

# What each binary is built from. Paths are repo-relative git pathspecs; ignored files (native/build, bin, obj) never count.
$script:RfSourceSets = @{
    native  = @('native', 'build-native.ps1')
    managed = @('src', 'Renderforge.csproj', 'meta.json')
}

function Get-RfSourceFiles([string] $Kind) {
    # Tracked + untracked-but-not-ignored, minus files deleted from the working tree. One "hash path" line per file,
    # hashed from the WORKING TREE content (git hash-object), so an uncommitted edit changes the result.
    $spec = $script:RfSourceSets[$Kind]
    if (-not $spec) { throw "unknown source set '$Kind'" }
    $files = @(git -C $script:RfRoot -c core.quotepath=off ls-files -co --exclude-standard -- @spec |
               Sort-Object -Unique | Where-Object { Test-Path -LiteralPath (Join-Path $script:RfRoot $_) -PathType Leaf })
    if ($LASTEXITCODE -ne 0 -or $files.Count -eq 0) { throw "git ls-files found no '$Kind' sources under $script:RfRoot" }
    Push-Location $script:RfRoot
    try { $hashes = @($files | git hash-object --no-filters --stdin-paths) } finally { Pop-Location }
    if ($LASTEXITCODE -ne 0 -or $hashes.Count -ne $files.Count) { throw "git hash-object failed for the '$Kind' sources" }
    for ($i = 0; $i -lt $files.Count; $i++) { "{0} {1}" -f $hashes[$i], $files[$i] }
}

function Get-RfSourceHash([string] $Kind) {
    $text = (Get-RfSourceFiles $Kind) -join "`n"
    $sha = [Security.Cryptography.SHA256]::Create()
    try { -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)) | Select-Object -First 8 | ForEach-Object { $_.ToString('x2') }) }
    finally { $sha.Dispose() }
}

function Get-RfStamp([string] $Kind) {
    $commit = (git -C $script:RfRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw "git rev-parse HEAD failed in $script:RfRoot" }
    $dirty = if (@(git -C $script:RfRoot status --porcelain --untracked-files=all).Count -gt 0) { 1 } else { 0 }
    [pscustomobject]@{ Kind = $Kind; Commit = $commit; Dirty = $dirty; Src = (Get-RfSourceHash $Kind) }
}

function Format-RfStamp($s) { "RFSTAMP|kind=$($s.Kind)|commit=$($s.Commit)|dirty=$($s.Dirty)|src=$($s.Src)|END" }

function Read-RfStamp([string] $Path) {
    # The stamp is plain ASCII in both binaries (a C string literal / a UTF-8 custom-attribute blob).
    $text = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Path).Path))
    $m = [regex]::Matches($text, 'RFSTAMP\|kind=(\w+)\|commit=([0-9a-f]{40})\|dirty=([01])\|src=([0-9a-f]{16})\|END')
    if ($m.Count -eq 0) { return $null }
    $distinct = @($m | ForEach-Object { $_.Value } | Sort-Object -Unique)
    if ($distinct.Count -ne 1) { throw "$Path carries $($distinct.Count) different build stamps: $($distinct -join ' , ')" }
    $g = $m[0].Groups
    [pscustomobject]@{ Kind = $g[1].Value; Commit = $g[2].Value; Dirty = [int]$g[3].Value; Src = $g[4].Value }
}

function Assert-RfStamp([string] $Path, [string] $Kind, [switch] $RequireClean) {
    # The binary must have been built from exactly the tree that is on disk now.
    $have = Read-RfStamp $Path
    if (-not $have) { throw "BUILD STAMP: $Path carries no RFSTAMP - it predates the stamp or is not ours. Rebuild." }
    $want = Get-RfStamp $Kind
    $why = @()
    if ($have.Kind -ne $Kind)        { $why += "kind $($have.Kind), expected $Kind" }
    if ($have.Commit -ne $want.Commit) { $why += "built at commit $($have.Commit.Substring(0, 12)), HEAD is $($want.Commit.Substring(0, 12))" }
    if ($have.Src -ne $want.Src)     { $why += "built from $Kind sources $($have.Src), the tree is $($want.Src)" }
    if ($have.Dirty -ne $want.Dirty) { $why += "dirty=$($have.Dirty), the tree is dirty=$($want.Dirty)" }
    if ($RequireClean -and ($have.Dirty -ne 0 -or $want.Dirty -ne 0)) { $why += "a release is built from a clean tree (dirty=$($have.Dirty)/$($want.Dirty))" }
    if ($why.Count) { throw "BUILD STAMP MISMATCH: $Path - $($why -join '; '). Rebuild (build-native.ps1 / dotnet build) and do not ship this file." }
    Write-Host ("build stamp OK: {0,-24} {1} commit={2} dirty={3} src={4}" -f (Split-Path $Path -Leaf), $Kind, $have.Commit.Substring(0, 12), $have.Dirty, $have.Src)
    $have
}

function Set-RfContentIfChanged([string] $Path, [string] $Content) {
    if ((Test-Path -LiteralPath $Path) -and ([IO.File]::ReadAllText($Path) -ceq $Content)) { return }
    New-Item -ItemType Directory -Force -Path (Split-Path $Path -Parent) | Out-Null
    [IO.File]::WriteAllText($Path, $Content, (New-Object Text.UTF8Encoding $false))
}

if ($WriteNative) {
    $s = Format-RfStamp (Get-RfStamp 'native')
    Set-RfContentIfChanged ([IO.Path]::GetFullPath($WriteNative)) "// generated by build\stamp.ps1 - do not edit`n#define RF_BUILD_STAMP `"$s`"`n"
    Write-Host $s
}
if ($WriteManaged) {
    $s = Format-RfStamp (Get-RfStamp 'managed')
    $cs = "// generated by build\stamp.ps1 - do not edit`n[assembly: System.Reflection.AssemblyMetadata(`"RfStamp`", `"$s`")]`n" +
          "namespace Renderforge { internal static class BuildStamp { internal const string Value = `"$s`"; } }`n"
    Set-RfContentIfChanged ([IO.Path]::GetFullPath($WriteManaged)) $cs
    Write-Host $s
}
if ($Read) {
    $s = Read-RfStamp $Read
    if (-not $s) { Write-Host "no build stamp in $Read"; exit 1 }
    Write-Host (Format-RfStamp $s)
}
