"""
Smoke test: initialize SteamworksPy against the already-running, logged-in
Steam client and confirm it bound to Phoenix Point (appid 839770).

Run from this directory (steam_appid.txt + SteamworksPy64.dll + steam_api64.dll
must be in the working dir). Steam must be running and logged in.

    cd workshop/steamugc
    python init_test.py
"""
import os
import sys

# Ensure the local `steamworks` package and native DLLs (which live next to this
# file) are found regardless of the caller's CWD.
HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)
sys.path.insert(0, HERE)

from steamworks import STEAMWORKS  # noqa: E402


def main() -> int:
    sw = STEAMWORKS()           # loads SteamworksPy64.dll + reads steam_appid.txt
    sw.initialize()             # boots the Steam API against the running client

    app_id = sw.app_id
    steam_id = sw.Users.GetSteamID()
    persona = None
    try:
        persona = sw.Friends.GetPlayerName()
    except Exception as exc:  # noqa: BLE001
        persona = f"<unavailable: {exc}>"

    print(f"OK: SteamworksPy initialized")
    print(f"app_id          = {app_id}")
    print(f"IsSteamRunning  = {bool(sw.IsSteamRunning())}")
    print(f"SteamID         = {steam_id}")
    print(f"PersonaName     = {persona}")

    if app_id != 839770:
        print(f"WARNING: expected appid 839770, got {app_id}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
