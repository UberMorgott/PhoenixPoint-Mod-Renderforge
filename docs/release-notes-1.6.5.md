# Renderforge 1.6.5

Image-quality and frame-generation fix release. Vendor runtimes are unchanged (DLSS 310.9.1, Streamline 2.14.1, XeSS SDK 3.0.2, FSR SDK 2.3.0, NIS 1.0.3).

## Changed - read this first

- **Auto quality at 4K is now Quality (was Performance).** The image is sharper, and the frame rate is lower than in 1.6.3 / 1.6.4 for 4K players on Auto. Pick **Performance** manually to get the old frame rate back. Above 4K, Auto is Balanced; up to 1200p it stays DLAA.
- **The frame limit with frame generation now means presented fps** (what you see on screen) for all three providers: DLSS-G, XeSS-FG and FSR-FG.

## Fixed - upscaling

- **Geoscape blur and shimmer under every upscaler.** Rotating props and decals wrote wrong motion vectors.
- **The upscalers now get real scene depth.** Under deferred rendering they were handed colour instead.
- **Soft image on DX12.** Upscaler inputs were not paired with the frame Unity had actually rendered.
- **XeSS and FSR stair-stepping / softness.** The jitter sign was wrong for both.
- **Geoscape markers are sharp on DX12**; site markers and aircraft are drawn after reconstruction instead of going through the upscaler.

## Fixed - frame generation (DLSS-G, XeSS-FG, FSR-FG)

- **Image shake that grew toward the screen edges.** The generators were fed inputs that broke their contract (projection, row orientation, frame pairing).
- **Pacing.** The Reflex / XeLL sleep now runs on the main thread at the start of the frame, and exactly one limiter paces at any time.
- **Geoscape markers stay sharp with frame generation on.**
- **DLSS-G could stay off when VRAM was low.** An out-of-VRAM warning from Streamline was treated as a failure.
- **Robustness of turning frame generation on / off and switching provider**: the provider state follows what the SDK actually answered, and teardown is ordered and serialised.

## Known issues

- DLSS Performance on DX12 shows slightly more sharpening flicker on fine geoscape detail than on DX11. Lower Sharpness or use Quality.
- Rare FSR frame-generation hang when VRAM is heavily overcommitted. Not resolved; diagnostics were added to the mod log.
- With frame generation on, geoscape markers update at the rendered (not the generated) rate while the camera orbits.
