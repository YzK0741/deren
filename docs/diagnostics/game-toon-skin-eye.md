# Skin and eye material alignment

Evidence: `laevatain1.rdc`, frame 1706. Skin is drawn at EIDs 806 and 892
(PS 37820/37858); eyes at EIDs 827 and 871 (PS 37822/37856).

## Skin

Captured material cb6[0].x contains smoothness: body 0.258, face 0.0. The
preview GLB carries roughnessFactor 0.8 for both. Skin profile lane 15 now
carries `(authored smoothness, 0, 0, 1)` from the material sidecar. Negative
smoothness means absent and preserves the glTF fallback; zero is valid.
This family-specific payload reuses the existing lane without changing its
eye MatCap or cloth meanings, or the material buffer layout.

Both main and punctual specular lighting use `1 - smoothness`. Punctual rim
luma uses `base * 0.96 * (1 - metallic)`, independently of the diffuse ramp/LUT
composition. The existing face diffuse tint and highlight map remain separate.

## Eyes

PS 37822 cb6[18] is approximately `(1.27547, 0.23825, 0.73298, 1)`.
The DXBC uses this linear HDR colour directly in the MatCap RGB/alpha
composition. Applying another sRGB decode to the corresponding raw profile
colour changes its hue and intensity, so that conversion is removed.

Local diffuse lighting is accumulated after the eye contrast lift, using the
pre-light composition. Eyes have no generic GGX specular term. In both game
eye variants, the mode-3 rim branch computes a candidate but then clears its
attenuation (`movc r8.x, r15.x, 0, r8.x` in PS 37822); it must not illuminate
the iris. The shared loop's eye adapter suppresses specular and rim modes.
Other families retain their existing lighting policy.

## Verification

`python scripts/gates/game_toon_skin_eye_response.py --build-dir build`
checks authored roughness, zero smoothness, the absent-payload fallback, rim
input, MatCap colour, local diffuse routing and suppression of eye specular/rim
lights on the GPU. The preview application must be rebuilt for the host-side
smoothness staging. Private shaders/configs are written under ignored `tmp/`.

These changes do not reproduce the complete game's clustered light selection,
cookies, screen-space shadows, or post-processing pipeline.
