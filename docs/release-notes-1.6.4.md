# Renderforge 1.6.4

- **Updated runtimes: NVIDIA DLSS 310.9.1 and Streamline 2.14.1.** FSR 2.3.0, XeSS 3.0.2 and NIS 1.0.3 were already the latest releases.
- **New Options -> Graphics "DLSS MODEL" row.** Choose Auto (default, best quality: transformer K/M/L), K, J, M, L, or the CNN models E and F. E is much faster at a softer image -- measured at 1440p on an RTX 5070 Ti: DLAA 65 -> 94 fps, Quality 81 -> 93 fps. Auto stays the default because image quality comes first.
- **Fewer hitches.** Sharpen / LUT / style / grade shaders are precompiled, so toggling them no longer stalls; the mip-bias sweep is cached (no 10-15 ms hitch right after a level starts); geoscape markers update on game events instead of periodic full rescans; settings are saved once a slider stops moving instead of every frame.
- **GPU saving: with the upscaler Off and only post effects on, motion vectors and depth are no longer rendered.**
- **Fixes:**
  - Frame generation could present a stale prep frame or shifted DLSS-G frame tokens.
  - Post effects were not released on level exit.
  - DLSS history was reset during smooth camera FOV blends, causing shimmer.
  - The DLSS MODEL row showed the wrong state after a provider switch.
  - UI sprites that loaded late missed the pixel-perfect pin.
