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

Since 1.2.1 only the **Full** zip is uploaded (single-zip policy — one archive, nothing to pick).

```powershell
gh release create v<x.y.z> `
  build\release\Renderforge-Full-<x.y.z>.zip `
  build\release\SHA256SUMS.txt `
  --title "Renderforge <x.y.z>" --notes-file docs\release-notes-<x.y.z>.md
```

## 8. Steam Workshop — **USER-GATED**

The Workshop item ships the **Full** zip's content. Procedure and prerequisites:
`E:\DEV\PhoenixPoint\PerkOracle\docs\OPERATIONS.md` (SteamworksPy publisher, appid 839770, Steam client
running and logged in as the owner). Renderforge's `publishedfileid` is recorded in `docs\DESIGN.md`
under "Packaging".
