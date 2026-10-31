# Environment lighting acceptance, 2026-10-29

Implemented local HDR presets, asynchronous CPU preparation with transactional frame-boundary
publication, shared IBL rotation/relative strength, independent background controls, and selectable
Khronos PBR Neutral tone mapping. Procedural sky and ACES remain the defaults. No performance
optimization, material rewrite, atmospheric scattering or scene-reflection feature is included.

## Verification

- Release build and shader compilation passed on Windows/Clang/Vulkan.
- Full CTest: 75/75 passed (`tmp/environment-full-final.log`, 24.34 seconds).
- Targeted controls/configuration, toon source contract, documentation and shader-source tests:
  5/5 passed (`tmp/environment-targeted-final.log`).
- Independent source review completed. Review fixes reject nonrepresentable half-float radiance
  before conversion and apply exposure before the first rendered frame.
- `git diff --check` passed. Capture executable SHA256 matches the final build executable:
  `4564bcbd62f8ca30861e5cb4e87d2943363003a74d3a5fe24fd0cdeaac36f96a`.

Twelve actual-render captures exited successfully, each producing one PNG. New-path captures used
Vulkan validation and reported no validation errors; the pre-change baseline disabled validation.
This is functional/visual acceptance, not a performance comparison or an interactive GUI exercise.

## Image checks

| Check | Result |
| --- | --- |
| Before/after default settings | Exact RGB match, zero changed pixels |
| Exposure 1.5 on first frame vs frame 100 | Exact RGB match, zero changed pixels |
| Studio Neutral rotation 0 vs 90 degrees | 194137 changed pixels; mean absolute difference 9.202/255 |
| PBR Neutral vs ACES with identical Studio Neutral input | Mean absolute difference 34.123/255, including background |
| Environment/blurred background vs solid background | Object interior differs by at most one RGB code value |

The background comparison uses the Papermill solid-background silhouette eroded by three pixels
(165616 interior pixels). Environment background changes 144 of those pixels by at most one code;
blurred background changes one pixel by one code. Silhouette/compositing pixels are excluded, so
this does not claim full-object pixel identity. Rotation changes reflections as expected. Tone-map
differences are observations, not an accuracy ranking.

Standard captures are 900x1000, auto-fit camera (yaw 0, pitch 0, distance 4.5227, target
0/-1.6446/0), fixed exposure 1, and no SSAO, shadows, TAA, bloom or FXAA. HDR comparisons use no
sun, input gain 1, relative gain 1, environment 256 with nine mips, irradiance 16 and BRDF LUT 128.
Default comparisons retain the legacy sun. Exact configuration/commands are retained per run.

The additional `studio-closeup` image uses camera 0/12/3.2, the same target, Studio Neutral,
relative gain 0.45, solid linear grey 0.26, exposure 1 and PBR Neutral. It demonstrates the new
workflow with closer framing; it is not a controlled match to the supplied Viewer screenshot.

Capture directory: `tmp/environment-captures/` (before/after default, studio-neutral, studio-aces,
studio-rotated, papermill, environment-background, blurred-background, live-switches,
first-frame-exposure, steady-exposure, studio-closeup). Preview:

![Studio Neutral closeup](../../tmp/environment-captures/studio-closeup/screenshot_20261029_000716.png)

## Switching and failure handling

Capture-only cycling exercised successful publication of Neutral HDR and procedural sky. An invalid
HDR and a finite but out-of-range RGBE fixture were rejected with clear errors while retaining the
previous environment. This verifies the controller/publication path; it does not establish that
every preset was published during the cycle. CPU preparation overlaps rendering, but publication
still uses the existing wait-idle transaction and may briefly stall. GPU upload failure recovery
remains the existing transactional code; no device-failure injection was performed.

## Reproduction and provenance

[Usage/configuration](../environment_lighting.md) includes the pinned, hash-checked HDR fetch script.
The compressed [evidence](2026-10-29-environment-lighting-evidence.json.gz) retains run summaries,
configuration, commands, executable/DLL/shader manifests where available, screenshot hashes,
source hashes, capture/analysis scripts, reference asset provenance and test logs. Raw PNGs and
logs remain in the local capture directory. The closeup manifest records only executable hash
and command; it uses the same frozen bundle as the standard after captures. Baseline is b7138fc8.

Studio Neutral is Amazon/Khronos CC-BY-4.0. Papermill is HDRLabs/UX3D CC-BY-NC-SA-3.0, used only
as an optional local noncommercial reference; its HDR is not bundled in the repository. Keep the
downloaded license files. DamagedHelmet retains its supplied `asset/LICENSE.md`. PBR Neutral
source/license/revision are retained under `source/third_party/khronos_tonemapping` (Apache-2.0).

The supplied Viewer screenshot does not identify HDR bytes, orientation, camera or exposure.
These results establish the new environment workflow and default compatibility, not equivalence
to Viewer BRDFs, colour management, antialiasing or material-extension support. Linux/macOS and
manual GUI interaction were not tested in this acceptance run.
