"""
Headless Steam Workshop publisher for the Renderforge Phoenix Point mod.

Rides the ALREADY-RUNNING, logged-in Steam client via SteamworksPy (no
username/password) -- the same auth model the official PPWorkshopTool uses.
Copied from PerkOracle's workshop/steamugc/publish_ugc.py; only the constants
below differ.

Requirements (all live next to this script, in workshop/steamugc/):
  * steamworks/                 (the SteamworksPy python package)
  * SteamworksPy64.dll          (native ctypes shim, Steamworks SDK 1.64)
  * steam_api64.dll             (must export SteamInternal_SteamAPI_Init,
                                 i.e. SDK >= ~1.57; see README.md)
  * steam_appid.txt             (contains exactly: 839770)
Steam must be running and logged in as the account that will OWN the item.

Usage:
  # First publish (creates a brand-new item, HIDDEN so the page can be reviewed):
  python publish_ugc.py --create --changenote "v1.4.1 initial release" --visibility hidden

  # Subsequent updates (re-upload content to an existing item):
  python publish_ugc.py --update --item 1234567890 --changenote "v1.5.0"
  # ... add --visibility public the first time to make the reviewed page public.

On success: prints the publishedfileid + item URL, writes published_id.txt,
and stamps the id into ../renderforge.vdf (SteamCMD fallback path).
"""
import argparse
import os
import sys
import time

# --- Resolve paths BEFORE any chdir (content/preview must be absolute) --------
HERE = os.path.dirname(os.path.abspath(__file__))            # workshop/steamugc
WORKSHOP_DIR = os.path.dirname(HERE)                          # workshop
REPO_ROOT = os.path.dirname(WORKSHOP_DIR)                     # repo root

CONTENT_FOLDER = os.path.join(WORKSHOP_DIR, "Dist")
PREVIEW_FILE = os.path.join(WORKSHOP_DIR, "image", "steam_preview.jpg")
LOCALE_DIR = os.path.join(WORKSHOP_DIR, "locale")
VDF_FILE = os.path.join(WORKSHOP_DIR, "renderforge.vdf")

# Per-language store descriptions, in the order they are pushed. Steam shows
# each viewer the description matching their client language; "english" is the
# default/fallback for any locale not listed. (Steam API language codes.)
# https://partner.steamgames.com/doc/store/localization/languages
LOCALE_DESCRIPTIONS = [
    ("english", "description.english.txt"),
    ("russian", "description.russian.txt"),
    ("german", "description.german.txt"),
    ("french", "description.french.txt"),
    ("spanish", "description.spanish.txt"),
    ("italian", "description.italian.txt"),
    ("polish", "description.polish.txt"),
    ("schinese", "description.schinese.txt"),
]
PUBLISHED_ID_FILE = os.path.join(HERE, "published_id.txt")

APP_ID = 839770
TITLE = "Renderforge"

# Global Workshop tags (NOT per-language). Phoenix Point's valid tags are:
# Geoscape, Tactical, Difficulty, Gameplay, Bionics, Mutations. None of them
# describes a graphics/engine mod, so Renderforge ships with NO tags (an unknown
# tag can fail the submit). Applied during the english pass of
# --localize-descriptions when non-empty (tags are item-global).
WORKSHOP_TAGS = []

# Native libs + steam_appid.txt resolve relative to CWD inside SteamworksPy,
# so run from this directory.
os.chdir(HERE)
sys.path.insert(0, HERE)

from steamworks import STEAMWORKS                                      # noqa: E402
from steamworks.enums import (                                        # noqa: E402
    EWorkshopFileType,
    ERemoteStoragePublishedFileVisibility,
)

ERESULT_OK = 1  # steamworks.enums.EResult.OK

# "hidden" is what the Steam web UI calls PRIVATE.
VISIBILITY_MAP = {
    "public": ERemoteStoragePublishedFileVisibility.PUBLIC,
    "friends": ERemoteStoragePublishedFileVisibility.FRIENDS_ONLY,
    "private": ERemoteStoragePublishedFileVisibility.PRIVATE,
    "hidden": ERemoteStoragePublishedFileVisibility.PRIVATE,
}

# The Full content folder is ~245 MB; give the upload half an hour.
UPLOAD_TIMEOUT_S = 1800.0


def build_description() -> str:
    """Default (english) description for create/update.

    Per-language descriptions live in workshop/locale/ and are pushed
    separately via --localize-descriptions; english is the Steam fallback.
    """
    en_path = os.path.join(LOCALE_DIR, "description.english.txt")
    with open(en_path, "r", encoding="utf-8") as f:
        combined = f.read().strip()
    # Steam's limit is 8000 *bytes* (it reports "ASCII characters" but counts
    # the UTF-8 byte length). Multi-byte Cyrillic/CJK costs >1 byte per char,
    # so validate the encoded length, not the character count.
    nbytes = len(combined.encode("utf-8"))
    if nbytes > 8000:
        raise SystemExit(
            f"Combined description is {nbytes} UTF-8 bytes "
            f"({len(combined)} chars), exceeds Steam's 8000-byte limit."
        )
    return combined


def pump_until(steam, holder: dict, timeout: float, label: str) -> dict:
    """Pump RunCallbacks until `holder` is populated by the async callback."""
    deadline = time.time() + timeout
    while not holder:
        steam.run_callbacks()
        if time.time() > deadline:
            raise SystemExit(f"TIMEOUT waiting for {label} after {timeout:.0f}s")
        time.sleep(0.1)
    return holder


def create_item(steam) -> dict:
    """CreateItem (async). Returns dict with id + legal-agreement flag."""
    holder: dict = {}

    def on_created(result):
        holder["result"] = int(result.result)
        holder["id"] = int(result.publishedFileId)
        holder["needs_legal"] = bool(result.userNeedsToAcceptWorkshopLegalAgreement)

    print(f"[create] CreateItem(app={APP_ID}, COMMUNITY) ...")
    steam.Workshop.CreateItem(
        APP_ID, EWorkshopFileType.COMMUNITY,
        callback=on_created, override_callback=True,
    )
    pump_until(steam, holder, timeout=60.0, label="CreateItemResult_t")

    if holder["result"] != ERESULT_OK:
        raise SystemExit(
            f"CreateItem FAILED: EResult={holder['result']} "
            f"(see steamworks.enums.EResult). No item was created."
        )
    print(f"[create] OK -> publishedfileid={holder['id']}")
    return holder


def add_gallery_previews(steam, handle: int, gallery: list) -> bool:
    """Try to ADD extra gallery preview images via AddItemPreviewFile.

    Returns True if the images were submitted to the binding, False if the
    binding does not expose AddItemPreviewFile (the headless add is then
    impossible and must be done on the Workshop web page instead).

    NOTE: AddItemPreviewFile ADDS a new preview on every call. Re-running this
    script with the same --gallery images would DUPLICATE them on the item.
    """
    if not gallery:
        return True  # nothing requested

    add_fn = getattr(steam.Workshop, "AddItemPreviewFile", None)
    if not callable(add_fn):
        print("[gallery] SKIPPED: this SteamworksPy build does not expose "
              "ISteamUGC::AddItemPreviewFile.")
        print("[gallery] The following screenshots must be added MANUALLY on the "
              "Workshop web page (Add/Edit Images):")
        for img in gallery:
            print(f"[gallery]   - {img}")
        return False

    # Defensive: only reached if a future rebuild adds the binding.
    for img in gallery:
        if not os.path.exists(img):
            raise SystemExit(f"Gallery image not found: {img}")
        print(f"[gallery] AddItemPreviewFile(image) <- {img}")
        add_fn(handle, img, 0)  # k_EItemPreviewType_Image == 0
    print("[gallery] NOTE: re-running with the same images would DUPLICATE "
          "these previews.")
    return True


def submit_update(steam, published_file_id: int, description: str,
                  visibility, changenote: str, tags: list, gallery: list) -> dict:
    """StartItemUpdate -> set fields -> SubmitItemUpdate (async, uploads).

    ``visibility`` may be None: the item's current visibility is then left
    untouched (a brand-new item starts private/hidden on Steam's side).
    """
    handle = steam.Workshop.StartItemUpdate(APP_ID, published_file_id)
    print(f"[update] StartItemUpdate handle={handle}")

    steam.Workshop.SetItemTitle(handle, TITLE)
    steam.Workshop.SetItemDescription(handle, description)
    steam.Workshop.SetItemContent(handle, CONTENT_FOLDER)
    steam.Workshop.SetItemPreview(handle, PREVIEW_FILE)
    if visibility is not None:
        steam.Workshop.SetItemVisibility(handle, visibility)
    if tags:
        steam.Workshop.SetItemTags(handle, tags)

    gallery_added = add_gallery_previews(steam, handle, gallery)
    print(f"[update] title/desc/content/preview/visibility set "
          f"(content={CONTENT_FOLDER}, preview={PREVIEW_FILE}, "
          f"visibility={visibility.name if visibility is not None else 'unchanged'}, "
          f"tags={tags or 'none'})")

    holder: dict = {}

    def on_submitted(result):
        holder["result"] = int(result.result)
        holder["id"] = int(result.publishedFileId)
        holder["needs_legal"] = bool(result.userNeedsToAcceptWorkshopLegalAgreement)

    print(f"[update] SubmitItemUpdate(changenote={changenote!r}) ... (uploading)")
    steam.Workshop.SubmitItemUpdate(
        handle, changenote,
        callback=on_submitted, override_callback=True,
    )

    # Pump + log upload progress until the submit callback fires.
    deadline = time.time() + UPLOAD_TIMEOUT_S
    last_pct = -1
    while not holder:
        steam.run_callbacks()
        try:
            prog = steam.Workshop.GetItemUpdateProgress(handle)
            pct = int(prog["progress"] * 100)
            if pct != last_pct and prog["total"]:
                print(f"[update] {prog['status'].name} "
                      f"{prog['processed']}/{prog['total']} ({pct}%)")
                last_pct = pct
        except Exception:
            pass
        if time.time() > deadline:
            raise SystemExit(
                f"TIMEOUT waiting for SubmitItemUpdateResult_t after {UPLOAD_TIMEOUT_S:.0f}s")
        time.sleep(0.25)

    if holder["result"] != ERESULT_OK:
        raise SystemExit(
            f"SubmitItemUpdate FAILED: EResult={holder['result']} "
            f"(see steamworks.enums.EResult)."
        )
    holder["gallery_added"] = gallery_added
    print(f"[update] OK -> upload committed for id={holder['id']}")
    return holder


def submit_description_for_language(steam, published_file_id: int, lang_code: str,
                                    description: str, changenote: str,
                                    tags: list = None) -> int:
    """Push ONE localized description for ``lang_code`` and return its EResult.

    Sets the per-language title (to the global English TITLE, so every language
    shows the same name) plus the description (no content/preview/visibility
    touched), so nothing is re-uploaded. SetItemTitle is REQUIRED per language:
    scoping the update with SetItemUpdateLanguage otherwise writes an EMPTY title
    for that language, leaving the store-page heading blank on non-english pages.
    If ``tags`` is given, also sets the item's (global) Workshop tags on this
    update -- intended for the english/default pass only, since tags are not
    per-language. Blocks on the SubmitItemUpdateResult_t callback. Returns the
    int EResult.
    """
    handle = steam.Workshop.StartItemUpdate(APP_ID, published_file_id)
    steam.Workshop.SetItemUpdateLanguage(handle, lang_code)
    steam.Workshop.SetItemTitle(handle, TITLE)
    steam.Workshop.SetItemDescription(handle, description)
    if tags:
        ok = steam.Workshop.SetItemTags(handle, tags)
        print(f"[locale] {lang_code:<9}    SetItemTags({tags}) -> {ok}", flush=True)

    holder: dict = {}

    def on_submitted(result):
        holder["result"] = int(result.result)

    steam.Workshop.SubmitItemUpdate(
        handle, changenote,
        callback=on_submitted, override_callback=True,
    )
    # No content upload here, so this resolves quickly; cap at 120s anyway.
    pump_until(steam, holder, timeout=120.0,
               label=f"SubmitItemUpdateResult_t[{lang_code}]")
    return holder["result"]


def localize_descriptions(steam, published_file_id: int, changenote: str,
                          tags: list = None) -> dict:
    """Iterate LOCALE_DESCRIPTIONS, push each, collect per-language EResult.

    english is pushed first (it is the default/fallback). If ``tags`` is given,
    the (global) Workshop tags are set ONCE on the english pass. A failure on
    one language is recorded and the rest still run. Returns {lang_code: eresult}.
    """
    results: dict = {}
    for lang_code, filename in LOCALE_DESCRIPTIONS:
        path = os.path.join(LOCALE_DIR, filename)
        with open(path, "r", encoding="utf-8") as f:
            text = f.read().strip()
        nbytes = len(text.encode("utf-8"))
        if nbytes > 8000:
            raise SystemExit(
                f"{filename} is {nbytes} UTF-8 bytes, exceeds Steam's 8000-byte limit."
            )
        print(f"[locale] {lang_code:<9} <- {filename} ({nbytes} bytes) ...", flush=True)
        # Tags are item-global; set them only on the first (english) pass.
        pass_tags = tags if lang_code == "english" else None
        try:
            eresult = submit_description_for_language(
                steam, published_file_id, lang_code, text, changenote, pass_tags)
        except SystemExit as e:
            print(f"[locale] {lang_code:<9} ERROR: {e}")
            results[lang_code] = -1
            continue
        results[lang_code] = eresult
        status = "OK" if eresult == ERESULT_OK else f"FAILED (EResult={eresult})"
        print(f"[locale] {lang_code:<9} -> {status}")
    return results


def persist_id(published_file_id: int) -> None:
    with open(PUBLISHED_ID_FILE, "w", encoding="utf-8") as f:
        f.write(str(published_file_id))
    print(f"[persist] wrote {PUBLISHED_ID_FILE}")

    # Stamp the id into the SteamCMD .vdf (replace placeholder or prior id).
    try:
        with open(VDF_FILE, "r", encoding="utf-8") as f:
            vdf = f.read()
        import re
        new_vdf = re.sub(
            r'("publishedfileid"\s*")[^"]*(")',
            lambda m: f'{m.group(1)}{published_file_id}{m.group(2)}',
            vdf,
        )
        if new_vdf != vdf:
            with open(VDF_FILE, "w", encoding="utf-8") as f:
                f.write(new_vdf)
            print(f"[persist] stamped publishedfileid={published_file_id} into {VDF_FILE}")
        else:
            print(f"[persist] WARNING: no publishedfileid line replaced in {VDF_FILE}")
    except FileNotFoundError:
        print(f"[persist] WARNING: {VDF_FILE} not found; skipped vdf stamp")


def main() -> int:
    ap = argparse.ArgumentParser(description="Headless SteamworksPy Workshop publisher for Renderforge")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--create", action="store_true",
                      help="Create a brand-new Workshop item, then upload content.")
    mode.add_argument("--update", action="store_true",
                      help="Update an existing item (requires --item).")
    mode.add_argument("--localize-descriptions", action="store_true",
                      help="Push per-language store descriptions from workshop/locale/ "
                           "for an existing item (requires --item). Does NOT touch the "
                           "title, content, preview or visibility.")
    ap.add_argument("--item", type=int, default=0,
                    help="Existing publishedfileid (required with --update / "
                         "--localize-descriptions).")
    ap.add_argument("--changenote", default="Renderforge update",
                    help="Change note shown in the item's history.")
    ap.add_argument("--visibility", choices=list(VISIBILITY_MAP), default=None,
                    help="Item visibility. Omitted = leave unchanged (a new item "
                         "starts hidden). 'hidden' == 'private'.")
    ap.add_argument("--tags", default="",
                    help="Comma-separated Workshop tags (default: none).")
    ap.add_argument("--gallery", nargs="+", default=[], metavar="IMG",
                    help="Additional gallery preview images to ADD to the item. "
                         "NOTE: this SteamworksPy build does not expose "
                         "ISteamUGC::AddItemPreviewFile, so these are reported as "
                         "a manual web step instead of being uploaded headlessly.")
    args = ap.parse_args()

    if (args.update or args.localize_descriptions) and not args.item:
        ap.error("--update / --localize-descriptions requires --item <publishedfileid>")

    # ------------------------------------------------------------------
    # Localize-descriptions mode: push per-language store descriptions only.
    # No content/preview/title/visibility touched -> early return.
    # ------------------------------------------------------------------
    if args.localize_descriptions:
        missing = [fn for _, fn in LOCALE_DESCRIPTIONS
                   if not os.path.exists(os.path.join(LOCALE_DIR, fn))]
        if missing:
            raise SystemExit(f"Missing locale description file(s) in {LOCALE_DIR}: {missing}")

        print("=" * 70)
        print("Renderforge Workshop publisher (SteamworksPy, headless)")
        print(f"  mode        : localize-descriptions")
        print(f"  app_id      : {APP_ID}")
        print(f"  item        : {args.item}")
        print(f"  locale dir  : {LOCALE_DIR}")
        print(f"  languages   : {', '.join(c for c, _ in LOCALE_DESCRIPTIONS)}")
        print(f"  tags        : {WORKSHOP_TAGS or 'none'} (set on english pass)")
        print(f"  changenote  : {args.changenote}")
        print("=" * 70)

        steam = STEAMWORKS()
        steam.initialize()
        print(f"[init] SteamworksPy ready: appid={steam.app_id}, "
              f"SteamID={steam.Users.GetSteamID()}, "
              f"user={steam.Friends.GetPlayerName().decode(errors='replace')}")
        if steam.app_id != APP_ID:
            raise SystemExit(f"Bound to wrong appid {steam.app_id}, expected {APP_ID}")

        results = localize_descriptions(steam, args.item, args.changenote,
                                        tags=WORKSHOP_TAGS)

        url = f"https://steamcommunity.com/sharedfiles/filedetails/?id={args.item}"
        print("=" * 70)
        print("LOCALIZE RESULT")
        print(f"  item URL : {url}")
        for lang_code, _ in LOCALE_DESCRIPTIONS:
            r = results.get(lang_code)
            status = "EResult.OK" if r == ERESULT_OK else f"FAILED (EResult={r})"
            print(f"  {lang_code:<9}: {status}")
        en_ok = results.get("english") == ERESULT_OK
        print(f"  tags     : {WORKSHOP_TAGS or 'none'} "
              f"{'applied on english pass (EResult.OK)' if en_ok else 'NOT confirmed (english pass failed)'}")
        print("=" * 70)

        steam.unload()
        all_ok = all(r == ERESULT_OK for r in results.values())
        return 0 if all_ok else 1

    # Sanity-check inputs.
    for p, what in [(CONTENT_FOLDER, "content folder"), (PREVIEW_FILE, "preview"),
                    (os.path.join(LOCALE_DIR, "description.english.txt"),
                     "english description")]:
        if not os.path.exists(p):
            raise SystemExit(f"Missing {what}: {p}")
    preview_bytes = os.path.getsize(PREVIEW_FILE)
    if preview_bytes >= 1024 * 1024:
        raise SystemExit(f"Preview {PREVIEW_FILE} is {preview_bytes} bytes; Steam caps it at 1 MB "
                         f"(run workshop/image/make-previews.ps1).")

    visibility = VISIBILITY_MAP[args.visibility] if args.visibility else None
    tags = [t.strip() for t in args.tags.split(",") if t.strip()]
    description = build_description()

    print("=" * 70)
    print("Renderforge Workshop publisher (SteamworksPy, headless)")
    print(f"  mode        : {'create' if args.create else 'update'}")
    print(f"  app_id      : {APP_ID}")
    print(f"  content     : {CONTENT_FOLDER}")
    print(f"  preview     : {PREVIEW_FILE}")
    print(f"  visibility  : {args.visibility or 'unchanged'}")
    print(f"  changenote  : {args.changenote}")
    print(f"  desc length : {len(description)} chars")
    print("=" * 70)

    steam = STEAMWORKS()
    steam.initialize()
    print(f"[init] SteamworksPy ready: appid={steam.app_id}, "
          f"SteamID={steam.Users.GetSteamID()}, user={steam.Friends.GetPlayerName().decode(errors='replace')}")
    if steam.app_id != APP_ID:
        raise SystemExit(f"Bound to wrong appid {steam.app_id}, expected {APP_ID}")

    needs_legal = False
    if args.create:
        created = create_item(steam)
        published_file_id = created["id"]
        needs_legal = needs_legal or created["needs_legal"]
    else:
        published_file_id = args.item
        print(f"[update] using existing publishedfileid={published_file_id}")

    submitted = submit_update(steam, published_file_id, description,
                              visibility, args.changenote, tags, args.gallery)
    needs_legal = needs_legal or submitted["needs_legal"]
    published_file_id = submitted["id"] or published_file_id

    persist_id(published_file_id)

    url = f"https://steamcommunity.com/sharedfiles/filedetails/?id={published_file_id}"
    print("=" * 70)
    print("PUBLISH RESULT")
    print(f"  publishedfileid : {published_file_id}")
    print(f"  item URL        : {url}")
    print(f"  upload          : committed (SubmitItemUpdate returned EResult.OK)")
    if args.gallery:
        if submitted.get("gallery_added"):
            print(f"  gallery         : {len(args.gallery)} preview image(s) submitted via AddItemPreviewFile")
        else:
            print(f"  gallery         : NOT added headlessly (binding lacks AddItemPreviewFile)")
            print(f"                    add these MANUALLY on the Workshop web page:")
            for img in args.gallery:
                print(f"                      - {img}")
    if needs_legal:
        print("  ACTION REQUIRED : Steam reports you must ACCEPT THE WORKSHOP LEGAL")
        print("                    AGREEMENT once. Open the item URL above in a browser")
        print("                    (or the Steam client) and accept the agreement, or the")
        print("                    item may stay hidden. This is a one-time per-account step.")
    else:
        print("  legal agreement : not flagged")
    print("=" * 70)

    steam.unload()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
