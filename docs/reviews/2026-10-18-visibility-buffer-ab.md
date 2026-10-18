# Visibility buffer v0.6: the ON/OFF A/B

**Date:** 2026-10-18. **Build:** Release, clang64 (`build-release-dyn-clang64`), Vulkan 1.4.357 validation
layers on. **Switch:** `[render] visibility_buffer` (`false` = the geometry lane, `true` = the visibility
raster + resolve lanes; see the plan's v0.5). **Status:** v0.5 is committed and the default-OFF frame is
byte-identical to the frozen gate; this record is v0.6's measurement of the ON arm, plus the two defects
that measuring it found.

## What was measured, and how to re-derive it

Two full capture-gate rounds with the SAME scenarios, cameras, models and frame counts:

```
pwsh -File scripts/windows/check_render.ps1 -Full -BuildDir build-release-dyn-clang64
pwsh -File scripts/windows/check_render.ps1 -Full -ExtraConfig 'visibility_buffer = true' -BuildDir build-release-dyn-clang64
```

Each round renders every scenario twice (the gate's determinism check; `flaky: 0` in both rounds) and keeps
its frame and log as `<scenario>.last.png` / `<scenario>.debug.log` under `<BuildDir>/render-check`. The
rounds' artefacts are collected into two directories (plain copy of those two file families), and:

```
python scripts/measure/visibility_ab.py <off_dir> <on_dir>
```

reads, per scenario: which lane the captured frame actually recorded (the raster's item count, or the
runtime's named fallback reason), the ON log's validation findings, and the frame difference. That
instrument is the check this section rests on: **a fallback scenario MUST read 0 changed pixels**, because
the whole frame records the old lane - a non-zero number there would mean the arms took different paths by
accident, and the rest of the table would be measuring the wrong thing.

## The result

Admitted = the captured frame was produced by the RESTER + RESOLVE lanes; fallback = the whole frame
recorded the geometry lane. "changed" and the means are ON vs OFF on the final frame (per-pixel maximum
channel difference; 1080x960 frames).

| scenario | lane | validation findings | pixels changed | frame mean | mean over changed | max |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| deferred | visibility, 1 item | 0 | 6.88% | 0.554 | 8.048 | 165 |
| deferred_ssao_off | visibility, 1 item | 0 | 6.91% | 0.559 | 8.082 | 162 |
| deferred_taa_fxaa | visibility, 1 item | 0 | 6.88% | 0.360 | 5.231 | 128 |
| unlit | visibility, 1 item | 0 | 6.28% | 0.434 | 6.911 | 191 |
| shadow_single | visibility, 1 item | 0 | 6.89% | 0.555 | 8.060 | 165 |
| metal_rough_glossy | visibility, 5 items | 0 | 0.51% | 0.006 | 1.124 | 117 |
| glossy_motion | visibility, 5 items | 0 | 0.59% | 0.006 | 1.107 | 78 |
| sponza | fallback: MASK material | 0 | 0.00% | 0.000 | 0.000 | 0 |
| transparent_blend | fallback: MASK material | 0 | 0.00% | 0.000 | 0.000 | 0 |
| deformation | fallback: skinned/morphed leaf | 0 | 0.00% | 0.000 | 0.000 | 0 |
| gaussian_splat | fallback: no visible opaque leaves | 0 | 0.00% | 0.000 | 0.000 | 0 |
| laevatain_goo_toon | fallback: depth-EQUAL re-shading stage | 0 | 0.00% | 0.000 | 0.000 | 0 |
| laevatain_goo_toon_body | fallback: depth-EQUAL re-shading stage | 0 | 0.00% | 0.000 | 0.000 | 0 |
| laevatain_no_sidecar | fallback: depth-EQUAL re-shading stage | 0 | 0.00% | 0.000 | 0.000 | 0 |
| laevatain_old_chain | fallback: depth-EQUAL re-shading stage | 0 | 0.00% | 0.000 | 0.000 | 0 |

Reading it:

- **Seven scenarios took the new path, eight fell back whole, and every fallback is 0.00% changed** - the
  whole-frame rule works: the frames whose lane is the old one are exactly the frames the old path
  produces. The reasons are the runtime's own vocabulary (`deren::engine::visibility::admission`), printed
  once per session and unit-tested clause by clause.
- **The admitted frames differ by 1.1-8.1 of 255 on the pixels that changed, and by 0.006-0.56 over the
  whole frame.** The differences are the v0 path's known, expected ones, not structural: the compute stage
  samples material textures at explicit LOD 0 while the fragment path's sampler picks a derivative mip, and
  the resolve's tangent frame comes from the triangle's edges while the fragment's comes from screen-space
  derivatives. Both are documented in `scene/visibility_resolve.slang`; a structural error (a hole, a
  shifted surface) would show as a large REGION difference, which the tile-level check in
  `scripts/measure/tiletab.py` would attribute to scene positions instead of detail.
- **The two sphere-sweep scenarios are the near-identity pair (0.5-0.6 changed, mean 0.006)**: that scene's
  materials are smooth/without normal-map detail, so neither of the two v0 difference sources has anything
  to act on - and they are the control that says the ~7/255 of the deferred scenes is the DETAIL being
  resolved, not the shading being wrong.
- **Zero validation findings across all fifteen ON runs**, and both rounds reported `flaky: 0` (each
  scenario rendered twice, same hash).

## What it took to get here: the two measured defects

1. **v0.5's object-space normal (fixed before this round).** The first admitted capture came out darker on
   the whole object: the resolve interpolated the vertex normals WITHOUT the item's model matrix and passed
   them to the shared decode as geometric normals. The G-buffer debug view named it exactly - the normal
   channel's x/y matched the old path and z did not, which is what a glTF Y-up -> Z-up model rotation looks
   like when one producer forgets it. With the model transform the deferred scene's mean over changed
   pixels dropped from 63/255 to 7/255.

2. **The depth-EQUAL re-shading stage (fixed in this round).** The four `laevatain_*` scenarios are the toon
   character chain: its stage draws the opaque leaves a SECOND time with `depth_compare::equal` over the
   frame. First A/B: those frames were admitted, and the character stage's coverage shrank by ~106k pixels
   (the ON/OFF mean over changed pixels was 67/255, against 8/255 for the plain deferred scene). Three
   views of the same fact - the character's pixels fell back to the deferred shading the stage was meant to
   replace - and the mechanism is exact equality: a depth value produced by a DIFFERENT pipeline module can
   be one ULP away, and the fragment is then rejected. The frame-level admission now refuses a frame that
   carries that stage (`depth_equal_stage`), with the depth producer's unification as the follow-up.
   After the clause: 0.00% changed, as the whole-frame contract says.

   A session can print BOTH lane lines once: the app mirrors the render switches into the runtime every
   frame, so a frame before the mirror lands can record one lane and the rest the other. The captured
   frame (frame 40) is what the table measures, so `visibility_ab.py` reads the LAST lane line.

## Known v0 limits this A/B quantifies rather than hides

- alphaMode MASK materials are not resolved (a compute pass cannot discard a pixel whose depth is already
  written): the frame falls back WHOLE, which is why sponza and transparent_blend are in the fallback half.
  The shader's `mesh_stat_visibility_unresolved` counter measures what a partial attempt would leave.
- near-plane-clipped triangles cannot be reconstructed from the id (counted, left empty).
- material fetches are LOD 0, and the tangent frame is the triangle's (both above).
- skinned/morphed/instanced draws, custom pipelines and item-table overflows fall back whole.

## What this record does NOT claim

- The ON path is not the default and this round does not change any frozen reference: the fifteen legacy
  hashes keep describing the OFF arm, and the full gate on the same build is 15/15 (see the gate log).
- No ON-path hash is frozen yet; that is the v0.7 step (a specified scenario's vis-path reference hash and
  its explanation).
- The frame-level differences are quantified in display units on this machine; the run that produced this
  table is one Release build on one driver, the same rule every other capture number in this repository
  follows.

## The logs and artefacts

The two rounds' per-scenario frames and logs are machine-local scratch under
`<BuildDir>/render-check/ab-off`, `<BuildDir>/render-check/ab-on` (the gate's own directories). The numbers
above are reproducible from them with the command in "What was measured"; the logs are also where the
`visibility raster: recorded N item(s)` (admitted) and `visibility buffer: frame NOT admitted (...)` (fallback)
lines live.

## The ON-path reference gate (v0.7)

The ON arm needs a frozen reference of its own, and it is a SECOND reference set rather than more entries in
the legacy file: the legacy file describes the geometry lane, and a -Full round's verdict is "15 of 15
against it". So the gate grew one `tier = "visibility"` scenario and one selection switch:

- The scenario is `deferred_visibility`: the `deferred` scenario's model, camera, frame count and config plus
  `visibility_buffer = true` - one key different, so the two rounds' hashes on it ARE the A/B on one frame.
- `-Visibility` selects that tier; `-Full` deliberately does NOT (the legacy verdict stays 15/15), and the
  cross-selection note in the script says why.
- Its frozen hash lives in `docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt`
  (`deferred_visibility = 104E00AFDF3630EA` on this build), and the round that judges it is:

```
pwsh -File scripts/windows/check_render.ps1 -Visibility -Compare docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt -BuildDir build-release-dyn-clang64
```

which this build passes: `deferred_visibility 104E00AFDF3630EA 104E00AFDF3630EA matched`, deterministic across
the round's two runs (`flaky: 0`), zero validation findings in its log. A `-Compare` round does not need a
machine-local baseline: the frozen file IS the reference, so the determinism check and the verdict run even on
a machine that never seeded one (the script's local-reference branch says so).

The hash is a NEW reference, not a regression of the old one: when the visibility path's resolve changes
(a mip rule, the near-plane clip, a material family), this scenario moves and the legacy file must not - the
two files together are what keeps "the old path is unchanged" and "the new path changed on purpose"
separable. Re-baselining it is `-Visibility -Update`-free by design: the committed file is updated by hand
from the round's actual hashes, in the same commit as the change that moved them, with the reason.

## Follow-ups (v0.8 record)

The plan's road after v0 is software raster writing the SAME visibility buffer (hardware and software
converge on the id), then material binning (the resolve stops branching per pixel and bins by material tile),
then a cluster hierarchy with screen-error LOD writing the ids (the id layout changes there: item/meshlet
fields narrow), then paging/streaming. This v0 build keeps the id small for exactly that reason and does not
reserve complexity for it.

The three concrete debts this record leaves:

1. **The depth producer is not unified with the depth-EQUAL re-shading stage** (the v0.6 defect): the
   character pass needs its own pipeline's depth bit-exactly, so those frames fall back. Options are to have
   the character stage write its surface through the resolve/compute path as well, or to give it a
   reconstructable depth source instead of the attachment.
2. **alphaMode MASK has no visibility-path answer yet**: a compute resolve cannot discard a pixel whose depth
   is already written, so MASK materials (and every frame containing one) record the old lane. A later step
   can either bin masked materials out of the raster lane or give the resolve an alpha test against the
   stored coverage.
3. **The retired `visibility_depth` family** stays declared/created/published (schema ids renumber on removal)
   until the v1 software raster's resource pass, which is also where the five storage twins' layout should be
   revisited.
