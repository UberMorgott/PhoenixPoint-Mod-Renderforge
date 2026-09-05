# Editable head mesh: bounded live gates stopped safely

> Archived experiment: NR and face enhancement were retired on2026-09-05. Generated screenshots, obsolete docs probes and abandoned D: workspaces were removed at the user's request. Commands below record historical verification and must not be run against the current mod; its diagnostic APIs were removed. Compact JSON/results remain. See the [retirement dossier](2026-09-05-dlss5-retirement-dossier.md).

- Scope: reconstruct an unchanged readable head mesh using measured native skinning operators. No anatomical enhancement, generated face assets, model downloads or production face hook. Human-quality faces were **not achieved**. Further face experiments are paused following the user's direction.
- Runtime: existing PID44280, user's manually loaded `22222.zsav`, geoscape `Playing`, Sophia's original `Tutorial_Head_Female` renderer ID-207306 / mesh ID1562772. No restart, deploy, save load/write or actor selection change occurred during this probe.

## Actual results

- Prepared isolated DLL SHA256 `359E23F8516AFE94E31B6912E539BF93D2CD0E94B48728E9F5C9490C85C8A6DA` refused the first head before calibration: `Unsupported attribute storage: TexCoord0/Float16`.
- Read-only live descriptor confirmed UV0 is `Float16`, dimension2, stream1. Source and original native BakeMesh passed descriptor matching. This was an unsupported input in the probe, not evidence that the game's mesh is inaccessible.
- One authorized corrective extension admits only that observed half UV layout. Installed `Object.Instantiate<T>` calls native `Internal_CloneSingle`; the corrected reconstruction clones the readable original bake and writes only Position/Normal/Tangent plus original weights/bindposes. It does not convert or rewrite UVs. Post-reconstruction descriptor and bit-exact decoded-UV comparisons remain mandatory, including signed zero. Actual clone storage preservation was **not reached live**.
- Corrected DLL `RenderforgeRestMeshProbeHalfUv.dll`, SHA256 `2DA82E83115BF50502B69B1035DA901F32E382F8FBD8D86B625789F0FCF60731`, built with0warnings/0errors. Offline numerical checks passed inverse/nonunit/singular/conditioning/nonfinite gates, frozen-pose predicates, output containment, narrow half-UV admission and signed-zero/exact-UV comparisons.
- The single retry passed the descriptor gate, then refused `Legacy/full weight values disagree.` The gate compares per-bone sums from readable `Mesh.boneWeights` against `GetAllBoneWeights()`/`GetBonesPerVertex()` at tolerance1e-7. Unity exposes separate native getter paths; equivalence cannot be assumed.
- **Missing measurement:** the first offending vertex and actual weight pair were not retained because the probe exports full influences only after successful validation. The bounded read-only sample proved2960 legacy vertices and first four influence counts `[1,1,1,1]`; it did not capture offending weights. Quantization/renormalization is an unverified possibility, not the diagnosed cause. No tolerance was relaxed and no weight array was substituted.
- Neither attempt created the hidden calibration renderer, reconstructed clone or visible presentation. Independent Unity pose A/B, frozen NR-Off image comparisons and anatomy quality were therefore **not tested**. Yael was not visited after the generic gate failed.

## Restoration and handoff

- Both abort reports record `originalSharedMeshRestored=true`, `originalBoneIdentityPreserved=true`, `originalMaterialsPreserved=true`. Both static probe owners returned null after cleanup. This is identity/cleanup proof; native restored-geometry delta is explicitly unmeasured because no presentation occurred.
- Final live mesh ID remained1562772; `System.Object.ReferenceEquals` on initial/final selected actor returnedtrue. Baseline/final Renderforge config JSON matched exactly; timeScale remained1. No bone/material/camera/settings mutations were made.
- Final handoff: PID44280, geoscape `Playing`,2560x1440, DLAA, NR init/create/eval0x1 and alive1, FG X2 live, renderer resets5 unchanged. This describes the handoff instant; the lead subsequently plans to disable experimental NR according to the user's new direction.
- Exclusive live ownership was released before this report. No further face research or live calls followed the handoff.

## Local evidence (D:, not packaged in the mod)

- Root: `D:\RenderforgeWork\head-rest-reconstruction\`.
- Exact restoration artifacts: `sophia-live\report.json`, `sophia-half-uv-live\report.json`.
- Native refusal envelopes: `sophia-begin-result.json`, `sophia-half-uv-begin-result.json`.
- Baseline/final runtime proof: `live-baseline.json`, `live-final.json`; actor reference equality was PPCLI jobj153 with `result.ok=true,value=true`.
- Prepared source and offline proof: `RoundtripProbe.cs`, `NumericChecks.cs`, `README-probe.md`, `half-uv-build-result.txt`. No raw game mesh export or generated per-head asset is committed. Production package increment: zero.
