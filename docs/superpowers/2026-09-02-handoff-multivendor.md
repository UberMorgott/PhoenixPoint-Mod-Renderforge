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
| 2 DLSS on D3D12 (IDevice seam, Device12, sharpen PSO, Plugins staging) | Code done, reviewed. Image correct. **Open: DEVICE_REMOVED after 1–5 min in tactical** — see below |
| 3 FSR 4.1/3.1.5 via ffx-api | Code done + reviewed + fixes; probe OK (3.1.5 on RTX). In-game Task 10 NOT run |
| 4 XeSS 3 (DP4a) | Code done + reviewed + fixes; probe OK. In-game Task 9 NOT run (jitter A/B open) |
| 5 Frame generation (FSR-FG, XeSS-FG, DLSS-G) | Plan only. Starts with the Present-hook / shadow-swapchain spike. Blocked on Phase 2 stability |
| 6 Packaging | Tasks 1–6 DONE (`build\release.ps1`, per-vendor zips, README, RELEASING.md). Tasks 7–8 (GitHub release, Workshop) USER-GATED, not run |

## The open D3D12 problem (critical path)

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

## Next steps, in order

1. Finish/verify the owned-resource fix; pass the D3D12 gates (DLSS, FSR, XeSS soaks + screenshots).
2. Phase 3 Task 10 + Phase 4 Task 9 in-game (incl. jitter sign A/B for FSR/XeSS).
3. Phase 5 (frame generation) per its plan: Task 1 spike first.
4. Re-run `build\release.ps1`, then Phase 6 Tasks 7–8 with the user's explicit OK.
