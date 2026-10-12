# Renderforge — Agent Operations Runbook

Agent-facing runbook for routine ops on the **Renderforge** Phoenix Point mod.
Commands are verified against the scripts in `workshop\`. All paths assume
**CWD = `E:\DEV\PhoenixPoint\Renderforge`**. Release cutting (version bump, zips,
GitHub release) is `docs\RELEASING.md`; this file covers the Steam Workshop.

## Build stamp — which tree a binary was built from (2026-10-12)

- **Why:** an incremental `deploy.ps1` shipped a `RenderforgeNative.dll` that was NOT HEAD. The pre-commit gate had
  built a stash-reduced tree into `build\native`; the full sources then came back through a copy that kept their OLD
  mtime, so MSBuild (which compares file TIMES) kept linking the reduced tree's objects. A whole regression run was void.
- **Stamp:** both DLLs carry `RFSTAMP|kind=<native|managed>|commit=<40 hex>|dirty=<0|1>|src=<16 hex>|END`
  (`build\stamp.ps1`; `src` = hash of the working-tree content of that binary's sources: `native\` + `build-native.ps1`,
  resp. `src\` + `Renderforge.csproj` + `meta.json`).
  - Read it from a file: `pwsh -File build\stamp.ps1 -Read <dll>`.
  - At mod load (Player.log / mod log): `build stamp: managed=<commit12>[+dirty] src=<h> native=<commit12>[+dirty] src=<h>`,
    plus ` MISMATCH` when the two DLLs are not one build. PPCLI: `{"member":"BuildInfo"}` on `RenderforgeMod`.
  - **A test run is about HEAD only when that line shows HEAD's commit twice and no `+dirty`.** Record it with the results.
- **Enforcement:** `build-native.ps1` asserts the linked DLL's stamp; `deploy.ps1` asserts both DLLs against the tree
  before copying anything (also with `-SkipNative`: a `build\out` older than HEAD is refused);
  `build\release.ps1` refuses a dirty tree, rebuilds native with `-Clean` (intermediate dir deleted first) and asserts
  both stamps = HEAD, clean - also with `-SkipBuild`. The pack manifests record `commit`.
- **Objects follow content:** `build\native\rf-src-manifest.txt` records the content hash of every native source per
  build; the next build re-dates each file whose content differs (`build-native: content changed ..., re-dated: <path>`),
  so MSBuild recompiles it whatever its mtime says. A build dir without a manifest is deleted (unknown origin).
- **The gate builds elsewhere:** inside a `qgate -Full` run (`QGATE_SLOT_HELD=1`, or `build-native.ps1 -Gate` by hand) the build goes to `build\gate\native` + `build\gate\out`.
  Nothing the gate compiles (a partial commit, a reduced tree) can reach `build\native` / `build\out`.
- Never restore sources into the tree with a tool that keeps timestamps and then trust an incremental build made by
  anything other than `build-native.ps1`.

## FG test levers and diagnostics lines (2026-10-12)

- `RENDERFORGE_FG_TEST_SDK_FAIL=<call>[,<count>][,skip=<n>]`, `RENDERFORGE_FG_TEST_PRESENT_DELAY=<ms>,<every>` - see DESIGN.
- `RENDERFORGE_FG_TEST_PRESENT_STUCK=<ms>,<every>` - every every-th shadow Present sleeps ms inside the window the present
  watchdog times (a vendor Present that does not return). `> 2000` ms = the three `WATCHDOG` lines + `returned`; quit the
  game while it sleeps = the regression for the watchdog thread's lifetime (clean exit, no `did not stop` line).
- `RENDERFORGE_FG_TEST_VRAM_MB=<n>` - commit n MB of throwaway textures on Unity's device before the first FG chain
  (`host: TEST RENDERFORGE_FG_TEST_VRAM_MB=... committed k of m textures, vram local u/b MB`). Check `nvidia-smi`
  first and leave room for whatever else runs on the GPU.
- `host: provider=... , vram local <usage>/<budget> MB` - every chain start. `OVER BUDGET` = expect stalls.
- `host: WATCHDOG shadow Present #n ... has not returned for N ms` - a vendor Present is stuck; the line says whether
  Unity's queue still retires work (`Unity queue probe COMPLETED` / `DID NOT COMPLETE`). Keep the log and the dump.
- PPCLI on `RenderforgeMod`: `BuildInfo` (stamps), `FgCaps` (who paces: `targetFrameRate=-1` = the provider's SDK),
  `FgLatency` (XeSS `sdkFails=mode/enable/input`, ` sleepFails=n` once an `xellSleep` was refused),
  `ProbeMarkerClick` / `ProbeCraftClick` (both: `pass=True` = `moved=0`, the mod changes no click; `other` is geometry).
- Stuck vendor Present, three lines (2026-10-13): `host: WATCHDOG shadow Present #n ...` (queue probe + vram),
  `host: WATCHDOG fences: copy fence completed a of signalled b; Unity frame fence completed c, next at Present entry d`,
  `host: WATCHDOG presenting thread <tid> stack: N frames < module+0xoffset < ...` (which vendor wait it sits in; the
  vendor's own game / interpolation / composition fences have no public query). The watchdog owns references to what it
  probes and is joined at device shutdown and plugin unload (`... did not stop within 15 s` = a driver call hangs).
- `AuditMvModes` keys are `family/active|inactive/mode` (inactive = pooled or hidden objects).
- Known limits (review 5, documented, not fixed): (a) XeSS-FG - a REFUSED cap change leaves XeLL enforcing the previous
  accepted cap while Unity's limiter takes the new one (`FG_PACER_HELD` off): two pacers until the retry (every 30
  presents) is accepted or the request is taken back; the effective cap is the stricter of the two. DLSS-G: the same
  for the first frame of a flow start. (b) DLSS-G `Off()` at teardown is not gated on "options issued this Streamline
  frame": one `Repeated slDLSSGSetOptions` warning is possible and logged by us first. (c) the gate's build isolation
  (`build\gate\`) hangs on `QGATE_SLOT_HELD=1` set by `qgate -Full`; a gate run without it builds into `build\native`
  - the stamps then refuse the stale DLL at deploy/release. `refs\` SDK headers are outside the source hash: after an
  SDK update build with `-Clean`.

## Workshop
### Identity & paths

| Thing | Value |
|---|---|
| Steam appid (Phoenix Point) | **839770** |
| Workshop publishedfileid | `workshop\steamugc\published_id.txt` (also stamped in `workshop\renderforge.vdf`); `0` in the vdf = not created yet |
| Item URL | `https://steamcommunity.com/sharedfiles/filedetails/?id=<id>` |
| Owner SteamID64 (Morgott) | **76561197996210591** |
| Publisher script | `workshop\steamugc\publish_ugc.py` |
| Pack script | `workshop\pack-dist.ps1` (Full stage → `workshop\Dist\`, ~245 MB) |
| Locale descriptions | `workshop\locale\description.<lang>.txt` (english, russian, german, french, spanish, italian, polish, schinese) |
| Preview image (square) | `workshop\image\steam_preview.jpg` (< 1 MB; built by `workshop\image\make-previews.ps1`) |
| Tags | **none** — no Phoenix Point tag (Geoscape, Tactical, Difficulty, Gameplay, Bionics, Mutations) fits a graphics mod |
| Playbook | `workshop\WORKSHOP.md` |

### Prerequisites (CRITICAL — check before any publish)

1. **Steam desktop client RUNNING and LOGGED IN as the owner** (Morgott,
   SteamID64 76561197996210591). The publisher rides that session; no
   username/password is used. Wrong account or closed Steam → wrong owner / init failure.
2. **Native deps in `workshop\steamugc\` are gitignored**: `steamworks/`,
   `SteamworksPy64.dll`, `steam_api64.dll` (must export `SteamInternal_SteamAPI_Init`;
   PP's bundled one is too old → `WinError 127`), `steam_appid.txt` (= `839770`).
   On this machine copy them from `E:\DEV\PhoenixPoint\PerkOracle\workshop\steamugc\`.
3. Smoke test: `python workshop\steamugc\init_test.py` → `app_id = 839770` + persona.
4. **Publishing is USER-GATED.** Never run `--create` / `--update` /
   `--localize-descriptions` / `update.ps1` without the owner's explicit go-ahead
   in chat. Packing (`pack-dist.ps1`) and `--help` are free.

### Two DIFFERENT operations — do not confuse them

- **`--update`** uploads `workshop\Dist\` = the new mod build. Subscribers get it.
- **`--localize-descriptions`** edits only the store text (+ tags). Ships nothing.

### Task: first publish (item does not exist yet)

1. `.\build\release.ps1` — needs a CLEAN, committed tree (refuses otherwise); rebuilds native from an empty intermediate
   dir and asserts both DLL build stamps = HEAD (§Build stamp). Packs every runtime including frame generation (the old `-WithFrameGen` opt-in is gone:
   a bare run once produced a 122 MB Full zip without FG). Compare the Full zip size (~171 MB) against the last release.
2. `pwsh -File workshop\pack-dist.ps1` — must print `Dist <version>: N files, X MB`.
   It throws if the stage is missing or its version differs from `meta.json`.
3. `pwsh -File workshop\image\make-previews.ps1` — `steam_preview.jpg` < 1 MB.
4. `python workshop\steamugc\publish_ugc.py --create --changenote "v<x.y.z> initial release" --visibility hidden`
   — HIDDEN on purpose; the owner reviews the page first. Expect
   `[create] OK -> publishedfileid=<id>` then `[update] OK -> upload committed`.
   Writes `published_id.txt`, stamps the vdf. If the legal-agreement flag is
   printed, the owner accepts it once at the item URL.
5. `python workshop\steamugc\publish_ugc.py --localize-descriptions --item <id> --changenote "descriptions"`
   — expect 8 × `EResult.OK`.
6. Owner adds gallery screenshots in the web UI
   (`https://steamcommunity.com/sharedfiles/managepreviews/?id=<id>`) — not scriptable.
7. Go public: `python workshop\steamugc\publish_ugc.py --update --item <id> --changenote "v<x.y.z>" --visibility public`.
8. Commit `workshop\steamugc\published_id.txt` + `workshop\renderforge.vdf`:
   `chore(workshop): record publishedfileid <id>`.

### Task: update the mod (new build)

Trigger: "обнови мод", "update the mod", "publish a new build".

1. Version bumped per `docs\RELEASING.md` §1; `.\build\release.ps1` run.
2. `pwsh -File workshop\pack-dist.ps1`.
3. `python workshop\steamugc\publish_ugc.py --update --item <id> --changenote "v<x.y.z> - <what changed>"`
   (`--visibility` omitted = unchanged). Block until `[update] OK -> upload committed`.
4. Verify: item page "Last updated" changed; the changenote appears in Change Notes.
5. Commit anything changed: `chore(workshop): publish v<x.y.z>`.

### Task: edit / localize the store description

Trigger: "поменяй описание", "update the description", "localize".

1. Edit `workshop\locale\description.<lang>.txt` — BBCode, no `[color]`,
   `[list=1]…[/list]` for ordered lists, **< 8000 UTF-8 bytes per file**
   (Cyrillic/CJK ≥ 2 bytes per char; the script aborts on overflow).
2. `python workshop\steamugc\publish_ugc.py --localize-descriptions --item <id> --changenote "<note>"`
   — per-language `EResult.OK` table, all OK.
3. Commit: `docs(workshop): <what changed>`.

### Task: change tags

Edit `WORKSHOP_TAGS = [...]` in `workshop\steamugc\publish_ugc.py` (valid: Geoscape,
Tactical, Difficulty, Gameplay, Bionics, Mutations; unknown tags fail the submit),
then re-run `--localize-descriptions` (tags apply on the english pass). Current
decision: **empty** — none fits a graphics mod.

### Task: SteamCMD fallback

Only when SteamworksPy cannot be provisioned; item must already exist:
`./workshop/update.ps1 -ChangeNote "<what changed>" -SteamUser <name>` — SteamCMD
prompts for password + Steam Guard itself; nothing is stored.
