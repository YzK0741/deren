# Visibility-buffer probes (three read-only investigations, 2026-10-18+)

Three independent read-only probes were dispatched after the "non-extension" work: (A) the toon
depth-EQUAL mismatch, (B) a depth-readback harness design, (C) the material-binning decision. This file
records their findings so the next session can act without re-doing the investigation. Nothing here is
implemented unless it says so in the review record.

## (A) Depth-EQUAL root cause: two live hypotheses, one eliminated area

Verified from code (facts):
- The character-forward pass and the visibility raster lane share the SAME `gbuffer_depth` image, view and
  per-swapchain-image instance; the character pass LOADs it, nothing between the lanes writes/copies/clears
  depth (`runtime.post_graph.cpp:267-274,538,786`; `character_forward.cpp:76-79,108-110`). The
  "attachment path" suspect is eliminated.
- Both stages read the same camera heap slot via their own push `frame_slot` (disassembled: both
  `OpIAdd %uint_16898, <frame_slot>` with view/proj at the same offsets), and both take `leaf->push.model`.
- Clip-position SPIR-V is identical in both modules.
- The resolve's depth check uses a 1e-3 NDC tolerance, so "0 mismatches" never disproved a ULP difference.
- The failure is a fine per-pixel hatch over the whole character (EQUAL capture), i.e. ULP-scale, not a data
  mix-up or missing geometry.

Live hypotheses, ranked:

1. **H1 - the character stage's vertices take `pbr_shade_vertex`'s morph→skin→world chain even for this
   (unskinned) asset** (`pbr.slang:85-89,160-184`; loader gives unskinned vertices weights `(1,0,0,0)` at
   `gltf_loader.cpp:1121-1152`, so `wsum>0` takes the skin branch and reads joint matrices + divides),
   while the visibility raster lane computes `mul(float4(v.position,1), push.model)` directly
   (`visibility.slang:138`). With `skin_base==0` the joint slot's content decides whether this is exactly
   identity; if it is not, the two stages' depths differ on every character pixel - matching the hatch.
   **Cheapest test:** force weights to `(0,0,0,0)` for meshes without JOINTS_0 (or gate the skin branch on
   `push.skin_base != 0`), then re-run the EQUAL A/B on `laevatain_old_chain`; the hatch should disappear if
   H1 holds. This is a small, testable code change.
2. **H2 - cull inputs differ: host meshlet culling uses `view_proj_unjittered` (`runtime.cpp:1548-1555`,
   `primitive.cpp:86-118`) while the raster lane's GPU cull uses the JITTERED proj (`visibility.slang:107`),
   and every failing capture has TAA on.** A meshlet present in one depth and absent from the other cannot be
   recovered by LESS_OR_EQUAL - this is the natural explanation for the meshlet-shaped residual blobs the
   LEQUAL A/B still shows. **Cheapest test:** run the LEQUAL A/B with TAA off, or make the character pass
   cull against the jittered proj; compare residual. Independent of H1.
3. **H3 - cross-module rasterization/rounding divergence (needs the readback harness to see raw bits).**
   Only if H1/H2 are refuted; the harness plan (B) is the way.

Recommended single fix once the mechanism is known: a depth-only prepass in the character pass that writes
the G-buffer depth with the character pipeline itself (candidate (a) from the plan), so the re-shading stage
tests against depth its own pipeline produced. Cost ~one extra 7-item geometry pass, sub-ms.

## (B) Depth-readback harness: minimal plan

No depth readback exists today; the only readbacks are swapchain screenshot and the Gaussian HDR color
capture. Phase 1 (~150 lines, 4 files) gets the depth the character stage TESTS against:
1. `output_targets.cppm` family 6: add `transfer_source` to the depth family's flags.
2. `core.api_core.cpp` owned-image copy: derive bpp from `resolved_format` (D16=2 / D24 / D32=4) and use
   `aspect_for(resolved_format)` instead of the hardcoded COLOR aspect; log the VkFormat once.
3. `runtime.post_graph.cpp` beside the Gaussian HDR capture hook: env-gated copy `gbuffer_depth` →
   host-visible buffer (precedent lines ~951-985).
4. `runtime.readback.cppm`: write `<prefix>.depth.bin` + `<prefix>.inputs.bin` (camera blocks, per-leaf push
   payloads, leaf lists) after `wait_idle()`; add an env switch `DEREN_CHARACTER_DEPTH_EQUAL` to reproduce
   the EQUAL failure for the measurement frame.
Phase 2 (~180-250 lines) captures the depth the character stage PRODUCES (debug depth image + throwaway
color target, env-gated graph addition). CPU comparison script classifies pixels: never-written / bit-equal /
1-ULP / other, plus `memcmp` on camera blocks and push payloads. Cheapest first alternative: copy the
R32_UINT visibility ID buffer (it already has `transfer_source`? add it) and decode ids on CPU - answers "is
the stored surface the one the character stage drew" without any depth-format work.

## (C) Material binning: evidence says do NOT build it

Per-pixel resolve work enumerated (id 4 B, depth 4 B, item row 112 B, meshlet ~12-48 B, indices 6-12 B,
3 vertices 96 B, prev-matrix 64 B, material row 80 B/item, ~4.5 filtered texture taps, five stores 28 B/px,
plus the near-plane clip's 60-float scratch on every pixel).

- Stores: 29.0 MB/frame → 31.6 GB/s at 0.92 ms; fixed reads id+depth 8.3 MB → 9 GB/s. **Not
  write-bandwidth-bound** (~16% of an assumed 250 GB/s).
- Texture: ~2.0 Gfetch/s ≈ 1-2% of the TMU budget. **Not texture-throughput-bound.**
- ALU estimate ≤0.2-0.3 ms. **Verdict: latency/occupancy-bound (medium confidence)**, the likely culprits
  being the id→item→meshlet→vertex→material dependent chain, register pressure from the near-plane clip block
  (60 floats of scratch on every pixel even when `behind==0`), and divergence.
- Material coherence is already maximal: material is per ITEM (one helmet = one material row for the whole
  measured frame; sponza tiles stay inside one item except at silhouettes). **Binning buys ~0% coherence on
  the frame it was sized against.**
- Ceiling argument: the whole OFF fragment G-buffer pass (same shared decode + same 5 targets) is 0.19 ms,
  so any material-only optimization is bounded by ~21% of the resolve's 0.92 ms; the 0.73 ms gap is geometry
  reconstruction, which binning does not remove.

Ranked alternatives (all shader-local):
1. **Clip fast-path**: when `behind==0`, use the direct projection path; keep the clip path only for crossing
   triangles (expected 5-25%, tests the register-pressure hypothesis; ON hash must stay `0F4CC199AEC42A0D`).
2. Multi-pixel per thread with item/meshlet/matrix hoisting (20-40% if latency-bound).
3. Scoped `-O2/-O3` for this shader (Slang default is -O1 today; ON hash may move, re-baseline with reason).
4. Micro-trims: depth load after the valid-id early-out, TBN behind the normal-map flag, extent lane instead
   of per-thread `GetDimensions`.
5. Material binning v2 LAST (ceiling ≤0.19 ms, no coherence win, needs a new RHI indirect-compute verb).

Decisive cheap measurement before anything: re-measure the split on the CURRENT tree (the 0.92 ms predates
the near-plane clip), then a coverage-scaling run (camera far away, same lane) for the fixed per-pixel floor,
then an env-gated "constant-surface" resolve variant - `S = M0 - M1` is the absolute ceiling of any
material/bin scheme. Decision rule: build only if `S >= 0.30 ms`; arithmetic predicts 0.05-0.15 ms, i.e.
binning should be killed, and the optimization budget should go to the clip fast-path.
