# The visibility buffer lane (experimental)

**Status: implemented, experimental, off by default.** This page is the manual's description of the lane's
current shape, its admission rules, its gate and its measured limits. The design record is
`docs/superpowers/plans/2026-10-17-visibility-buffer.md`; the measurements and known differences are in
`docs/reviews/2026-10-18-visibility-buffer-ab.md` (with the follow-up probe record beside it).

**The LOD above this lane is Subtilis**: its screen-error cut chooses which cluster records this lane rasterizes
(off by default). See [Subtilis](subtilis.md) for the builder, the cut, the capacity contract and the measured
numbers that the OFF/ON evidence in this page predates.

This lane is shared renderer infrastructure. Subtilis supplies geometry selections; the resolve can emit
visible-material and screen-footprint demand for [amnis](amnis.md) texture streaming. amnis can consume
that feedback without Subtilis DAG LOD, and Subtilis can use fully resident textures without amnis streaming.
Geometry selection and texture residency retain separate policies and configuration gates.

An additional experimental mode, `[render] software_raster=true`, moves conservatively classified sub-pixel
clusters from the hardware mesh-task prefix into a tiled CSR software raster. It requires both the visibility lane
and Subtilis and is default-off. It composes with HZB culling and recovery: HZB evaluates both the hardware prefix
and software suffix, while software raster filters its suffix using the final HZB visibility/recovery flags. The
software candidate is resolved before a depth-only merge updates the G-buffer depth. See
[Subtilis M4](subtilis.md#10-milestone-status) for the recorded evidence and the final closeout: the mode
remains optional/default-off and is not optimized further.

A second opt-in mode, `DEREN_SOFTWARE_RASTER_GPU=1`, moves the software-candidate routing itself into the HZB
compaction: the compute stage classifies and reserves the bounded software list, emits the hardware survivors and
the compute-indirect arguments, and the software kernel consumes those arguments without a production CPU count
readback. Capture-only snapshots validate the hardware/software partition against the HZB flags/survivors (both
frame slots, closed-history fallback, forced budget exhaustion, nonzero recovery). The mode is default-off and its
same-build performance matrix shows no stable whole-frame win over software-off DAG+HZB; the full-stage
resolve/merge verifier remains unconnected. The mode is closed as an optional negative result and is not
optimized further. See the [routing checkpoint](reviews/2026-10-23-software-routing.md) for the evidence and
limitations and the [final closeout](reviews/2026-10-23-subtilis-final-closeout.md).

## What it is

The deferred scene path has two interchangeable opaque producers:

1. the fragment G-buffer lane (the default, always available), and
2. the **visibility buffer lane**: a mesh-shader raster writes only a packed 32-bit surface id plus the shared
   G-buffer depth, and a compute **resolve** rebuilds the five G-buffer targets from that id.

Everything after the opaque handoff is identical: the depth is the frame's own G-buffer depth, the lighting
stage samples the same targets, and the toon re-shading stage tests the same depth. The switch is
`[render] visibility_buffer` (`false` by default, `source/application_configuration/application_configuration.cppm`).

```
[mesh] visibility raster   ->  R32_UINT id  +  shared gbuffer_depth
[compute] visibility resolve ->  albedo/metallic, normal/roughness, material/AO/flags, velocity, scene color
[vertex/fragment] lighting, transient, character, toon rims, gaussian, TAA, post (unchanged)
```

## The id

One 32-bit word: valid(1) + item(12) + meshlet(12) + triangle(7). The resolve looks up the draw item, then the
meshlet record, then the triangle inside that record's index window - the same 48-byte meshlet record the
fragment G-buffer path uses. A triangle at id 0 must not read as "empty", which is why the valid bit exists.

## Admission and fallback

The lane records only when the whole opaque frame can be produced from the item table. A frame that needs any
of the following falls back to the fragment G-buffer lane as a whole (never a partial picture):

- deforming geometry (skinned or morphed leaves),
- instanced draws,
- custom pipelines / non-meshlet leaves,
- missing geometry or no meshlet run,
- meshlet or item table overflow,
- frames with no opaque draws at all.

The fallback is a runtime decision frozen once per frame, logged once per session with the reason. The toon
re-shading stage that draws opaque character leaves with an exact depth test works on this lane: the measured
depth mismatch that used to force a fallback was a clip-position expression difference, fixed by spelling the
clip position as the nested matrix product the toon pipeline uses (the two stages' depth is now bit-identical).

## Material MASK

Alpha-tested (glTF `alphaMode` MASK) materials are handled by the **raster fragment**: it samples the material's
albedo alpha and discards before the depth write, so a cut-out pixel lets the surface behind win the id and the
depth. The compute resolve does not re-run the cut-out test; a pixel with a valid id means the raster kept it.

## What changed recently (all measured)

- **Material mip selection**: the resolve samples material textures at a LOD derived from the triangle's
  UV-vs-screen scale, not at LOD 0. The deferred A/B against the old lane moved from 6.88% to 4.57% of pixels,
  with the mean over changed pixels from 8.05 to 2.74 (off path byte-identical).
- **Near-plane clipping**: triangles that cross the eye plane are clipped (Sutherland-Hodgman) and fanned back
  into triangles instead of being dropped. On Sponza the resolve's unresolved-pixel count fell from 858,868 to
  15,539 per session (-98.2%), and pixels differing from the old path by more than 64 went from 0.951% to
  0.106%.
- **MASK materials** are carried by the lane (Sponza and the AlphaBlendModeTest scene both record it), and the
  old whole-frame fallback for them is retired.
- **Depth-EQUAL re-shading** no longer forces a fallback (see above); all four toon scenarios record the lane
  and their frames measure 0.00-2.13% of pixels different from the old path, with the per-changed-pixel
  magnitude at the level of the other admitted scenes.

## Configuration and gate

- `[render] visibility_buffer = false` - off by default; when off, the lane does not record and the frozen
  off-path references are byte-for-byte unchanged.
- A/B capture: run the capture harness with the switch appended, e.g.
  `pwsh scripts/gates/check_render.ps1 -Only deferred -ExtraConfig 'visibility_buffer = true'`.
- Frozen reference for the on path: `docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt` (the
  `deferred_visibility` scenario), checked with `-Visibility -Compare <file>`. The legacy frozen round remains
  `docs/reviews/2026-10-17-render-graph-gate/legacy-hashes.txt` (15/15) and does not include the visibility
  tier.
- Frame-to-frame A/B of the two producers: `scripts/measure/visibility_ab.py` over two capture directories.

## Known limits and cost

- The lane is an alternative opaque producer, not a faster one yet: measured on one 1080x960 deferred frame,
  the mesh raster bucket is ~0.01 ms while the compute resolve bucket is ~0.68 ms (range 0.64-0.96 over three
  runs) against the fragment G-buffer pass's ~0.19 ms, so the on-path total stays above the off path
  (~1.21 vs ~0.94 ms). The resolve is the cost centre, and three repeated runs cannot resolve a change below
  about ten percent; performance claims need five or more repeats or the benchmark CSV.
- Differences from the fragment path that are expected, not bugs: material texture fetches use an explicit LOD
  rebuilt from the triangle (the per-pixel derivative and anisotropic choice of a fragment sampler are not
  reproduced), and the tangent frame comes from the triangle's edges rather than screen-space derivatives.
- Near-plane geometry is now clipped, but the resolve remains a reconstruction: a triangle with a corrupt id or
  a degenerate screen footprint is still reported and left empty.
- Deforming, instanced and custom-pipeline frames still fall back by design; making the visibility lane the
  default requires a performance result that does not exist yet.

## See also

## Same-frame texture residency (experimental)

`[render] visibility_texture_streaming = true` opts in alongside `visibility_buffer = true`. Geometry
remains resident. Scene registration keeps whole decoded images in shared CPU storage and initially
publishes ordinary placeholders for unneeded textures. A full-resolution compute scan identifies this
frame's visible materials, including the shared hardware/software winner rule and HZB recovery output.
Feedback is512 words plus status (2052 bytes); the coherent mapped buffer is published for host reads
and read only after a graphics frame checkpoint completes.

The graph's compiled schedule is partitioned before resolve, preserving compiled resource barriers
and ownership. After the prefix completes, required textures load synchronously, unneeded descriptor
slots return to placeholders and unload is attempted; a remaining command/group lease defers physical
reclamation. The suffix shades the same frame with the same camera/items. Newly visible required
textures must load before shading; upload refusal fails the frame explicitly.

MASK alpha-source, toon/overlay/transparent material textures and renderer defaults stay pinned.
Shared textures inherit pinning. An ineligible opaque lane or active ray-traced shadow uses full
residency. Disabling streaming requests one safe full-residency restore before ordinary graph drawing.
Enable at startup before importing materials; already eager images are not retroactively converted.
Configuration/scene changes follow the existing serialized frame boundary.

This is whole-image GPU residency, not mip/page streaming. CPU decoded sources remain allocated.
Logged payload counts distinguish total/pinned/physically ready bytes and deferred unloads; they do
not measure VMA block release or process VRAM. Frame checkpoint and synchronous upload can cause
substantial stalls. The option is default-off and not an FPS optimization. See the approved
[design](superpowers/specs/2026-10-26-vbuffer-residency-design.md) and execution record beside it.

## See also

- `docs/mesh_shaders.md` - the mesh-shader path the raster lane runs on.
- `docs/render_graph.md` - the frame's lanes and their ordering.
- `docs/shaders.md` - the shared shader bodies and slot conventions both producers use.
