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

All presets blend against the ungraded reconstructed pixel by the user-selected strength. `Off` bypasses the shader path; shader/resource failures also fall back to the reconstructed frame.
