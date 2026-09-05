# Local multiview texture toolchain probe

> Archived experiment: NR and face enhancement were retired on2026-09-05. Generated screenshots, obsolete docs probes and abandoned D: workspaces were removed at the user's request. Commands below record historical verification and must not be run against the current mod; its diagnostic APIs were removed. Compact JSON/results remain. See the [retirement dossier](research/2026-09-05-dlss5-retirement-dossier.md).

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

## D: storage constraint (2026-09-05)

- All future toolchain downloads, weights, environments, package caches, and heavy temporary files must use `D:\RenderforgeTools\face-multiview`. The earlier estimate of 25 GiB is optional development-tool working space, not a mod download size. This offline route would distribute accepted game assets and integration code; Python, diffusion checkpoints, and the authoring application are not intended runtime dependencies. No final asset size is established.
- The verified portable distribution was copied without deleting or modifying the E: source: **31 files, 156,582,349 bytes, zero SHA256 mismatches**. The archive remains on E:; there was no new download. EXE signature remains `Valid`, and its SHA256 is the executable hash above.
- Current executable: `D:\RenderforgeTools\face-multiview\StableProjectorz-2.4.5\Stable Projectorz 2.4.5.exe`; working directory: `D:\RenderforgeTools\face-multiview\StableProjectorz-2.4.5`.
- After copying, D: had **2,658,471,120,896 bytes (2475.89 GiB)** free. A read-only C: volume check reported **20,285,964,288 bytes (18.89 GiB)** free. These are time-specific measurements, not permission to use C: for this toolchain.
- Historical storage setup (initializer now removed): `& .\docs\Initialize-FaceMultiviewStorage.ps1` was run in each dedicated PowerShell process **before** subsequent toolchain commands. It only creates D: directories and configures that process and its future children. It does not affect already-running applications or independently launched UI processes. Closing that shell discards its environment changes; no user or machine variables are changed.
- Downloads, model files, saved projects, and logs have explicit `downloads`, `models`, `projects`, and `logs` directories under the D: root. A future backend checkout must also reside under that root. The initializer prepares `environments\forge-neo`; it does not create a Python virtual environment. Any future `uv venv` or backend-specific environment setting must explicitly use that D: path rather than relying on a working-directory default.
- Cache variables are grounded in the official [uv environment reference](https://docs.astral.sh/uv/reference/environment/), [pip cache option](https://pip.pypa.io/en/stable/cli/pip/#cmdoption-cache-dir), [Hugging Face environment reference](https://github.com/huggingface/huggingface_hub/blob/main/docs/source/en/package_reference/environment_variables.md), [PyTorch Hub storage rules](https://github.com/pytorch/pytorch/blob/main/docs/source/hub.md), [extension cache implementation](https://github.com/pytorch/pytorch/blob/main/torch/utils/cpp_extension.py), [Inductor worker cache settings](https://github.com/pytorch/pytorch/blob/main/torch/_inductor/runtime/compile_tasks.py), [CUDA JIT cache reference](https://docs.nvidia.com/cuda/cuda-programming-guide/05-appendices/environment-variables.html), and [Gradio cache helpers](https://github.com/gradio-app/gradio/blob/main/gradio/utils.py). Backend versions and their effective paths still need verification before installation or generation.
- **This does not prove zero C: writes.** The distribution's `Stable Projectorz 2.4.5_Data/app.info` names company `Igor Aherne` and product `Stable Projectorz 2.4.5`. The corresponding `C:\Users\Morgott\AppData\LocalLow\Igor Aherne\Stable Projectorz 2.4.5` directory did not exist at the check; there is no existing profile size to report. [Unity persistentDataPath](https://docs.unity3d.com/ScriptReference/Application-persistentDataPath.html) resolves LocalLow through Windows Known Folder APIs, so changing TEMP/TMP does not redirect it. A complete profile redirect for this release is unverified. Logs/settings or other application-managed files may still appear there, and their size is unknown until runtime observation.
- Unity supports a separate [`-logFile` argument](https://docs.unity3d.com/Manual/PlayerCommandLineArguments.html) for routing Player logs to `D:\RenderforgeTools\face-multiview\logs\StableProjectorz.log`; it is not a complete profile redirect. No registry, Windows folder setting, junction, or global AppData override was changed. No application was launched, and no backend or weights were installed during storage preparation.
- Verification: PowerShell `Parser.ParseFile` returned **0 errors**; all **20 process paths** were under the D: root and the corresponding user/machine environment values stayed unchanged. With the initializer loaded, `uv cache dir`, `uv python dir`, `uv tool dir`, `python -B -m pip cache dir`, and Python `tempfile.gettempdir()` all returned the intended D: locations. Installed `uv 0.11.7` was used read-only; no Python/model package was downloaded. `git diff --check` passed.
