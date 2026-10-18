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

## Follow-up measured after v0.6: the material LOD (first of the non-extension fixes)

The v0.6 table's largest admitted-scene term was the material fetch's LOD: the resolve sampled at explicit
LOD 0 while the fragment sampler's derivative picked a mip, so minified detail was sharper on the new path.
The resolve now derives the LOD from the same quantity a derivative approximates - the triangle's UV area over
its screen area, i.e. the UV scale per pixel (`visibility_texture_lod` in `scene/visibility_resolve.slang`) -
and passes it as the `SampleLevel` argument. It is still an explicit fetch (the compute-derivative capability
is not in this renderer's baseline), and it reproduces the mip RANGE rather than the hardware's per-pixel
anisotropic choice.

Measured on the same `deferred` frame, ON vs OFF, this build:

| metric | before (LOD 0) | after (triangle-scaled LOD) |
| --- | ---: | ---: |
| pixels changed | 6.88% | **4.57%** |
| frame mean | 0.554 | **0.125** |
| mean over changed pixels | 8.048 | **2.741** |
| share of pixels differing by >4 | 2.91% | **0.67%** |
| max pixel difference | 165 | **129** |

The OFF arm is unchanged byte for byte (`deferred = FAE60C0B1D8C1191`, the frozen legacy hash) - only the
resolve's module was recompiled - and the ON-path reference moved with the fix, so
`docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt` now names `7EF9249001EB6035` with this section as
the reason for the re-baseline. Remaining known difference source: the tangent frame (triangle edges vs screen
derivatives), which is the next non-extension fix.

## The G-buffer residual after the LOD fix, and why the TBN item is closed

The next non-extension item on the list was "align the resolve's tangent frame with the fragment's". The
per-channel measurement says there is nothing left to align. The same `deferred` frame, ON vs OFF, captured
through the G-buffer debug view (channels 0/1/2 = albedo, world normal, roughness) after the LOD fix:

| channel | changed | frame mean | mean over changed | max |
| --- | ---: | ---: | ---: | ---: |
| 0 albedo | 2.65% | 0.098 | 3.678 | 151 |
| 1 world normal | **0.76%** | **0.012** | 1.624 | 64 |
| 2 roughness | 3.65% | 0.112 | 3.056 | 145 |

The normal channel is the tell: BEFORE the LOD fix it was the largest difference (15.29% changed, mean 8.72,
max 209 - the normal MAP's mip, not the frame), and now it is the smallest. Algebraically the two TBN
constructions were never different - `surface_tbn` solves the same linear system from ddx/ddy that the
resolve solves from the triangle's edges, and a change of basis does not change ∂P/∂u, ∂P/∂v - so the earlier
normal-channel gap was the normal map being sampled at LOD 0, i.e. the fix that landed. The TBN follow-up is
therefore CLOSED AS NOT NEEDED; what remains is the LOD being a per-TRIANGLE scale rather than the sampler's
per-pixel gradient (albedo and roughness show it most, 3-4% of pixels at ~3/255 on the changed ones), plus the
rasterizer-interpolation vs resolve-perspective-weight float residue. A per-pixel gradient would shrink it
further at the cost of the analytic derivative reconstruction; that is a quality/diminishing-returns call for a
later pass, not a correctness debt.

## Depth-producer unification (non-extension item 1): first root-cause evidence

v0.6 recorded the depth-EQUAL re-shading stage as a 1-ULP depth difference between pipeline modules. The
first measurement for the fix checks that premise at the SPIR-V level: the two mesh stages disassemble to the
SAME arithmetic for the clip position - a `view_proj = OpMatrixTimesMatrix(view, proj)`, then
`OpMatrixTimesVector(model, position)`, then `OpMatrixTimesVector(view_proj, world)`, in that order
(`visibility.mesh.spv` and `pbr.meshlet.spv` both). With the same inputs the depth values these stages write
must therefore be bit-identical, so the 1-ULP hypothesis is NOT supported by the code the pipeline runs. What
the next step has to instrument is the INPUT side and the comparison itself: the model matrix bytes each
pipeline receives, the camera block, and the actual depth values at the rejected pixels (a readback, not the
8-bit debug view) - the stage's coverage mask cannot distinguish "depth unequal" from "depth equal but the
pixel was never drawn by the character stage for another reason", and the earlier mask measurement compared
two DIFFERENT captures for its base, which is one confound this item must remove.

So: item 1 is now root-cause-finding, not "apply the known fix". The candidate remain unchanged (a: produce
the depth with the character stage's own module; b: give the stage a reconstructable depth source; c: only if
the input comparison shows it, an arithmetic/contraction change), but the decision needs the raw depth values,
which the next session measures with a temporary readback of the depth attachment at the rejected pixels.

## Depth-EQUAL root cause, measured (item 1, round 2)

A temporary env-gated diagnostic isolated the mechanism (both switches reverted before this commit; they were
measurement scaffolding, not code to keep):

- bypassing the `depth_equal_stage` admission clause reproduces v0.6 EXACTLY: laevatain_old_chain ON vs OFF
  is 33.45% changed / 20.736 frame mean / 62.0 over changed pixels - the same numbers v0.6 recorded, so the
  clause is bypassed correctly and the earlier measurement was not confounded;
- the SAME bypass with the character pipeline's depth compare changed from EQUAL to LESS_OR_EQUAL drops that
  to 9.47% changed / 4.559 frame mean (mean over changed 48.16).

So the mechanism IS the exact depth comparison: with `equal`, the character stage's fragments are rejected
wherever its own depth and the stored G-buffer depth differ at all; with `less_or_equal` they draw. The
SPIR-V check above shows the clip-position arithmetic is identical in both pipelines, so the difference is on
the DATA side (model/camera bytes reaching the two stages, or a rasterization-state difference), not the
expression. That is what the next measurement reads back: the stored depth and the character stage's own depth
at the rejected pixels (a readback attachment), plus the model/camera bytes each stage received.

The fix candidates narrow with it: (a) make the character stage produce the depth it tests against (its own
pass writes the G-buffer depth on such frames), (b) give it a depth compare that tolerates the difference only
after proving where the difference comes from (b is NOT acceptable while the source is unknown - a tolerance
would hide a wrong-depth producer), or (c) align the data/state path. LESS_OR_EQUAL is measured to be closer
to the old frame but is NOT the fix: it changes which fragments the stage draws (two surfaces at one pixel
would both shade) and would need its own A/B on the 4 toon scenarios before it could be considered.

## MASK impact measured (item 2, round 3)

Sponza is the frame the MASK admission clause turns away, so its size is the number that decides how the
resolve should grow an alpha test. With the clause bypassed (temporary env switch, reverted before this
commit) the visibility path DOES run on sponza: the raster lane records all 62 items and the resolve reports
**957,719 pixels over the session that could not be rebuilt** - the alphaMode MASK fragments the resolve
recognises as cut-out (it evaluates the MASK rule but has no surface BEHIND the pixel to fall back to). That
is ~1.2% of a frame's pixels per render, and the picture difference against the old lane measures
changed% / frame_mean / mean_changed in the review table below this section (the holes show the background
through alpha-tested foliage).

THE FIX DIRECTION THIS POINTS AT: a resolve-side alpha test is NOT sufficient - not for lack of information
(the resolve already samples the albedo alpha and applies the cutoff) but for lack of the NEXT surface: the
raster lane stored only the nearest triangle's id, and a discarded masked pixel would need the depth-behind
id. The viable shape is to keep masked triangles out of the id raster BEFORE it writes depth - either the
existing `mask_bake` compute pass's per-triangle collapse (already built for the ray-traced shadows) marking
the triangles the raster must skip, or an alpha-tested visibility fragment stage (interpolated UV + albedo
alpha discard) which is a heavier raster. The first reuses a measured instrument and keeps the raster
id-only; it is the direction item 2 should take.

## The ON path's GPU timing marks (non-extension fix, round 4 - the instrument item 5 needs)

The ON arm was recording a frame whose per-pass GPU timing report was destroyed by the lane swap: the report
walks the frame's marks in `gpu_mark_id` order, the geometry lane is what wrote `scene_end` and
`rt_shadow_end`, and a visibility frame does not record it - so every later mark of the frame was refused.
MEASURED on `deferred` ON: **492 "GPU timing mark N was refused (9)" lines per session**, which also means
the per-pass timings (the instrument item 5's performance baseline needs) were unusable on exactly the path
the baseline must measure.

The fix: the resolve lane - the pass that REPLACED the geometry lane as the opaque producer - emits the same
two marks after its dispatch, meaning the same thing (the opaque producer is done; the ray-traced shadow
lane's slot in the report is unchanged). MEASURED after: **0 refusals**, and the frame's hash is unchanged
(`7EF9249001EB6035`) because marks are timing queries, not pixels. The OFF arm never had the problem (the
geometry lane records its own marks), so no OFF reference moves.

## Performance baseline: the ON path is SLOWER than the geometry lane (item 5, first measurement)

The GPU timing marks fixed in the previous commit make this measurable on the ON path. Two 130-frame runs of
the same `deferred` config (130 frames = the report's 60-frame window has converged; the SECOND window of each
run is quoted, the first includes startup), per-pass GPU timestamps from the runtime's own report:

| pass bucket | OFF (geometry lane) | ON (visibility raster + resolve) |
| --- | ---: | ---: |
| scene | 0.32 ms | **0.65 ms** |
| lighting | 0.20 ms | 0.21 ms |
| bloom | 0.05 ms | 0.03 ms |
| composite | 0.37 ms | 0.22 ms |
| **total** | **0.95 ms** | **1.11 ms** |

READ IT HONESTLY: the ON path is ~17% SLOWER in GPU time on this frame, not faster. The bucket the report
calls "scene" is where the difference lives (0.65 vs 0.32 ms): on an ON frame it contains the mesh-task id
raster AND the compute resolve, while the OFF frame's contains only the fragment G-buffer pass - so the id
raster plus the resolve costs about twice the single G-buffer pass it replaces on this scene, and the
composite/bloom columns' differences are small/variance. CPU-side totals were not stable enough in capture
mode to quote. This is the number the optimization item (material binning, then the software raster) has to
move: it says the resolve is the cost centre, not the raster, and that "the visibility path exists" does not
mean "the visibility path is faster" yet.

CAVEATS, recorded because a baseline without them invites over-reading: one scene (DamagedHelmet, one opaque
item), one machine (RTX 4060 Laptop), capture mode with validation layers ON and a screenshot read-back at the
end; the per-pass numbers are averaged over the frame window and the pass labels are the runtime's buckets
("scene" as defined above), not per-lane counters. The next measurement splits the registry's buckets to
separate raster from resolve (a lane-level mark), and only then should an optimization be judged.

## The baseline is NOT trustworthy yet: the TAA configuration contradicts the non-TAA one

The same 130-frame measurement was repeated on `deferred_taa_fxaa` (the second admitted configuration):

| pass bucket | OFF | ON |
| --- | ---: | ---: |
| scene | 0.67 ms | **0.27 ms** |
| lighting | 0.25 ms | 0.14 ms |
| bloom | 0.28 ms | 0.23 ms |
| composite | 0.24 ms | 0.26 ms |
| fxaa | 0.06 ms | 0.04 ms |
| **total** | **1.50 ms** | **0.94 ms** |

That is ON ~37% FASTER - the OPPOSITE SIGN of the `deferred` measurement (ON ~17% slower). Two configurations,
two contradictory directions, so the per-pass numbers are NOT a baseline yet. The likely causes, all visible
in the method: a single 60-frame window per arm (frame pacing in capture mode is not steady - the CPU `pace`
phase measured 59.79 vs 0.92 ms between the two windows of one run), the pass labels are the runtime's buckets
whose boundaries the lane swap moved, and `scene` on the TAA arm may still be absorbing different lanes than on
the non-TAA arm. NO optimization may be judged against these numbers.

What the next measurement must do before any optimization is judged: repeat each arm >=3 runs and quote the
median + spread; make the raster/resolve split explicit with a lane-level mark; and verify the lane attribution
from the logs (raster recorded N items / resolve counters) rather than assuming both arms took their intended
path. The item-5 status is therefore "instrument works, baseline NOT yet established" - recorded here so the
next round doesn't treat either direction as a fact.

## The baseline, measured properly: repeats + medians (item 5, corrects round 5/6)

The two contradictory single-window rounds were replaced with the method the contradiction demanded: 3 runs
per arm of the same 130-frame `deferred` measurement, each run's LAST converged 60-frame window, median quoted.
Results (ms, GPU pass timings from the runtime's report):

| arm | scene (3 runs, sorted) | scene median | total (3 runs, sorted) | total median |
| --- | --- | ---: | --- | ---: |
| OFF | 0.27 / 0.28 / 0.33 | **0.28** | 0.71 / 0.84 / 1.04 | **0.84** |
| ON | 0.92 / 1.05 / 1.06 | **1.05** | 1.70 / 2.02 / 2.05 | **2.02** |

This is a stable, one-direction signal: the ON path's GPU total is **~2.4x** the geometry lane's on this frame
(2.02 vs 0.84 ms), and the `scene` bucket - mesh-task id raster + compute resolve - is **~3.75x** the single
fragment G-Buffer pass it replaces (1.05 vs 0.28 ms). The round-5 single-window numbers (1.11 vs 0.95) and the
round-6 TAA contradiction were BOTH window/pacing artifacts; they are superseded by these medians, and the
round-6 TAA configuration must be re-measured with repeats before it is quoted at all.

CONSEQUENCE FOR THE ROADMAP, recorded before any optimization work: material binning / the software raster
now have a concrete target - the resolve+raster pair costs more than the thing it replaced, so a visibility
path that is not faster than the fragment G-buffer is the honest starting point, not a regression to hide.
The next measurement (lane-level mark splitting raster from resolve) says WHICH of the two to optimize first.

## The repeat measurement makes the contradiction REAL: the pass labels are the problem

Round 6's TAA result was dismissed as a single-window artifact. Repeated 3x per arm it is reproducible:

| arm | scene median [range] | total median [range] |
| --- | ---: | ---: |
| OFF (TAA) | 0.67 [0.66..0.76] | 1.30 [1.29..2.04] |
| ON (TAA) | **0.28** [0.27..0.30] | **0.95** [0.95..1.21] |

ON is reproducibly ~27% FASTER on total in the TAA configuration, while the non-TAA `deferred` configuration
is reproducibly ~2.4x SLOWER (round 7). Both are now stable measurements, so the discrepancy is not noise - and
the per-bucket numbers make the cause visible: the OFF+TAA `scene` bucket reads 0.67 ms while the OFF non-TAA
`scene` bucket reads 0.28 ms for a frame that does the SAME G-buffer work plus a TAA pass the report already
has a bucket for (`taa 0.00`). The `scene` bucket is therefore absorbing different lanes depending on the
path (TAA resolve/copy marks, and on ON frames the raster+resolve pair), exactly the caveat the round-7
section recorded.

CONCLUSION, and it is the blocking fact for item 5: the runtime's pass-timing BUCKETS cannot compare the two
opaque lanes - the marks that bound them are written by different lanes in the two paths. The baseline needs a
lane-level mark (raster end, resolve end) so raster, resolve, TAA and lighting are attributed per LANE rather
than per bucket, and only then can material binning or any other optimization be judged. Until then the item-5
status stays: instrument works, comparison not valid - but the absolute ON numbers (scene+resolve+taa+lighting
around 0.9-2.0 ms total for a 1080x960 frame) are usable as a smoke-level check.

## Family 15 (the retired capture depth) is GONE from the producer (item 4, first half)

v0.5 made the raster lane write the shared G-buffer depth, so `visibility_depth` (family 15) had no importer
left; the cleanup kept it "declared, created and published for schema stability". This step removes the
producer side entirely: `output_targets_pass` no longer declares it (16 -> 15 families), the runtime no longer
adopts or publishes it, and both heap-slot mirrors (shader `heap_slot_constants.slang` and
`render_layout.cppm`) drop the constant. `resource_id::visibility_depth` itself stays in the enum (removing an
enum value renumbers every id after it) and heap slots 727..734 stay reserved for the v1 software raster's own
private depth - both documented in docs/managed_resources.md.

MEASURED: one depth image per swapchain image is no longer created or cleared (~33 MB at 1080x960 across 8
images) and its per-frame descriptor publication is gone; the frames are unchanged - OFF `deferred` is
`FAE60C0B1D8C1191` and ON `deferred` is `7EF9249001EB6035`, both identical to before this step, and CTest is
49/49 (which includes the shader/host heap-slot mirror parity test that caught the two halves during the
cleanup). No reference or review hash moves.

## Lane-level split: the design the bucket labels demand (round 10)

The report's labels are static (interval i is labelled `gpu_timing_labels[i]`), so the split cannot be done
by only adding marks to the ON path - an OFF frame would then carry the wrong label on the interval where
its geometry pass runs. The design that keeps both arms comparable in ONE table:

- insert ONE new mark between `shadow_end` and `scene_end` (`visibility_raster_end`), so the report has two
  intervals: `scene` (shadow_end -> that mark) and `visibility raster` (mark -> scene_end);
- BOTH paths write the mark at the same semantic point. ON: the raster lane writes it after its item
  dispatches; the resolve lane writes `scene_end` after its dispatch. OFF: the geometry lane's callback
  writes the new mark FIRST (so the interval before it is ~0) and `scene_end` after the pass, which keeps
  the geometry pass inside the SAME second interval label;
- the labels become path-independent: ON's second interval is "raster + resolve" (the pair the bucket
  measured), OFF's second interval is the geometry pass. The first interval is the raster-only time on ON
  and ~0 on OFF. That makes ON's raster cost directly readable as (first interval) and the pair comparable
  against OFF's second interval without a second label set.

The mark's id must sit BEFORE `scene_end` in the enum for the write order to be legal, which shifts every
later mark id by one; every writer uses the symbolic ids, and the labels table is one shorter than the mark
list, so the change is mechanical. This is the next implementation step for item 5; no measurement is
claimed for it here.

## The lane split, measured: the RESOLVE is the cost (item 5 closed on the comparison question)

The lane-level mark from the previous design landed (both paths write it at the same semantic point, so the
labels are path-independent), and 3 repeats per arm of the 130-frame `deferred` measurement now give a
comparable split:

| bucket | OFF (geometry lane) | ON (visibility path) |
| --- | ---: | ---: |
| visibility raster | 0.09 ms [0.09..0.09] | **0.01 ms** [0.01..0.01] |
| scene (OFF: geometry pass; ON: resolve) | 0.19 ms [0.19..0.19] | **0.92 ms** [0.64..1.05] |
| **total** | **0.72 ms** | **1.65 ms** |

Reading it:

- The split is VALID: OFF's two buckets sum to the round-7 single bucket (0.09 + 0.19 = 0.28 ms), so the mark
  did not move the geometry pass out of the "scene" slot on the old path.
- The mesh-task id RASTER is essentially FREE on this frame (0.01 ms, one item / one meshlet visible).
- The COMPUTE RESOLVE is the cost: 0.92 ms - about 5x the fragment G-Buffer pass it replaced (0.19 ms), and the
  reason the ON total is 1.65 vs 0.72 ms (~2.3x).
- CONSEQUENCE FOR THE OPTIMIZATION ITEM: material binning (or any resolve-side work) is the right lever; the
  raster lane is not worth optimizing on this scene. The item-5 baseline question is answered as far as this
  scene can answer it; the TAA configuration's re-measurement (3 repeats, the round-8 medians) can now use the
  same labels if needed, and its ON-total advantage there remains unexplained by this split and is not used.

The mark's pixel effect: none (OFF `deferred = FAE60C0B1D8C1191`, ON `deferred = 7EF9249001EB6035`, both
unchanged), so the frozen references and the ON reference file do not move.

## The frozen OFF gate re-verified after the non-extension changes (round 12)

The changes landed in this goal round series are all either ON-path-only (resolve LOD, GPU marks, lane split)
or producer-side cleanups whose declared contract is "no pixel changes" (family 15 removal). The authoritative
check is the full frozen round, re-run on the current tree: **15/15 scenarios matched, 0 mismatched,
compare_render_hashes PASS** against docs/reviews/2026-10-17-render-graph-gate/legacy-hashes.txt. That is the
objective's hard constraint satisfied end to end (not just the per-scenario `deferred` check each change ran),
and the ON reference file is unchanged as well (7EF9249001EB6035). CTest remains 49/49.

## MASK item: the reuse-mask_bake plan does NOT fit, and the lane split says what does (round 14)

Following the round-3 direction (exclude MASK triangles before the raster writes depth), the available
instrument was checked against what the raster lane actually consumes. `mask_bake` (the ray-traced shadows'
alphaMode MASK bake) writes a BAKED vertex/index buffer with the cut-out triangles collapsed - a different
geometry stream. The visibility raster draws the original MESH LETS through the meshlet record table
(`first_index/index_count/base_vertex`); a baked index buffer is not addressable by those records without a
second meshlet build, so "reuse mask_bake" is not a wiring change - it is a new baking pipeline for the
visibility meshlets, and its per-triangle approximation (the bake's 4x4 barycentric grid) would still drop
pixels the old fragment path keeps, i.e. a structural difference, not a fix.

THE FIX THE MEASUREMENTS POINT AT is the other candidate: make the visibility RASTER's fragment stage do the
alpha test. It needs three things the mesh stage does not carry today - the item's material index, the
interpolated UV and the albedo-texture fetch - and then a `discard` BEFORE the depth write, which makes the
rasterizer's own depth test fall through to the surface behind: exactly the property the resolve lacks (the
round-3 measurement: 957,719 pixels with no surface behind). The objection to this direction was always
COST - a material fetch in the raster - and the lane split has now priced it: THE MESH-TASK RASTER IS
0.01 ms on the measured frame against the resolve's 0.92 ms. The alpha-tested raster fragment is therefore
affordable, and it is the direction item 2 should be implemented in; no code is claimed here, the plan and the
pricing are the deliverable.

## Item 2, step 1: the visibility fragment now carries the material UV (round 16)

The alpha-tested raster fragment needs the interpolated UV (and, next, the item's material index) before it
can sample the albedo alpha and `discard` before the depth write. This step adds the UV interpolant to the
visibility mesh output and the id fragment's input, emitted from the same 64-byte vertex the mesh stage
already fetches (no new fetch, no runtime change). MEASURED: the ON `deferred` frame is unchanged
(`7EF9249001EB6035`) because carrying an interpolant is not using it; CTest 49/49. The material index + the
alpha test + the admission-clause removal are the next step of item 2, and their ON reference change will be
recorded here with the new hash.

## Item 2, step 2 spec: the push-block extension (round 17)

The alpha-tested raster fragment needs ONE more scalar per item: the material index (the albedo texture and
the cutoff live in the material table). The layout decision, taken before editing so the host/shader mirrors
and the tests move together:

- `visibility::push_block` gains `uint32_t material_index` and one explicit `uint32_t pad`, inserted between
  `frame_slot` (offset 68) and `geometry` (which must keep its 8-byte alignment): offsets become model 0,
  item_index 64, frame_slot 68, material_index 72, pad 76, geometry 80, total 112 bytes
  (`push_block_bytes` 104 -> 112). The explicit pad is not decoration: `geometry_lanes`' first member is a
  `uint2`, so the compiler would insert it silently and the Slang mirror would disagree with the host.
- `scene/visibility.slang` mirrors the same four scalars; the mesh stage passes `push.material_index` through
  to the fragment (a flat `nointerpolation uint`, beside the already-carried UV from the previous step).
- the fragment samples the albedo texture at the interpolated UV (implicit LOD is legal in a fragment stage)
  and does the glTF MASK rule (`flags & 16`, alpha < cutoff) with a `discard` BEFORE the depth write - the
  rasterizer then lets the surface behind win, which is what the resolve cannot do (957,719 pixels with no
  surface behind, round 3).
- `runtime::record_visibility_items` fills the new field from the leaf's material index; the load-bearing
  detail is the ORDER the shader and host mirrors are updated in: constants, static_asserts, mirror test,
  host writer, then the shader, so a mismatch is a test failure rather than a validation error at run time.
- only after that are the admission clause `masked_material` and its unit test removed, the ON references
  re-measured (deferred has no MASK leaf: its hash must stay `7EF9249001EB6035`; sponza becomes admitted and
  is the A/B that proves the cut-outs), and the review table extended.

No code this step: the offsets and the order are the deliverable, so the next step is mechanical.

## Item 2, step 2 landed: the push block carries the material index (round 18)

Per the round-17 spec: `visibility::push_block` is now 112 bytes - model 0, item_index 64, frame_slot 68,
material_index 72, explicit pad 76, geometry 80 - with the static_asserts, the mirror test's constants and the
Slang `VisibilityPush` all moved together (the test caught the one hard-coded geometry offset in the mirror
test, which is exactly the failure order the spec asked for). `record_visibility_items` fills the new scalar
from the leaf's material index, and the mesh stage passes it to the fragment as a flat `nointerpolation uint`
beside the UV from the previous step. The alpha test itself is NOT in yet - the fragment carries the input and
ignores it - so the frame is unchanged: ON `deferred` = `7EF9249001EB6035` as required, CTest 49/49, and the
frozen OFF path is untouched (no OFF code changed; the visibility shaders are only used by the ON lane).

## Item 2 landed: the MASK frames are CARRIED (round 19)

The visibility RASTER fragment now runs the glTF alphaMode MASK test - material table fetch, albedo alpha,
`discard` BEFORE the depth write - so a cut-out pixel keeps the surface behind it (the property the compute
resolve lacks), and the `masked_material` admission clause is retired with its unit test replaced by "a MASK
leaf is carried". Measured on this build:

| frame | lane | ON vs OFF |
| --- | --- | --- |
| deferred (no MASK leaf) | visibility | hash unchanged `7EF9249001EB6035` - the alpha test changes nothing without a MASK material |
| sponza (alpha-tested foliage/interior) | **admitted, 62 items** (was a whole-frame fallback) | 69.04% pixels changed, frame mean 3.674, mean over changed 5.32, max 179 - the foliage/interior differences are the known LOD+cutout-edge territory, and the resolve's own counter says the pixels are no longer "no surface behind" |
| transparent_blend (AlphaBlendModeTest: OPAQUE/MASK/BLEND mix) | **admitted, 7 items** (was a whole-frame fallback) | 4.45% pixels changed, frame mean 0.054, mean over changed 1.21, max 29 - the closest ON/OFF agreement of any multi-material scene so far |

The deferred ON reference and the frozen OFF gate do not move (no OFF code changed; the ON reference scenario
has no MASK leaf). CTest 49/49. Two frames that used to be refused whole - the alphaMode MASK pair - now
record the new path, which is item 2's stated goal.

## Correction to the round-19 claim, and where sponza's residual actually lives (round 20)

Round 19 said the resolve's counter "says the pixels are no longer no-surface-behind". That wording was too
strong; the corrected reading, with the measurement that settles it:

- sponza ON vs OFF, structural-hole proxy on the saved frames: pixels differing by >16 went from 3.591% (the
  pre-alpha-test frame, round 3's MASK-bypass capture) to 3.412% with the alpha test; >64 from 0.995% to
  0.951%. The MASK cut-out handling is CORRECT (transparent_blend: 4.45% changed, the closest multi-material
  agreement measured), but it barely moves sponza's picture - so the residual large differences there are NOT
  mostly MASK pixels.
- the resolve counter after the alpha test is 858,868 over the session (~10.7k pixels/frame, ~1.0% of the
  frame) of "could not rebuild their triangle", labelled by the counter as near-plane/degenerate - that is
  the size of the next item (near-plane clipping), not a MASK residue.
- the deferred ON reference and the frozen OFF gate are unchanged by the alpha test (no MASK leaf in the
  reference scene; no OFF code touched), CTest 49/49.

Item 2 is therefore complete on its stated goal (both MASK frames are carried); item 3 (near-plane clipping)
is the next quality lever, with the counter above as its acceptance measurement.

## The near-plane share is measured, and item 3 is sized (round 21)

The resolve's unresolved pixels now have a second counter (`mesh_stat_visibility_clipped`, stat lane 7): the
pixels whose triangle crossed the NEAR PLANE, counted where `clip_to_screen_pixel` fails. On sponza ON
(62 items, the alpha-tested MASK frame admitted in item 2):

  visibility resolve: 858,868 pixels could not rebuild their triangle,
                      OF WHICH 852,390 (99.25%) CROSSED THE NEAR PLANE,
                      0 had an id/depth disagreement

So item 3's target is precise and nearly the whole residue: 852,390 session pixels (~10.6k per frame, ~1.03% of
a 1080x960 frame) are triangles with a vertex behind the eye - exactly what a Sutherland-Hodgman clip against
the near plane recovers; the remaining 6,478 are degenerate/id-corruption cases that a clip does not address.
Deferred ON and the frozen OFF gate are unaffected (no OFF code changed; the deferred reference scene has no
near-plane triangle in view), CTest 49/49. Item 2's own claim stands as corrected in the previous section; the
`masked_material` clause is gone and the two MASK frames record the new path.

## Item 3 landed: the near-plane clip recovers the holes (round 22)

The resolve now clips a triangle that crosses the eye plane instead of giving up: the same three clip-space
vertices go through Sutherland-Hodgman against `w >= 1e-5` with the attributes riding the same edge parameter
(linear interpolation in homogeneous space - exactly the space the rasterizer interpolates a clipped edge in),
the polygon is fanned back into triangles, and the fan triangle whose screen barycentrics contain the pixel is
rebuilt with the existing depth/attribute pipeline. The MASK verdict is no longer taken in this stage at all:
the raster fragment owns the depth test and already discarded the cut-outs (item 2), so re-cutting a pixel the
raster KEPT would be a hole. MEASURED on sponza ON (62 items):

| counter | before clip | after clip |
| --- | ---: | ---: |
| resolve pixels that could not rebuild | 858,868 | **15,539** (-98.2%) |
| triangles crossing the near plane (recovered) | 852,390 | 852,390, all recovered |
| id/depth disagreements | 0 | 0 |

Picture (sponza ON vs OFF): pixels differing by >16 went from 3.412% to **1.993%**; by >64 from 0.951% to
**0.106%**. Deferred (no near-plane triangle in view) is unchanged at the pixel level vs the LOD-fix frame.

The unclipped triangle deliberately keeps v0's exact acceptance rule (a top-left edge can produce a hair
negative barycentric and v0 accepted it; the containment test applies only to a CLIPPED fan triangle), and the
clipped polygon's crossing count is still reported in its own stat lane. The deferred ON reference moved
(0F4CC199AEC42A0D) although its picture is the LOD-fix frame's: the clip changed the shader's codegen and the
reference is re-recorded here with this note as the reason; OFF is untouched, CTest 49/49 pending the final run.

## Item 1: the cull-source hypothesis is REJECTED (round 25)

The leading candidate from round 24 - the two paths culling from different flags, so the shared depth holds a
different surface than the re-shading stage tests against - was implemented and measured (visibility lane now
culls from `leaf->double_sided`, the same fact `primitive::draw` uses; the `depth_equal_stage` clause bypassed
with a temporary env switch for the re-test), and the result is EXACTLY the old failure: bypassed-EQUAL
laevatain ON vs OFF is still 33.45% changed / 20.736 frame mean / 62.0 over changed / max 209 - bit-for-bit
the round-2 numbers. So the cull sources agree on every leaf in these frames and the hypothesis explains
nothing. It is reverted with this record instead of kept as a "maybe correct" change.

The same build re-measured the two admitted double-sided scenes for regressions before reverting: sponza
>16 = 1.993%, >64 = 0.106%, transparent_blend >16 = 0.001%, >64 = 0.0%, deferred ON hash unchanged
(0F4CC199AEC42A0D). Item 1's root cause is therefore still on the DATA side (model/camera bytes or the depth
attachment path), and the depth-readback harness remains the only measurement that can close it - the static
candidates (SPIR-V arithmetic, pipeline state, cull source) are now all eliminated by measurement.

## Item 1: the meshlet-launch counts differ, and that is the next measurement (round 26)

While looking for the data-side difference in the depth-EQUAL failure, a cheap comparison of the runtime's own
`mesh culling` line exposed a large, unexplained asymmetry in the same scene:

| sponza session (80 renders) | geometry cost workgroup launches |
| --- | ---: |
| OFF (geometry lane) | 84,675 (~1,058/frame) |
| ON (visibility raster lane) | 18,870 (~236/frame) |

If the raster lane really launched 4.5x fewer mesh-shader workgroups for the SAME 62 items, its culling-margin
or run-partition arithmetic is not the geometry path's, and that alone can leave the shared depth missing
surfaces the re-shading stage then tests against - the exact shape of the EQUAL failure. THE COMPARISON IS
NOT YET CLEAN, though: the OFF number includes the shadow/other passes that also dispatch mesh work, and the
ON number counts only the lanes that ran, so the two figures are not the same measurement. The next step is
narrow: split the counter per PASS and per LANE on one admitted scene (the stat lane mechanism is there), then
compare "meshlets emitted / culled" between the geometry lane and the raster lane for the same 62 items. A
difference in EMITTED counts is the mechanism; equal emitted counts retire this lead.

## Two records: the full frozen gate on the current tree, and the launch-count lead weighed (round 27)

1. HARD CONSTRAINT, re-verified end to end after the item-2/item-3 shader work: the full frozen OFF round on
   the current tree is **15/15 matched, 0 mismatched, PASS** against the legacy hashes. The MASK alpha test and
   the near-plane clip are ON-path shaders, and the stat-lane addition recompiled the shared mesh-geometry
   header without moving a single OFF frame - which is what the mirror/statistic tests are there to keep true.

2. The round-26 lead (raster lane launching ~4.5x fewer mesh workgroups than the geometry cost counter on
   sponza) is NOT missing geometry: if the raster lane culled meshlets the old path draws, the base frame would
   lose whole surfaces, and the measured base ON-vs-OFF difference is small per changed pixel (16.6% of pixels,
   mean over changed ~15.5/255 at v0.6, dominated by shading/LOD rather than background-through-geometry,
   which would read near a hundred). The launch-count difference is therefore attribution (the OFF counter
   includes passes the ON arm does not run, e.g. the shadow lane's share), not a cull divergence - retired as a
   root cause for the depth-EQUAL failure unless the per-pass split says otherwise.

Item 1 therefore still needs the depth readback + input-byte dump to close; that is recorded as its entry point.
