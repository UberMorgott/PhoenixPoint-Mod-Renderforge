# Handoff — Renderforge multi-vendor / D3D12 / frame generation (2026-09-02)

Read this first in a fresh session. Everything below is committed on `main` of
`E:\DEV\PhoenixPoint\Renderforge` (inner repo, not pushed).

## Ground truth docs

- Spec: `docs\superpowers\specs\2026-09-02-multi-vendor-d3d12-design.md`
- Plans (one per phase, checkbox state is live):
  `docs\superpowers\plans\2026-09-02-phase1-renderer-switch.md` … `phase2-dlss-d3d12.md`,
  `phase3-fsr.md`, `phase4-xess.md`, `phase5-framegen.md`, `phase6-packaging.md`
- `docs\DESIGN.md`: sections "D3D12 probe", "Renderer switch", "Native plugin staging",
  "Upscaler providers", "Vendor SDKs on disk", "Packaging".
- Contract notes: `docs\research\dlss-ngx-d3d11-contract.md`, `fsr-ffx-api-d3d12-contract.md`,
  `xess-d3d12-contract.md`.

## Status per phase

| Phase | State |
|---|---|
| 1 Renderer switch (D3D11/D3D12 picker, restart, PPv2 D3D12 fix, greyed rows + native tooltips, overlay `Renderer:`) | DONE, in-game tested (A–E PASS) |
| 2 DLSS on D3D12 (IDevice seam, Device12, sharpen PSO, Plugins staging) | DONE. DEVICE_REMOVED fixed by `54af332` (owned resources, see below). Gate passed: DLAA 600 s + 3 loads, debug layer 0 mismatches on our lists. 2026-09-03: the D3D12 frame was ~2x darker (menu luma 28 vs 56) — NOT the shim (Passthrough copy was dark too); Unity's D3D12 present Blit decodes the sRGB outRT read but skips the backbuffer encode. Fixed in `DlssDriver.Make`: `outRT` = `RenderTextureReadWrite.Linear` on D3D12 only (D3D11 untouched). After: DLAA 56.5 / Quality 56.3 / FSR 56.1 / XeSS 56.2 / D3D11 DLAA 56.7, debug layer 0 errors on our lists (`DESIGN.md` owned-resource section, `docs\shots\darkfps\fix-*.png`) |
| 3 FSR 4.1/3.1.5 via ffx-api | DONE. In-game: FSR Quality 352 s soak + 1 load, `own-fsr.png` OK. Jitter A/B run 2026-09-02: indistinguishable on stills, defaults kept. `RfDbg::Attach` now in `Fsr12::Init` |
| 4 XeSS 3 (DP4a) | DONE. In-game: XeSS Quality 355 s + 1 load, `own-xess.png` OK. Jitter A/B run 2026-09-02: indistinguishable on stills, defaults kept |
| 5 Frame generation (FSR-FG, XeSS-FG, DLSS-G/MFG) | DONE + hardened. Commits: `55dbf41` (Present hook + spike), `e4915dd` (composition chain), `8a2b8a2` (Unity Present forwarding), `c57f6a0` (child HWND), `28f2013` (FSR-FG manual dispatch), `59e4cd0` (FSR-FG SDK swapchain on child), `fd6423a` (XeSS-FG + XeLL), `fd180d5` (DLSS-G Streamline manual hooking), `05dd6b3` (DLSS-G RCA: focus gate + copy fence + marker order + token FIFO), `aa53ba4` (verification + docs), `1a340d6` (Task 6 verification), `2ec662b`/`f0f003f`/`0397656`/`2c063cb`/`3c5c7df` (hardening round 2: teardown owned by main thread via `FgHostPump`/`Fg_Shutdown` ack, `Detach`/`DestroyDetached`, tearingDown gate, hook chain validation + owned ref + CAS unpatch, fence/ring result checks with `Quarantine`, SL pinned-for-process guard, XeSS DLL pin + markers in BeforePresent, Auto provider order NVIDIA DLSS→FSR→XeSS / Intel XeSS→FSR / else FSR→XeSS with terminal fallback, availability reasons from `Fg_Reason`, DllNotFound guard, FSR-FG 3x/4x refused with FG_ERR_UNSUPPORTED_MULTIPLIER, spike + composition/DComp + FSR manual dispatch + `RENDERFORGE_FG_CHAIN` DELETED, `deploy.ps1 -AllowRunning` rename-aside, `build-native.ps1` 310.7.* warn + non-NVIDIA probe warn). Task 6 results (Instance3, HEAD `aa53ba4` then hardened HEAD): FSR-FG X2 1.96-2.04, XeSS-FG X2 1.96-2.04, DLSS-G 1.00 unfocused (focus gate; 2/3/4x measured with dev plugin + runWhenNoFocus in Task 5 round 2), provider round-trip PASS, SR+FG combos PASS, D3D11 guard PASS, 10-min soak DLSS-Q + FSR-FG X2 with 3 loads PASS, quit path clean (exit ok, WerFault 0), Auto fallback verified with 0-byte `sl.common.dll` → FSR. Debug layer: 0 on our lists. Reviews: Codex thread `01a062e8-cda9-79b3-b4f7-8aa9bf741017`, files `C:\Temp\cx\{6158cf1a…,509f4590…,a44b7875…,8e06edc9…}.out.md` (may be gone); Opus reviews applied. Skipped: Codex "FG smoke probes in build-native" (out of scope) |
| 6 Packaging | Tasks 1–6 DONE (`build\release.ps1`, per-vendor zips, README, RELEASING.md). Release: `build\release.ps1 -WithFrameGen` → Core 138 KB, NVIDIA 50.6 MB, AMD 47.6 MB, Intel 73.1 MB, Full 171.4 MB, SHA256SUMS written; version still 1.1.0 (bump to 1.2.0 belongs to Phase 6 Task 7, user-gated). Without `-WithFrameGen` FG DLLs are NOT packed. Tasks 7–8 (GitHub release, Workshop) USER-GATED, not run |

## The D3D12 resource-state problem — SOLVED (`54af332`, `native\D3D12Owned.h`, DESIGN.md contract)

- Measured with the debug layer: Unity 2019.4 v5 never transitions a RT to `expected`; each Unity RT
  arrives in the state Unity's LAST use left it, deterministic per RT: `colorRT` RENDER_TARGET,
  `depthRT` RENDER_TARGET, `mvRT` COPY_DEST, `outRT` GENERIC_READ.
- Contract: shim owns typed twins (colorIn R8G8B8A8_UNORM, depthIn R32_FLOAT, mvIn R16G16_FLOAT,
  out +UAV; created COMMON). Per list: Unity RT pre-state → COPY_SOURCE/COPY_DEST → pre-state
  (declared `expected == current == pre-state`); owned inputs COMMON→COPY_DEST→NPSR→COMMON; owned
  out COMMON→UAV→COPY_SOURCE→COMMON. Ring waits on its own fence AND Unity's frame fence.
- Dead end (do not retry): `Texture2D.CreateExternalTexture` + `CommandBuffer.CopyTexture` —
  Unity refuses the base-format mismatch, TYPELESS → device removal. Managed driver unchanged.
- Unity's OWN lists still log ~1600 tracker mismatches/min under D3D12 (0 with the mod off) —
  not fatal, not ours.
- Leftover doc debt: `docs\research\fsr-ffx-api-d3d12-contract.md` and `xess-d3d12-contract.md`
  still describe the old declared-state model — update to the owned-resource contract.

## (history) The open D3D12 problem as it stood before the fix

- Allocator/list reuse bug FIXED (`b16ca5e`: ring owns its own fence; debug ids 552/553 = 0).
- Remaining root cause (measured with the D3D12 debug layer, `11d31e4`, comment in
  `native\Device12.cpp:71`): Unity 2019.4 `IUnityGraphicsD3D12v5::ExecuteCommandList` records
  the declared `current` but does NOT transition Unity RenderTextures to `expected`; a Unity RT's
  real pre-state varies per frame → every barrier on a Unity RT mismatches (id 527) → DEVICE_REMOVED.
- Fix in progress (agent "coder-d3d12-own", brief in the session transcript): shim owns the four
  per-frame D3D12 resources (colorIn/depthIn/mvIn at COPY_DEST, out at COPY_SOURCE), managed side
  wraps them via `Texture2D.CreateExternalTexture` and copies in/out with `CommandBuffer.CopyTexture`
  in ONE ordered command buffer; our lists barrier only our own memory. Fallback:
  `ALLOW_SIMULTANEOUS_ACCESS` + COMMON promotion. Codex-verified notes:
  `C:\Temp\cx\829b8b738b444eb3a384553783e16be5.out.md` (may be gone; key points are in the brief).
- Gate to pass before anything else: debug layer clean (id 527/552/553 = 0), 10-min DLAA tactical
  soak + 3 `start-mission` loads, then 5-min FSR and XeSS soaks, screenshots, plan boxes ticked.

## Tooling / environment facts

- Build: `.\build-native.ps1` (runs probes `dlss_probe` D3D11/D3D12/`--fsr`/`--xess`, all must PROBE OK),
  `dotnet build -c Release /p:PPRoot=D:\PP-Instance2` (0/0), `.\deploy.ps1` (targets Instance2; also
  stages the shim into `PhoenixPointWin64_Data\Plugins\x86_64`).
- Game for tests: `D:\PP-Instance2` ONLY (ContentTool session moved to `D:\PP-Instance3`). Never
  touch `D:\Steam\steamapps\common\Phoenix Point`. Launch
  `PhoenixPointWin64.exe -mods -force-d3d12` (+ `-force-d3d12-debug` and `$env:RENDERFORGE_D3D12_DEBUG=1`
  for the debug layer → `%TEMP%\renderforge-d3d12.log`). PPCLI: `E:\DEV\PhoenixPoint\PPCLI\PLAYBOOK.md`.
  Harness: session scratchpad `rf-test.ps1` (may be gone).
- ModConfig: `...LocalLow\Snapshot Games Inc\Phoenix Point\Steam\76561197996210592\ModConfig.json`
  (`Renderer` 1 = DX11, 2 = DX12; the mod relaunches itself if config ≠ running API).
- Windows "Graphics Tools" (D3D12 debug layer) is installed.
- SDKs in `E:\DEV\PhoenixPoint\refs\`: `FidelityFX-SDK` v2.3.0, `XeSS-sdk` v3.0.2, `Streamline` v2.12.0
  (+ `latest-dll\nvngx_dlssg.dll` 310.7.129 — the SDK copy is stale), `DLSS-sdk`. Drive C: is full —
  never download there.
- Reference screenshots (scratchpad, may be gone): D3D11 DLAA/Quality baselines were
  `p2-d3d11-dlaa.png` / `p2-d3d11-quality.png`; regenerate on D3D11 if needed.

## Working rules used this session

- Coders = `model: "fable"` (user request), reviewers/planners = opus. One one-shot agent per
  step, ≤15-line reports, explicit `git add <paths>`, conventional commits, commit-on-green on `main`.
- Greyed picker rows stay VISIBLE with the native tooltip reason (user requirement); never snap
  the picker back.
- Full set is the goal: DLSS SR/FG, FSR SR/FG, XeSS SR/FG, all latest SDKs; Vulkan is dead
  (no SPIR-V in the build); D3D11 XeSS (Arc-only) and FSR-DX11 forks are out.

## Tooling / environment facts (updated)

- Instance3 = `D:\PP-Instance3`, profile `...593`, parallel instances OK (Instance2 + Instance3 run
  side by side without conflict; each has its own mod folder and process).

## Next steps, in order

1. **User manual test**: mouse/keyboard input through the child HWND, DLSS-G production plugin 2x/3x/4x with the window focused, Alt-Tab/back, window resize, borderless — on Instance2/3.
2. Frame-pacing metric: CoV of `MsBetweenDisplayChange` via PresentMon still unmeasured for all three providers; capture once the user plays a session with FG on.
3. Resize under XeSS/DLSS-G not exercised (FSR only).
4. Phase 6 Tasks 7-8: version bump to 1.2.0, GitHub release + Workshop upload — user-gated, on the user's explicit OK.
Note: `Player.log` is shared between Instance2 and Instance3 (same LocalLow profile dir?) — when
ContentTool runs on Instance3 in parallel, prefer the mod's own log for evidence.
