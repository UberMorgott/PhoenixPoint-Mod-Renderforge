# Headless Steam Workshop publisher (SteamworksPy)

Publishes / updates the Renderforge Workshop item by riding the **already-running,
logged-in Steam client** — no username or password, exactly like the official
PPWorkshopTool. Auth = your active Steam session. Copied from PerkOracle's
`workshop/steamugc/`; only the constants at the top of `publish_ugc.py` differ.

Published item: see `published_id.txt` (written by the first `--create`).

## One-time setup

The native binaries and the SteamworksPy python package are environment-local
and **git-ignored** — recreate them in this folder (`workshop/steamugc/`):

1. **SteamworksPy** (python package + native shim) — from
   <https://github.com/philippj/SteamworksPy>:
   - Copy the repo's `steamworks/` package folder here.
   - Copy `redist/windows/SteamworksPy64.dll` here (built against Steamworks SDK 1.64).
   On this machine: copy both from `E:\DEV\PhoenixPoint\PerkOracle\workshop\steamugc\`.

2. **steam_api64.dll** — must export `SteamInternal_SteamAPI_Init`
   (Steamworks SDK ≈ 1.57 or newer). Phoenix Point's own
   `…\Phoenix Point\PhoenixPointWin64_Data\Plugins\x86_64\steam_api64.dll` is
   **too old** and fails to load the shim (`WinError 127`). Use a newer one,
   e.g. copied from another recent Steam game:
   `D:\Steam\steamapps\common\Slay the Spire 2\data_sts2_windows_x86_64\steam_api64.dll`.

3. **steam_appid.txt** — a file containing exactly `839770` (no newline), so the
   API binds to Phoenix Point.

Expected contents of this folder afterwards (★ = git-ignored / local-only):

```
publish_ugc.py        (committed)
init_test.py          (committed)
published_id.txt      (committed once it exists — public id, not a secret)
README.md             (committed)
steamworks/        ★  (vendored from philippj/SteamworksPy)
SteamworksPy64.dll ★  (vendored shim, SDK 1.64)
steam_api64.dll    ★  (SDK >=1.57 with SteamInternal_SteamAPI_Init)
steam_appid.txt    ★  (839770)
```

## Run

Steam must be running and logged in as the item **owner**. Run from the repo root
(the script chdirs itself).

```powershell
# Smoke test — confirms bind to appid 839770 + logged-in user:
python workshop\steamugc\init_test.py

# First publish (creates a brand-new HIDDEN item, then uploads ~245 MB):
python workshop\steamugc\publish_ugc.py --create --changenote "v1.4.1 initial release" --visibility hidden

# Make it public once the page has been reviewed, and every later update:
python workshop\steamugc\publish_ugc.py --update --item <id> --changenote "v1.4.1" --visibility public
python workshop\steamugc\publish_ugc.py --update --item <id> --changenote "v1.5.0"
```

Options: `--visibility public|friends|private|hidden` (omitted = unchanged;
`hidden` == `private`), `--tags a,b,c` (default none), `--changenote "..."`.

On success the script writes `published_id.txt` and stamps the id into
`../renderforge.vdf` (the SteamCMD fallback descriptor).

## Notes

- If `CreateItem` reports the **workshop legal agreement** flag, accept it once
  in the browser/Steam client at the item URL; the script prints a clear notice.
- `WORKSHOP_TAGS = []` on purpose — none of Phoenix Point's tags (Geoscape,
  Tactical, Difficulty, Gameplay, Bionics, Mutations) fits a graphics mod, and an
  unknown tag can fail the submit.
- Gallery screenshots are **not** set by the UGC API (this SteamworksPy build
  lacks `AddItemPreviewFile`); add them via the item's web page.
