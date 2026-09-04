# LUT design and licensing

Renderforge ships no LUT, screenshot, numeric curve, or other asset from a third-party mod. Its presets are original analytic colour transforms compiled into the native post-reconstruction shader. The managed frame submission gates them on a playing `Base.Levels.Level` that owns `PhoenixPoint.Tactical.Levels.TacticalLevelController`; Geoscape and every non-tactical level submit preset `Off` and remain ungraded.

## Reference targets

- [Aerix LUT - Photorealism](https://www.nexusmods.com/cyberpunk2077/mods/25836): true-to-life colour, clean contrast, natural highlights. Its page forbids modification and requires permission for asset use.
- [Nova LUT 4.0](https://www.nexusmods.com/cyberpunk2077/mods/11622): natural palette, lifelike luminance, modest contrast, reduced highlights. Its page forbids asset use.
- [Neutral LUT](https://www.nexusmods.com/cyberpunk2077/mods/13757): a vanilla-like neutral balance with little green tint and preserved vibrant lights. Its page allows asset use with credit, but the uploaded LUT derives from Cyberpunk game data and is not used here.
- [NEO / BleachBypass / NOIR](https://www.nexusmods.com/cyberpunk2077/mods/11083): desaturated high contrast while retaining vivid practical lights. Its page allows reuse with credit, but no asset or curve is used here.

Nexus does not grant a blanket default reuse licence. Its [File Submission Guidelines](https://help.nexusmods.com/article/28-file-submission-guidelines) say permissions are file-specific, credit is not a substitute for explicit permission, and absent permissions must be treated as requiring author approval. This implementation therefore uses only generic visual goals and independently authored math.

## Original presets

- **Realistic Desaturated:** neutral grey balance, clearly reduced saturation, slightly restrained contrast, and a subtle cool-neutral channel balance. It is deliberately not monochrome.
- **Neutral:** nearly source-level saturation with a small contrast lift and subtle warm-neutral channel balance.
- **Cinematic Bleach:** stronger luma contrast and substantially lower general saturation with a slight cool bias.
- **Vivid:** controlled chroma expansion, a modest contrast lift, and a slight warm bias.
- **B&W Cinema:** true monochrome at full strength, with a gentle S-curve and preserved black/white endpoints.
- **Noir:** stronger monochrome contrast and a red-filter channel balance that darkens blue/cyan relative to warm colours.
- **Amber Film:** restrained saturation and warm amber midtones, leaving black and neutral white untinted.
- **Arctic:** muted colour and cyan-blue midtones, with neutral endpoints.
- **Vintage Sepia:** warm monochrome midtones with raised blacks and soft whites, like faded photographic paper.

All presets blend against the ungraded reconstructed pixel by the user-selected strength. `Off` bypasses the shader path; shader/resource failures also fall back to the reconstructed frame.

The five cinema presets append ABI values 5–9; existing saved values are unchanged. They add no texture, model, or downloaded asset. The existing Cinematic Bleach already supplies the bleach-bypass look.

## Domain and verification

The shader uses display-referred RGB coefficients directly on the reconstruction output. The normal UNORM path views sRGB resources as raw UNORM bytes. FP16 paths receive their existing linear values without a new transfer-function conversion; overbrights remain finite and are not capped to 1. The same numeric profile can therefore look different on FP16-linear output. These are artistic grades, not calibrated film-stock emulations.

`native/probe/lut_probe.cpp` compiles the production HLSL and executes it on D3D11 WARP. It checks all nine presets over a 4,913-colour cube, alpha preservation, finite nonnegative output, zero/half/full blends, monochrome equality within floating-point tolerance, 1,025-step monotonic neutral ramps, and floating-point overbright retention. There is no LUT texture layout to validate.

See [cinema pack evidence](../2026-09-05-lut-cinema-pack.md) for the shared-source contact sheet, commands and visual-validation limits.
