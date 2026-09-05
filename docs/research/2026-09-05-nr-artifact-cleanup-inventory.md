# NR artifact cleanup inventory — 2026-09-05

- Scope: inventory only; zero deletions. Canonical findings: [retirement dossier](2026-09-05-dlss5-retirement-dossier.md). Exact selections and absolute paths: `D:\RenderforgeWork\nr-retirement-inventory\cleanup-manifest.json`.
- Generated candidates total: 28 directories plus45 selected documentation files; 3009 files /9,186,772,488 bytes. This is a provenance total, not permission to bypass the execution hold below. Snapshot sizes may drift.
- `docs/shots`:1719 files /8,144,837,773 bytes (1716 PNG,3 diagnostic analysis scripts), matching the original request inventory. No user source art found. Current explicit generated-artifact cleanup authorization supersedes the earlier no-deletion-without-permission restriction.
- Native owner reported automatic approval review rejected contained stale build removal as `blocked by policy`. Do not retry any build/**, installed-runtime or refs removal through this inventory. These entries describe ownership/size only; distinct screenshot and D: temporary candidates require the lead cleanup brief.
- No selected candidate has unresolved provenance. Mixed rollback backups are retained until deployment validation; unlisted directories are outside this inventory. Zero reparse points were observed in measured directory trees; recheck resolved containment and reparse points immediately before any later removal.

## Exact directory roots

| Resolved absolute path | Classification | Files | Bytes |
| --- | --- | ---: | ---: |
| `E:\DEV\PhoenixPoint\Renderforge\build\face-multiview-toolchain` | generated; build-owner hold | 33 | 216453660 |
| `D:\RenderforgeTools\face-multiview` | candidate | 31 | 156582349 |
| `E:\DEV\PhoenixPoint\Renderforge\build\face-regeneration-prototype` | generated; build-owner hold | 222 | 273274124 |
| `E:\DEV\PhoenixPoint\Renderforge\build\procedural-head-prototype` | generated; build-owner hold | 9 | 32478 |
| `E:\DEV\PhoenixPoint\Renderforge\build\numeric-anatomy-prototype` | generated; build-owner hold | 4 | 19287 |
| `E:\DEV\PhoenixPoint\Renderforge\build\fg-settings-diagnosis-20260905` | generated; build-owner hold | 251 | 13754896 |
| `E:\DEV\PhoenixPoint\Renderforge\build\mask-proof` | generated; build-owner hold | 149 | 115034893 |
| `D:\RenderforgeWork\procedural-head` | candidate | 210 | 154847636 |
| `D:\RenderforgeWork\numeric-anatomy` | candidate | 174 | 92855608 |
| `D:\RenderforgeWork\head-rest-reconstruction` | candidate | 111 | 3055017 |
| `E:\DEV\PhoenixPoint\Renderforge\docs\shots` | candidate | 1719 | 8144837773 |
| `E:\DEV\PhoenixPoint\refs\DLSS-NR` | code-worker | 1 | 165840496 |
| `D:\Steam\steamapps\common\Phoenix Point\Mods\Renderforge\RenderforgeNR` | code-worker-after-game-closed | 2 | 165851248 |
| `E:\DEV\PhoenixPoint\Renderforge\build\out\RenderforgeNR` | code-worker | 2 | 165851248 |
| `D:\RenderforgeWork\deploy-b62d161` | retain-until-deploy-validated | 54 | 421885725 |
| `D:\RenderforgeWork\campaign-load-diagnosis` | retain | 4 | 14592 |
| `D:\RenderforgeWork\fonts-final-proof` | retain | 124 | 13481188 |
| `D:\RenderforgeWork\fonts-lifecycle-proof` | retain | 35 | 22909094 |
| `D:\Renderforge-work\scene-style` | retain | 202 | 376337636 |
| `D:\Renderforge-work\font-integration` | retain | 503 | 140898041 |
| `D:\RenderforgeWork\nr-retirement-inventory` | retain | 0 | 0 |
| `E:\DEV\PhoenixPoint\Renderforge\build\character-probe` | generated; build-owner hold | 3 | 145515 |
| `E:\DEV\PhoenixPoint\Renderforge\build\character-probe2` | generated; build-owner hold | 3 | 146027 |
| `E:\DEV\PhoenixPoint\Renderforge\build\character-probe3` | generated; build-owner hold | 3 | 147099 |
| `E:\DEV\PhoenixPoint\Renderforge\build\head-probe1` | generated; build-owner hold | 3 | 174343 |
| `E:\DEV\PhoenixPoint\Renderforge\build\head-probe2` | generated; build-owner hold | 3 | 174363 |
| `E:\DEV\PhoenixPoint\Renderforge\build\head-probe3` | generated; build-owner hold | 3 | 174887 |
| `E:\DEV\PhoenixPoint\Renderforge\build\head-probe4` | generated; build-owner hold | 3 | 175527 |
| `E:\DEV\PhoenixPoint\Renderforge\build\head-probe5` | generated; build-owner hold | 3 | 175571 |
| `E:\DEV\PhoenixPoint\Renderforge\build\head-probe6` | generated; build-owner hold | 3 | 176135 |
| `E:\DEV\PhoenixPoint\Renderforge\build\mask-probe1` | generated; build-owner hold | 3 | 160899 |
| `E:\DEV\PhoenixPoint\Renderforge\build\mask-probe2` | generated; build-owner hold | 3 | 161523 |
| `E:\DEV\PhoenixPoint\Renderforge\build\mask-probe3` | generated; build-owner hold | 3 | 164459 |
| `E:\DEV\PhoenixPoint\Renderforge\build\mask-probe4` | generated; build-owner hold | 3 | 165627 |
| `E:\DEV\PhoenixPoint\Renderforge\build\mask-probe5` | generated; build-owner hold | 3 | 165627 |
| `E:\DEV\PhoenixPoint\Renderforge\build\mask-probe6` | generated; build-owner hold | 3 | 165707 |
| `E:\DEV\PhoenixPoint\Renderforge\build\mask-probe7` | generated; build-owner hold | 3 | 166823 |
| `E:\DEV\PhoenixPoint\Renderforge\build\mask-probe8` | generated; build-owner hold | 3 | 167463 |

## Selective files and preservation

- Candidate media/prompts/obsolete diagnostic source and caches:12 files/3,930,029 bytes in `docs/character-mask-proof`;13/6,464,380 in `docs/face-regeneration-prototype`;15/2,445,161 in `docs/procedural-head-prototype`;4/375,673 in `docs/numeric-face-anatomy-prototype`. Exact individual paths are in manifest `fileGroups[].paths`; do not delete the containing directories wholesale.
- Candidate obsolete initializer: `E:\DEV\PhoenixPoint\Renderforge\docs\Initialize-FaceMultiviewStorage.ps1`,1929 bytes. Probe source includes `AnatomyProbe.cs`, `ProceduralHeadProbe.cs` and their projects; removed production diagnostics invalidate their live entry points. Source remains recoverable from history; the dossier preserves outcomes.
- Inventory-only flat build outputs:41 diagnostic JSON/log files /3,481,629 bytes selected by known character/head/mask/foreground-FG prefixes, excluding Markdown. They are not part of the candidate total above and remain under build-owner hold.
- Retain all historical Markdown and primary `evidence.json`, `pixel-proof.json`, `roundtrip.json`; generated obj JSON is a cache, not primary evidence. Retain character inventory JSON and the new dossier/inventory.
- Preserve the requested font comparison `E:\DEV\PhoenixPoint\Renderforge\docs\research\font-final-proof-2026-09-05\comparison-2k-live-4k-offscreen.png`, its report/structured evidence, font lifecycle proof and current font integration artifacts.
- Preserve ordinary LUT/style validation: `docs/lut-cinema-pack`, `docs/scene-stylization`, their Markdown reports, `D:\RenderforgeWork\lut-pack` and `D:\Renderforge-work\scene-style`. The hyphenated `Renderforge-work` and `RenderforgeWork` are distinct roots.
- Preserve game saves/configurations, user inputs, source art, current ordinary-provider builds and unrelated tool caches. `D:\RenderforgeWork\deploy-b62d161` is a mixed rollback backup, not wholesale NR junk.
- Provenance sources: original pasted task attachment; character/mask, albedo, toolchain, micro-normal, numeric anatomy and rest-mesh reports linked in the dossier; exact file extension/name/script-reference inventory. No game call, build or global settings mutation was performed for this documentation task.

## Execution receipt after dossier commit

- Completed distinct authorized batch:2290 files /8,565,395,555 bytes removed; five non-build candidate roots plus45 explicitly selected docs files. Receipt: `D:\RenderforgeWork\nr-retirement-inventory\cleanup-receipt.json`; status `complete`, retained font hash unchanged.
- Remaining generated candidate inventory under build-owner hold:719 files /621,376,933 bytes. Additional41 flat build diagnostics, refs/build-output runtimes and installed runtime were excluded and untouched. Retention entries above remain in force.
- The original inventory totals are an audit snapshot, not a current existence claim. No automatic-review rejection was bypassed; no alternate deletion was attempted against held targets.
