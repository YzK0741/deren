# PBR shadow shader audit

This is the pre-fix audit. The three findings were subsequently addressed; see
[the same-day fix record](2026-10-12-pbr-shadow-fix.md).

Scope: inspect the current raster shadow caster and forward/deferred PBR shader paths, with particular
attention to the reported flicker when orbiting a model. No production shader or synchronization code
was changed. This audit does not claim reproduction of the user's historical visual symptom.

## Confirmed findings

### P1: Forward PBR leaves shared shading control fields uninitialized

`source/shaders/scene/pbr.slang:60-73` constructs `shade_input si` and fills the material/surface
fields, but never writes `shadow_override` or `punctual_replaced`. Neither field has a default
initializer in `lighting/shading.slang`. `shade_surface()` reads the first at line 870 to choose
whether to bypass shadow-map sampling, and the second at line 887 to disable punctual lights.
The deferred entry explicitly initializes both; the forward entry does not.

This is verified in the current compiled `pbr.frag.spv`, not inferred from struct syntax: function
float locals `%4238` and `%4237` are loaded without any store, copied through `%4259` and `%4258`,
then consumed in the shadow-override and punctual-light branches. Fresh compilation with the build's
Slang flags gives an identical binary. A compiler warning about `maybe_unused` is unrelated; compilation
does not diagnose these undefined inputs.

The renderer uses this fragment entry for its transparent forward PBR path. Consequently this defect
is important, but it cannot alone explain flicker restricted to opaque deferred geometry. The normal
forward values should explicitly be `shadow_override = -1.0` and `punctual_replaced = 0.0`; zero-initializing
the whole struct alone would incorrectly select a zero-visibility shadow override.

### P2: Cascade blend sampling has no guaranteed overlap coverage

`lighting/shading.slang:291-295` samples cascade N+1 BEFORE cascade N's far split, over the previous
10% by default. But `shadow_fit::fit()` fits N+1 starting exactly at that split; its inputs do not
include the configured blend width. An orthographic enclosing box can incidentally cover the band,
but that is not guaranteed. `calc_shadow_cascade()` returns fully lit when the next map's projected
coordinates fall outside its box, so a valid shadow can fade towards light during the blend.

A standalone audit executable linked against the actual current `deren.engine.shadow_fit` module
found a concrete counterexample with 45-degree FOV, 16:9 aspect, three cascades, 2048 map size,
scene centre (0,0,-5) and radius 5, and an above-horizon sun direction proportional to
(sin(40 degrees), 0.125, cos(40 degrees)):

- split = 1.41886997; receiver depth = 1.34792650; shader blend weight = 0.5;
- current cascade NDC = (-0.89169711, 0.56660706, 0.76853120), inside;
- next cascade NDC = (-1.00137711, 0.31947824, 0.45200521), outside.

The receiver is at a camera-frustum corner within the scene sphere. Its normal is aligned with the
sun, so the normal offset changes light-space depth, not the x/y footprint. If the current map reports
an occluder, the next function still returns 1.0, brightening the mixed visibility. Orbiting changes
which points enter this case. This reproduces a coverage-contract defect numerically, not a video
of the old flicker. The same blend structure occurs in the wider-kernel and area-light helpers,
although normal PBR calls the basic `calc_shadow()`.

## Related non-shader finding

`shadow_fit.cpp:188-191` computes `texel_raw = 2 * half_raw / map_size`, then
`half = ceil(half_raw / texel_raw) * texel_raw + texel_raw`. The quantity being rounded is therefore
`map_size / 2`, independent of the fitted size. This does not quantize the size to a stable world-space
increment as its comment claims; the resulting texel size remains proportional to the changing
frustum footprint. Floating-point boundary rounding can introduce additional steps.

The audit called the actual fit at orbit yaw 0 and 0.0001 radians (about 0.00573 degrees) for a static
scene and unchanged sun. Cascade-0 world texel size changed from 0.000518824498 to 0.000518839865.
Grid-centre snapping alone cannot remove the corresponding scale change. This is particularly relevant
to moving-camera edge shimmer, but the numerical result does not quantify visible severity for the
user's model. A stable size policy needs separate design and image validation.

## Checks that did not expose a shader defect

- Raster PCF uses a fixed 3x3 tap grid and explicit comparison LOD zero, without per-frame random rotation.
- Cascade selection uses camera view-space depth; it does not use the TAA-jittered projection.
- Host fit input and cache use the unjittered projection/view-projection, so the previously documented
  TAA-induced refit problem has a corresponding current guard.
- Shadow and PBR vertex deformation both read the current frame-slot skin/morph data and agree on
  world/light matrix conventions. Cascade push offset is 108 after the three frame lanes.
- Existing shadow-fit regression suite passes 58 checks, but does not test blend-band coverage or
  stable projection size under small orbit rotations.

## Interpretation and follow-up

Synchronization fixes may have removed an independent historical cause. They cannot initialize missing
shader fields or make the cascade blend and fit agree. The orbit symptom points more directly to
cascade coverage and projection stability than to an intrinsic time-varying PCF kernel. Confirmed shader
and interface defects should receive failing regressions before fixes, followed by moving-camera captures
of a representative opaque model with fixed sun, TAA and optional effects controlled independently.
The forward initialization fix also needs a transparent scene regression. No synchronization diagnosis
was attempted in this audit.

Evidence lives in `docs/reviews/2026-10-12-pbr-shadow-shader-audit/` (SPIR-V data-flow excerpt, fingerprints,
native fit witness source/output); full disassembly and executable remain in
`build-release-dyn-clang64/pbr-shadow-audit/`.
