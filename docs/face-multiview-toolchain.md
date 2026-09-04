# Local multiview texture toolchain probe

- Checked 2026-09-05. Status: **blocked before backend/model installation**. No generated multiview albedo and no game integration resulted from this probe.
- The intended experiment uses the actual exported head geometry and UVs, projects locally generated views onto that mesh, then exports one 2048 x 2048 albedo. It addresses projection alignment only; it cannot establish that geometry, normal maps, or the skin shader look realistic.
- Input directory: `build/face-regeneration-prototype/sofia-source/`; actual exports present: `head.json`, `_MainTex.png`, `_BumpMap.png`, `_MetallicGlossMap.png`, `_OcclusionMap.png`, `uv-wireframe.png`, `evidence.json`. `head.json` is not directly an OBJ import; conversion would still be required.

## Verified release mismatch

- The [official download page](https://stableprojectorz.com/) links [StableProjectorz v2.4.5 Minimal](https://files.stableprojectorz.com/StableProjectorz_v2_4_5%28Minimal%29.zip). Downloaded archive: 59,869,751 bytes; server `Last-Modified: Fri, 25 Jul 2025 18:10:25 GMT`.
- Archive SHA256: `759313396A0D5F1074782E8AD9FFD937239D8DBDA067944C7453DDEDBED08741`.
- Extracted executable `Stable Projectorz 2.4.5.exe`: Windows Authenticode status `Valid`, message `Signature verified.`
- Executable SHA256: `1BBF272329C768C3D42BF21EECB1CE9A2F337FD52A9E9D0833E27AEAC51B63D3`; signer `CN=Igor Aherne, O=Igor Aherne, S=Tyne and Wear, C=GB`; issuer `CN=Sectigo Public Code Signing CA R36, O=Sectigo Limited, C=GB`; certificate thumbprint `0042C3C728C4D43170FDC88F300437B2B80E7E02`.
- Exact executable: `E:\DEV\PhoenixPoint\Renderforge\build\face-multiview-toolchain\StableProjectorz-2.4.5\Stable Projectorz 2.4.5.exe`. Its working directory is `E:\DEV\PhoenixPoint\Renderforge\build\face-multiview-toolchain\StableProjectorz-2.4.5`.
- Extracted `GameAssembly.dll` SHA256: `84A7047E7CBA28C0F465E5566E8E8103B50EA523760E747451A0CD4A9BF6B33A`.
- Extracted `Stable Projectorz 2.4.5_Data/il2cpp_data/Metadata/global-metadata.dat` SHA256: `BDD19283AD8D75D24A6AF65FA8CCFB48EA0273B3BA06F9DC2E756255A2E03EC3`.
- Neither binary contains ASCII markers `SPZ_Agent_Bridge`, `SPZ_Agent_Protocol`, `SPZ_Agent_Tools`, or `agent-bridge`. The metadata **does** contain `Screenshot_MGR`, a positive control that application type names are readable.
- [Agent Bridge introduction](https://github.com/IgorAherne/StableProjectorz/commit/6867c4b3e9460d9f010a9c60b5a9cac70e438470) is dated 2026-07-31; [mandatory-token hardening](https://github.com/IgorAherne/StableProjectorz/commit/0562a10a08e212c270e1fa208543b591f754c4d0) is dated 2026-08-11. Both postdate the downloaded release.
- [Pinned current bridge documentation](https://github.com/IgorAherne/StableProjectorz/blob/37fecc906d07fade200ccf6abd1fc4af2e2868d3/Assets/_gm/Features/AgentBridge/README.md) describes `--agent-bridge`, a localhost TCP service, and calling `describe` first. Those source capabilities must not be assumed to exist in v2.4.5.
- Upstream main inspected at `37fecc906d07fade200ccf6abd1fc4af2e2868d3`; its [ProjectVersion.txt](https://github.com/IgorAherne/StableProjectorz/blob/37fecc906d07fade200ccf6abd1fc4af2e2868d3/ProjectSettings/ProjectVersion.txt) specifies Unity `6000.2.6f2`, revision `4a4dcaec6541`.

## Reproduce the static check

Run PowerShell from the repository root after extracting the official archive to the same ignored directory:

```powershell
$root = Join-Path (Get-Location) 'build/face-multiview-toolchain/StableProjectorz-2.4.5'
$metadata = Join-Path $root 'Stable Projectorz 2.4.5_Data/il2cpp_data/Metadata/global-metadata.dat'
$bytes = [IO.File]::ReadAllBytes($metadata)
$text = [Text.Encoding]::ASCII.GetString($bytes)
'SPZ_Agent_Bridge', 'SPZ_Agent_Protocol', 'SPZ_Agent_Tools', 'agent-bridge', 'Screenshot_MGR' |
    ForEach-Object { '{0}={1}' -f $_, $text.Contains($_) }
Get-AuthenticodeSignature -LiteralPath (Join-Path $root 'Stable Projectorz 2.4.5.exe') |
    Select-Object Status, StatusMessage
```

Observed output:

```text
SPZ_Agent_Bridge=False
SPZ_Agent_Protocol=False
SPZ_Agent_Tools=False
agent-bridge=False
Screenshot_MGR=True
Status: Valid
StatusMessage: Signature verified.
```

## Remaining prerequisite and local state

- A bridge-capable build or a separately verified GUI workflow is required before this tool can automate multiview generation. The application was not launched; no runtime `describe` response exists. No security settings were changed, and no binary patches or speculative bridge injection were attempted.
- [Forge Neo upstream](https://github.com/Haoming02/sd-webui-forge-classic/tree/neo) documents Python 3.13, CUDA-enabled PyTorch, `--api`, and warns against `--xformers` on RTX 50 GPUs. No Forge environment, CUDA runtime, or diffusion/ControlNet weights were installed because the front-end API prerequisite failed. These backend versions must be pinned and reverified when that prerequisite is resolved.
- Archive, extracted application, and `release-bridge-probe.json` reside under ignored `build/face-multiview-toolchain/`; total measured 216,453,660 bytes. E: had 283,414,237,184 bytes free after extraction. No resources were uploaded and no background process was started.
- Cleanup, if desired: remove only `build/face-multiview-toolchain/` after verifying its resolved path lies inside this repository. The committed documentation can be reverted independently; no game state needs restoration for this probe.
