# Renderforge 1.3.0

## Highlights

- **D3D12 auto-exposure restored** -- tactical missions are no longer dark under DirectX 12. The shipped PPv2 exposure compute shaders only target D3D11; a new asset bundle (`rf-exposure-d3d12.bundle`) provides D3D12-compatible copies with identical bytecode. Paired fixture proof and live verification confirm numerical parity (exposure 22.627 on both renderers, tolerance 1e-4). The loader falls back safely if the bundle is missing or kernels fail.
- **Crisp interface fonts** -- supported dynamic UI text renders sharper while preserving the original letter positions and layout. Unsupported or mismatched text uses the original rendering. Toggle under Options > Screen or Mods > Renderforge.
- **Live colour grading and scene styles** -- tactical LUT presets plus five analytic cinema presets (Neutral, CinematicBleach, Vivid, BlackAndWhiteCinema, Noir, AmberFilm, Arctic, VintageSepia, RealisticDesaturated) with a strength slider, and code-only scene stylization (Cartoon, PixelArt). All switchable live from Mods > Renderforge.
- **PixelArt default block size 4 px** (was 2). Range remains 2--16 actual output pixels.
- **D3D12 histogram guard** (`6a8b37c`) -- unsupported exposure kernel consumers no longer throw 4137+ exceptions per session; the guard silently uses the safe (dark, exception-free) path when the bundle is absent.
- **FG / upscaler resource-lifetime fix** (`ca89121`) -- managed and native graphics resources are retained until retirement acknowledgments complete, closing a resource race on FG teardown and re-enable sequences.

## Install

Download **`Renderforge-Full-1.3.0.zip`** from the GitHub release (single archive, all vendor runtimes included). Extract into `<Phoenix Point>\Mods\` so the `Renderforge` folder is at `Mods\Renderforge\`. SHA256 checksum in `SHA256SUMS.txt`.

## Known limits

- The earlier UnityPlayer access-violation crash on D3D12 remains unattributed; exception onset and crash are distinct findings.
- The exposure bundle is game-version-locked; a PP update that ships new exposure shaders may require a bundle rebuild.
