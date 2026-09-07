# Renderforge — Agent Operations Runbook

Agent-facing runbook for routine ops on the **Renderforge** Phoenix Point mod.
Commands are verified against the scripts in `workshop\`. All paths assume
**CWD = `E:\DEV\PhoenixPoint\Renderforge`**. Release cutting (version bump, zips,
GitHub release) is `docs\RELEASING.md`; this file covers the Steam Workshop.

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

1. `.\build\release.ps1 -WithFrameGen` — ALWAYS with the flag: frame generation ships, and without it the Full zip
   silently drops the FG runtimes (122 MB instead of ~171 MB; bit us on 1.5.1). Compare the size against the last release.
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
