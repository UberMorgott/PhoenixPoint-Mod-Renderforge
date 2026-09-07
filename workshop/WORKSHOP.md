# Renderforge — Steam Workshop publishing playbook

End-to-end guide for publishing and updating Renderforge on the Steam Workshop
for **Phoenix Point** (appid **839770**). Tooling copied from PerkOracle
(`E:\DEV\PhoenixPoint\PerkOracle\workshop\`), which has shipped that way since 2026.

Each step is tagged:
- **[NEEDS YOU]** — requires Steam login / a GUI / the owner's go-ahead.
- **[AUTOMATED]** — a script in this repo does it.

The agent-facing runbook (exact commands, verification) is `docs\OPERATIONS.md`.

---

## 0. What gets uploaded

A Phoenix Point Workshop item is a plain mod folder. For Renderforge that is the
**contents** of the Full release stage `build\release\stage\Full\Renderforge\` —
exactly what players get in `Renderforge-Full-<v>.zip`:

```
Renderforge.dll  RenderforgeNative.dll  meta.json  rf-exposure-d3d12.bundle
nvngx_dlss.dll  amd_fidelityfx_*_dx12.dll  libxess.dll  + FG DLLs (Streamline, nvngx_dlssg, amd_fidelityfx_framegeneration, libxess_fg, libxell)
README.md  LICENSE  LICENSE-NIS.txt  LICENSE-NVIDIA.txt  LICENSE-AMD.txt  LICENSE-INTEL.txt
manifest-full.json
```

`workshop\pack-dist.ps1` copies that folder flat into `workshop\Dist\` (gitignored,
no nested `Renderforge\`) and prints the total. It does **not** build: run
`build\release.ps1` first, and it refuses a missing stage or a stage whose
`meta.json` version differs from the repo's `meta.json`.

**Size: ~245 MB** (the vendor upscaler runtimes). The publisher allows 30 min for
the upload; SteamCMD has no such cap.

---

## 1. Prerequisites  **[NEEDS YOU once]**

- Steam desktop client **running and logged in as the owner** account. The
  publisher is headless and rides that session — no password is ever typed.
- Local-only native bits in `workshop\steamugc\` (gitignored; see its `README.md`):
  `steamworks/`, `SteamworksPy64.dll`, `steam_api64.dll`, `steam_appid.txt`.
  On this machine copy them from `PerkOracle\workshop\steamugc\`.
- Smoke test: `python workshop\steamugc\init_test.py` → `app_id = 839770` + your persona.

---

## 2. Images  **[AUTOMATED]**

`workshop\image\*_src.png` are the raw generated sources; `make-previews.ps1`
turns them into the deliverables (System.Drawing, no extra dependency):

```powershell
pwsh -File workshop\image\make-previews.ps1
```

| Output | Spec |
|---|---|
| `steam_preview.jpg` | square, ≤ 1024×1024, JPEG q≈85, **must be < 1 MB** (Steam rejects it otherwise; the script steps quality down until it fits) |
| `github_social.png` | 1280×640, centre-crop — GitHub → Settings → Social preview (web only) |

The square preview is set headlessly by every `--create`/`--update`
(`SetItemPreview`). **Gallery screenshots are web-UI only**: this SteamworksPy
build lacks `AddItemPreviewFile`, so add them at
`https://steamcommunity.com/sharedfiles/managepreviews/?id=<id>`.

---

## 3. First publish  **[NEEDS YOU — go-ahead + page review]**

Create the item **hidden** so the store page can be reviewed before anyone sees it:

```powershell
.\build\release.ps1                      # stage the Full pack (if not already)
pwsh -File workshop\pack-dist.ps1        # -> workshop\Dist (~245 MB)
python workshop\steamugc\publish_ugc.py --create --changenote "v<x.y.z> initial release" --visibility hidden
```

On success it prints the item URL, writes `workshop\steamugc\published_id.txt`
and stamps the id into `workshop\renderforge.vdf`. Commit both. If Steam flags the
**Workshop legal agreement**, accept it once at the item URL.

Then push the 8 localized descriptions (english is the fallback):

```powershell
python workshop\steamugc\publish_ugc.py --localize-descriptions --item <id> --changenote "descriptions"
```

Review the page, add gallery screenshots in the web UI, then make it public:

```powershell
python workshop\steamugc\publish_ugc.py --update --item <id> --changenote "v<x.y.z>" --visibility public
```

---

## 4. Updates  **[AUTOMATED, owner's go-ahead]**

```powershell
.\build\release.ps1
pwsh -File workshop\pack-dist.ps1
python workshop\steamugc\publish_ugc.py --update --item <id> --changenote "v<x.y.z> - <what changed>"
```

`--visibility` omitted = unchanged. Watch for `[update] OK -> upload committed`;
the item's "Last updated" timestamp changes.

**Descriptions only** (no content upload, "Last updated" does NOT change):

```powershell
python workshop\steamugc\publish_ugc.py --localize-descriptions --item <id> --changenote "<note>"
```

Edit `workshop\locale\description.<lang>.txt` first — BBCode, no `[color]`,
each file **< 8000 UTF-8 bytes** (the script validates bytes, not characters).

---

## 5. Tags

**None.** Phoenix Point's valid Workshop tags are Geoscape, Tactical, Difficulty,
Gameplay, Bionics, Mutations — all gameplay categories; none describes a
graphics/engine mod, and an unknown tag can fail the submit. `WORKSHOP_TAGS = []`
in `publish_ugc.py`; if Snapshot ever adds a fitting tag, set it there and re-run
`--localize-descriptions` (tags are applied on the english pass).

---

## 6. SteamCMD fallback  **[NEEDS YOU to log in]**

Only if SteamworksPy cannot be provisioned. Requires an existing item (the vdf's
`publishedfileid` must not be `0`):

```powershell
./workshop/update.ps1 -ChangeNote "What changed" -SteamUser <yoursteamname>
```

Packs `Dist\`, stamps the change note into `renderforge.vdf`, runs
`steamcmd +login <user> +workshop_build_item <abs vdf> +quit`. SteamCMD prompts
for password + Steam Guard in its own console; nothing is stored. Install SteamCMD
from <https://steamcdn-a.akamaihd.net/client/installer/steamcmd.zip> into `C:\steamcmd`.

---

## File map

| Path | Purpose | Committed |
|---|---|---|
| `workshop\pack-dist.ps1` | Full stage → `Dist\`, version-checked | yes |
| `workshop\Dist\` | Upload content folder | no (gitignored) |
| `workshop\renderforge.vdf` | SteamCMD descriptor (id stamped by publisher) | yes |
| `workshop\update.ps1` | SteamCMD fallback: pack + upload | yes |
| `workshop\steamugc\publish_ugc.py` | Headless publisher (create / update / localize) | yes |
| `workshop\steamugc\init_test.py` | Steam binding smoke test | yes |
| `workshop\steamugc\published_id.txt` | The item id, written by `--create` | yes |
| `workshop\steamugc\{steamworks/,*.dll,steam_appid.txt}` | SteamworksPy vendored bits | no (gitignored) |
| `workshop\locale\description.<lang>.txt` | 8 BBCode store descriptions | yes |
| `workshop\image\*_src.png` | Raw generated art | yes |
| `workshop\image\make-previews.ps1` | Sources → `steam_preview.jpg` + `github_social.png` | yes |
| `workshop\image\steam_preview.jpg` | Square Workshop preview (< 1 MB) | yes |
| `workshop\image\github_social.png` | GitHub social preview 1280×640 | yes |
