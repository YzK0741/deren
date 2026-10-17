# Render graph adoption: stage 1, the opt-in slice measured at gate scale

Stage 1 of the adoption plan: run the EXISTING opt-in main graph through the render gate
before writing any new declaration, because the delivered slice's evidence was a single
DamagedHelmet frame set (256x256, 8 frames, one camera) and could not say which scenarios
the graph actually covers.

## Method

Two rounds against the SAME binary, same session, same driver, differing only in the
injected `[render] main_render_graph` key:

```
pwsh -File scripts/windows/check_render.ps1 -Full -BuildDir build-release-dyn-clang64 -MainGraph
pwsh -File scripts/windows/check_render.ps1 -Full -BuildDir build-release-dyn-clang64 -Compare <round A hashes>
```

Two small harness changes were needed and are part of this commit:

- `-MainGraph` injects `main_render_graph = true` into every generated scenario config
  (a scenario that names the key itself still wins, because TOML rejects a duplicate).
- every run's `debug.log` is now copied next to its screenshot as `<scenario>.debug.log`.
  Without it the next scenario deletes the log, and the activation/fallback inventory IS
  that log.

## Result 1: which path each scenario ran (from the runs' own logs)

| scenario | path in its 40-frame run |
| --- | --- |
| deferred, deferred_ssao_off, glossy_motion, metal_rough_glossy, shadow_single, sponza, unlit | **graph only** |
| deferred_taa_fxaa, deformation, laevatain_goo_toon, laevatain_goo_toon_body, laevatain_no_sidecar, laevatain_old_chain | **graph first, then legacy** (both lines present) |
| transparent_blend | **legacy only** |

The first inventory pass (taking the first matching line per log) read as "13 of 14
activated". That is an artefact of the logs: the activation line is printed ONCE
(`main_graph_reported`, runtime.post_graph.cpp:360) and only after `prepared->record()`
returned ok, so its presence means "at least one frame was recorded through the graph" -
not "the run used the graph". Six scenarios contain BOTH lines, activation first and the
fallback 6-8 log lines later, so their run recorded a graph frame and then switched to the
legacy tail for the rest; the logs do not carry a frame index, so the number of graph
frames is not recorded, but it is not the 40 the round ran.

Why the eligibility flips mid-run is NOT answered here. The predicate is live state, not
configuration: `taa_active()` is `taa_on && pass_ready("taa") && deferred_lit_active()`
(runtime.frames.cppm:1574) and `feature_active()` delegates to a backend predicate over
per-frame feature facts (runtime.cpp:996). A term that becomes true after the first frame
- pass readiness, megalights activity, a character stage's facts - flips the frame from
graph to legacy. That is the first thing stage 2 must pin, because it decides whether the
gate's A/B evidence covers the graph path at all for those scenarios.

Consequence for the plan: today the graph covers the WHOLE run in 7 of 14 scenarios. The
other six are mostly legacy and must not be counted as graph-covered; `transparent_blend`
is the only clean fallback (the documented exclusion works).

## Result 2: byte identity, at full SHA-256

Every scenario's frame is byte-identical between the graph round and the legacy round -
14 of 14, compared as full 256-bit SHA-256 of the captured PNG (13 from the two rounds'
`*.actual.png` sets plus `deformation`, re-run in both configurations because it produced
no `.actual.png`). Examples:

```
deferred            FAE60C0B1D8C119188236E31...   identical
sponza              24D55DE5C6E814EEB87EDC5D...   identical
transparent_blend   4D3FD6ACB2953BE4BDB59831...   identical
deformation         723569BA0D03640C16B3C62AD88A648E03A3457793344F030AD44BBAECF36F81  identical
```

Honest reading: for the six mixed scenarios this mostly compares legacy against legacy
(39-40 of their 40 frames each), so the identity there is weaker evidence than it looks.
The seven graph-only scenarios are the ones where the comparison really is graph vs legacy.

## Result 3: the frozen baseline is stale on this machine (not a graph finding)

The rounds report `changed: 13` because they are compared against the machine-local
reference set, and that set no longer matches this machine/driver. The control that
separates the two:

- graph OFF, `deferred`, `-Compare frozen` -> hash `FAE60C0B1D8C1191`, MISMATCHED against
  the frozen `972A31EC5FF55C87`;
- graph ON, same scenario -> the SAME `FAE60C0B1D8C1191`.

So the frozen fourteen are stale here (the harness header already warns that references
are per-driver values), and the meaningful verdict in this session is the two-round
`-Compare <round A>` one, which reported `matched 14, mismatched 0` / `PASS (14 of 14)`.
Re-baselining the frozen list is a separate, deliberate act (`-Update`) and is NOT done
here.

## Result 4: documentation corrected while measuring

- the stage plan's checkboxes for 2.2/A2, 2.3/B and 2.4/C were still unchecked although
  the 2026-10-16 records accept them; 2.5/D and 2.6 gained an explicit status note;
- `docs/render_graph.md`'s limits paragraph still said execution binding/recording were
  pending while the same file's later sections document them as delivered;
- the Boost review's "not implemented here" line now points at the A1 record that
  superseded it the same day.
- the graph manual's exclusion list needs one caveat that this round measured: those
  stages select the legacy tail WHEN THE PREDICATE SAYS THEY ARE ACTIVE (live state), not
  when their config key is true - `deferred_taa_fxaa` sets `taa = true` and still recorded
  a graph frame.

## Evidence in this directory

- `maingraph-hashes.txt`, `legacy-hashes.txt`: the two rounds' per-scenario hashes.
- `maingraph-logs/<scenario>.debug.log`: the activation/fallback lines, one copy per run.
- `maingraph-configs/<scenario>.toml`: proof the key was injected (`main_render_graph = true`).
- `maingraph-deformation.png`, `legacy-deformation.png`: the one scenario re-run for a
  full-hash comparison.
- The full capture set (13 `.actual.png` per round, ~4 MB each) stays in the ignored build
  directory `build-release-dyn-clang64/render-check/`; the SHA-256 values above are the
  evidence, and the round is reproducible with the two commands in "Method".

## Addendum: the flip explained - the scenario's toggles land after the first frame

The fallback line now names the term that refused the frame (`runtime.frames.cppm`, printed once),
and the three mixed scenarios answered:

```
deferred_taa_fxaa   main graph: unsupported tail combination (TAA active) ...
deformation         main graph: unsupported tail combination (deferred lighting inactive) ...
laevatain_goo_toon  main graph: unsupported tail combination (character forward) ...
```

Readiness is NOT the cause: the TAA resolve pipeline is created at log line 642 and the
character-forward pipelines at 518-530, both well BEFORE the graph frame at 1004/1612 - so
`pass_ready()` was already true. What changes is the FEATURE STATE, and it changes because these
toggles do not reach the runtime from the configuration directly: they are copied into the GUI
binding struct and mirrored into the runtime by the frame loop
(`source/app/main.cpp:2520-2535` - `gui.character_forward = settings.render.character_forward;`
and the comment beside it says a config value that never reached the binding "would be overwritten
by the binding's default on the very first frame"). The first frame's eligibility is frozen before
that mirroring, so frame 0 is evaluated under the binding DEFAULTS:

| scenario | frame 0 was evaluated with | the rest of the run with |
| --- | --- | --- |
| deferred_taa_fxaa | TAA off | TAA on |
| deformation | G-buffer debug view off (its config sets `gbuffer_debug = true`) | debug view on, so deferred lighting inactive |
| the four laevatain_* | character forward off | character forward on |

What this is NOT: a defect. Frame 0 is not "wrongly claimed" by the graph - the renderer's own
semantics are that a toggle mid-run is seen by the frame AFTER it (see the note at
`source/app/main.cpp:2860`), so frame 0 genuinely runs WITHOUT TAA, without the debug view and
without the character stages. The graph covering that frame is correct for the frame's actual
feature state; what it is not is coverage of the scenario's named features.

The consequence is a COVERAGE statement, and it is the actionable half:

- 7 scenarios (deferred, deferred_ssao_off, shadow_single, sponza, unlit, glossy_motion,
  metal_rough_glossy) record the whole run through the graph, and their A/B identity really is
  graph against legacy;
- 6 scenarios (deferred_taa_fxaa, deformation, the four laevatain_*) exercise the graph for frame 0
  only; their A/B identity is legacy against legacy for the remaining 39 frames, so it must not be
  read as "the graph path is witnessed for TAA / the debug view / the character chain";
- transparent_blend is legacy only (the documented exclusion works).

An attempt to "fix" this by refusing the graph on the run's first frame (an `main_graph_armed`
latch) was written, measured and REVERTED: the suite rejected it -
`source/tests/test_render_graph_post.cpp:255` requires `graph == 1 && legacy == 0` for a
`main_render_graph = true` single-frame compose, which is exactly the contract "the first frame goes
through the graph", and the latch also contradicted the renderer's own toggle semantics. The reason
naming stayed; the latch did not.

So stage 2 inherits two jobs, in this order: (a) the declaration work below, and (b) when a stage
wants to claim a scenario as graph-covered, it needs the scenario to actually stay on the graph for
the whole run - which for the six means the features that exclude it must be either migrated or left
out of that scenario, not "the first frame counts".

## Stage 2B: the two transient-descriptor barriers are outside the graph's content model

Stage 2's first planned step was to take the last two hand-issued barriers into the graph. On a
main-graph frame `runtime.post_graph.cpp` issues an `undefined -> shader_read` transition for the
ray-traced visibility image and the MegaLights resolve image, because the deferred lighting descriptor
set keeps both statically bound even when the predicate excludes their producers. That makes the runtime
a second barrier owner inside a graph-recorded frame - the thing the migration rule forbids in the long
run.

Three attempts, three measurements, one conclusion: the graph core cannot express this transition today.

1. Importing both images with `initial_contents::undefined` and declaring them as sampled reads of the
   lighting pass is rejected at COMPILE. An active read must have defined contents
   (`render_graph.cpp:254` returns `error_code::undefined_read`; the manual says the same).
2. Importing them with `initial_contents::defined` and leaving the prior state empty is rejected by the
   IMPORT itself: the builder will not accept "the contents are defined" without the state those
   contents are in, so the main-graph frame fails to build (`test_render_graph_post` reported the
   failed frame) - and the same mistake in a unit test surfaces later as a `foreign_ref`, because the
   unchecked import leaves a default ref behind.
3. Deleting the two barriers outright leaves the graph-only scenarios byte-identical and
   validation-clean (`deferred` FAE60C0B..., `sponza` 24D55DE5..., zero VUID/ERROR/WARNING lines), but
   that is NOT evidence enough: these images are bound through the descriptor heap, and a heap-bound
   sampled image's layout state is exactly what the layers are weakest at. The project's own comment
   ("Inactive statically bound descriptors retain their existing external layout protocol") records
   that the barrier exists for this protocol, so this record does not delete a barrier on a blind spot.

The barriers stay, with the reason written down: the transition is not a data dependency the graph
models - it is descriptor-layout initialization for an image whose contents are deliberately undefined.
Taking it over needs one of two real changes, both bigger than a declaration:

- a graph-core concept for a "descriptor-initialization transition" (a read of contents that are
  undefined by design), which the content model deliberately refuses today; or
- pointing those two heap descriptors at a known-good dummy image on main-graph frames, so no
  transition is needed at all - a descriptor-heap change rather than a declaration.

Consequence: stage 2 proceeds with the declaration work, and the two barriers remain a tracked
out-of-model exception.

## Stage 2C: the GPU-driven geometry inputs are declared

The shared-read-only bundle covered only the eight scene buffers. The mesh stage's own inputs stayed
undeclared: the meshlet records it reads, this frame slot's host-culled run, and - the review's explicit
item - the statistics counters it updates with `InterlockedAdd`. They are now part of the geometry
pass's declaration:

- `geometry::shared_inputs` grew an optional `meshlet_inputs{table, culled, statistics}`, declared as
  `buffer_read` {storage, mesh_shader} for the two tables and `buffer_read_write` {storage,
  mesh_shader} for the statistics. The buffers themselves stay owned by the scene-tables producer and
  are still created outside the graph - what changed is that the graph now knows the pass touches them,
  so it owns their ordering instead of leaving them invisible.
- the runtime's `shared_imports` table gained an explicit access column, so the read-write counter buffer
  cannot be smuggled into the read-only bundle: it is imported with a read-write prior state
  (`{general, mesh_shader, read | write}`), the two tables with a read prior state. Import indices follow
  `pass::scene_tables_pass::ids`: 5 = meshlet table, 7 = statistics, 8 = host-culled table.
- **indirect commands** followed in the same increment: the mesh draws are `draw_mesh_tasks_indirect`
  records, so this buffer is declared as a `buffer_read` with the `indirect_buffer` usage at the
  **draw_indirect** stage (import index 6) rather than a storage read by the mesh stage. The geometry
  pass now declares 17 accesses (six stage writes + seven shared reads + four meshlet inputs), and the
  gate round that follows is byte-identical for all fourteen scenarios with zero validation findings.

Evidence:

- `test_render_graph_lighting` now composes the geometry pass with the meshlet inputs and asserts the
  pass's 16 accesses (six stage writes + seven shared reads + three meshlet inputs), the presence of the
  declared group in the compiled parameters, and - by mode - that the statistics are `read_write` while
  the table is `read`. 30 checks, 0 failed.
- CTest 44/44.
- The gate round with the graph on is byte-identical to the same-session legacy round for all fourteen
  scenarios (`matched 14, mismatched 0`), and the validation screening over the fourteen debug logs
  reports zero findings - so the newly declared atomic writes are ordered by the graph's plan without
  tripping synchronization validation.

What this deliberately does not cover (the remaining declaration items):

- **per-mesh addresses** and **material textures/IBL**: descriptor-heap bindings, not buffers of this
  bundle;
- **cluster producers**: the cluster/culling work still runs outside the graph;
- the statistics declaration covers the whole buffer even though the shader addresses one slot's stride;
  a byte-range declaration of just this slot's counters would be more precise and is a possible
  follow-up (the whole-buffer read-write is conservative and correct, not minimal).

## Stage 2D: per-mesh addresses are not a resource the graph can own

The review listed "per-mesh addresses" among the undeclared items. Measured, they are not a resource:
they are four 32-bit PUSH-CONSTANT lanes carrying two 64-bit device addresses for the draw's own vertex
and index buffer (`source/shaders/geometry/mesh_geometry.slang`, struct `MeshGeometryLanes`), filled per
draw from the primitive's buffers right before its dispatch
(`source/engine/primitive/primitive.cpp:133-134`: `lanes.vertex_address = env.buffer_address(...)`). The
buffers themselves are owned by the primitive, not the frame: `static_draw_primitive` holds them as
`object_manager<rhi::buffer> vertex_buffer/index_buffer`
(`source/engine/primitive/primitive.cppm:2213-2214`), established at asset load and read-only afterwards.

So there is nothing for the graph to order: the addresses are push-constant values (the graph declares
resources, not draw data), the buffers have no frame-level producer, and the shader's fetches through
them are plain reads. Declaring them would mean enumerating every drawn primitive's two buffers at graph
construction time - hundreds of imports that would produce zero barriers - so the item is recorded as
out of the graph's model rather than implemented.

The enabler that WOULD make it declarable is a storage decision, not a graph feature: if static geometry
lived in one pooled vertex/index buffer (or a small fixed set), the pool could be imported once as a
read-only input and the per-draw lanes would become offsets into a declared resource. That is
`docs/mesh_shaders.md`-territory (the pooling step), and it is the same shape as the two
transient-descriptor barriers in Stage 2B: **what keeps an item out is a data-layout fact, not a missing
declaration**.

## Stage 2E: the scene-wide IBL images are declared; the material texture set is not

The lighting stage's image set is now complete at the graph level: the prefiltered environment cube, the
irradiance cube and the BRDF LUT (the "material textures/IBL" item on the review's list) are imported as
sampled reads and declared through `lighting::ibl_inputs`. They are uploaded once at startup and
read-only afterwards, so their prior state is the sampled one and the declaration adds knowledge rather
than a barrier.

The other half of that item - the per-material textures - is not declarable, for the same reason as
per-mesh addresses: they are individual images bound through the descriptor heap's 128-slot array
(`owned_textures`/`owned_texture_views`, capacity `scene_texture_capacity`), uploaded per asset. A graph
declaration would amount to enumerating up to 128 dynamically populated images for zero barriers; the
enabler that would make it declarable is a single array image (or a fixed pool) imported once - a
storage decision, not a graph feature.

Evidence: the lighting pass now declares 10 accesses in the test (five stage accesses + two shared
uniforms + three IBL images), 31 checks, 0 failed. CTest 44/44. The gate round with the graph on is
byte-identical to the same-session legacy round for all fourteen scenarios (matched 14, mismatched 0)
with zero validation findings, and thirteen scenarios recorded through the graph.

## Stage 2F: the cluster sort is prepared for the graph, and what its migration needs next

The clustered-light sort (`pass::cluster_pass`) is the review's "cluster producer" item. Its migration is
two steps; the first is in: `cluster_pass::record_content()` now records the dispatch alone, while the
legacy `record()` keeps owning its two buffer barriers (`shader_write -> shader_read`) - the same
`deferred`/`bloom` split the graph path already uses through `post_recording_payload::record`, so a
graph-recorded frame can never execute both barrier paths for one submission. The legacy path is
unchanged (deferred and sponza keep their hashes) and the suite is green.

The second step's prerequisites, measured rather than assumed:

- the main-graph eligibility is frozen in `record_scene()`, which runs AFTER the cluster stage inside
  `record_main_drawcalls()` - so "skip the legacy stage, the graph records it" requires moving the freeze
  to the top of `record_main_drawcalls()`;
- on a main-graph frame the sort's only consumers are the in-graph deferred lighting pass (heap bindings
  11/12); the legacy shadow pass does not read the cluster lists (`shadow.cppm` has no cluster reference -
  it renders depth from the light), so moving the sort inside the graph does not starve a legacy consumer.
- the two per-slot cluster buffers come from a managed snapshot (`cluster_counts`/`cluster_indices`), so
  they can be imported as leases like the other generated inputs.

The second step is the next increment: a graph adapter declaring the two per-slot buffers (the producer as
a discard write, the lighting pass as a read), the earlier eligibility freeze, and the runtime wiring.

The graph-side half of that step is now in: a new `graph::cluster` adapter declares the producer's two
buffers - `counts` as a read-write access because the dispatch atomically increments it over the table the
frame-slot reset host-zeroes, `indices` as a discard write because it is written wholesale - and
`lighting::cluster_inputs` declares the consumer reads. `test_render_graph_lighting` composes the producer
and the consumer and asserts what the migration is FOR: a memory dependency on both buffers before the
lighting pass, and the sort earlier in the execution order (36 checks, 0 failed). Nothing calls the
adapter from the runtime yet, so no rendering behaviour changed (deferred and sponza keep their hashes);
the runtime wiring is the next increment.

The first runtime half followed: the eligibility predicate moved into `main_graph_candidate()` and is now
frozen at the TOP of `record_main_drawcalls()`, before the cluster sort's owner is decided - `record_scene()`
and the fallback log read the frozen value instead of recomputing it. The gate round after the move is
byte-identical for all fourteen scenarios (matched 14, mismatched 0), validation-clean, and the
per-scenario graph/legacy inventory is unchanged - so the earlier freeze changed the ordering of the
decision, not its value.

The cluster producer is now MIGRATED on graph frames. `record_post_graph` imports the two per-slot buffers
(`counts` with the compute read-write prior state the last frame left, `indices` undefined), appends the
sort as graph lane 8 through the `graph::cluster` adapter, and records it with the barrier-free
`record_content` entry; the legacy stage is skipped on those frames (`graph_cluster_frame`). The gate round
after the wiring is byte-identical for all fourteen scenarios with zero validation findings - so the graph
owns the sort's ordering against the lighting pass that reads the bins, and the pass's two hand-issued
barriers now belong to legacy frames only.

One thing the suite taught while wiring it: `cluster_resources_available()` must require the ACTIVE chain's
cluster pass and this slot's buffer objects, not merely a non-null snapshot entry - the first version
looked up the ids only, and `test_render_graph_post` (whose stub chain has a cluster-ish snapshot but no
active cluster stage) failed the frame instead of falling back. With the stricter check a frame that
cannot record the lane keeps the PRE-MIGRATION split (legacy stage records outside the graph, the graph
does not declare the reads), which is exactly the behaviour a frame without the managed cluster resources
needs - the same graceful-degradation rule the fallback log documents.

## Stage 2H: the cascaded shadow map is declared (consumer half); acceleration structures are not a graph kind

The lighting pass's shadow reads were invisible to the graph. They are declared now: `lighting::shadow_cascades`
is a layered depth array (one image per frame slot, one layer per active cascade) imported from the
`shadow_map` snapshot with the SAMPLED prior state - the renderer's hand-back barrier already makes every
allocated layer sampled before the graph records, so this is knowledge, not a barrier. A frame without an
allocated shadow map keeps the pre-migration split (nothing declared).

Evidence: `test_render_graph_lighting` now asserts the light pass's 11 accesses (five stage + two shared
uniforms + three IBL images + the cascades) and the declared texture in the compiled parameters - 37
checks, 0 failed. CTest 44/44. The gate round is byte-identical for all fourteen scenarios (matched 14,
mismatched 0) with zero validation findings.

Two follow-ups are recorded rather than implemented:

- the shadow pass's PRODUCER migration needs the same split the cluster sort got: the pass owns its
  per-layer depth-attachment transitions, but the runtime owns the hand-back barrier over every allocated
  layer, so moving the pass in means the graph owns both - a `record_content`-style refactor plus a lane
  and the hand-back's replacement by declarations;
- **acceleration structures are not a graph resource kind at all**: `resource_kind` is `{texture, buffer}`,
  so a TLAS cannot be declared today. The lighting pass does not read the TLAS directly (it reads the RT
  visibility image, which is the Stage 2B item); the TLAS consumers are the ray passes, none of which are
  in the graph yet. Taking it in needs a new core resource kind plus usage/barrier lowering for
  acceleration-structure reads at ray-tracing stages - a core feature, not a declaration.

The producer's enabling half is in: `shadow_pass::record_content()` records the cascades with none of the
pass's own ordering (the per-layer `undefined -> depth_attachment` transitions are skipped; the legacy
`record()` keeps them), which is what a graph-recorded frame needs - the graph declares the shadow map as a
written graph resource and emits both those transitions and the hand-back the runtime issues for legacy
frames. The legacy path is unchanged: CTest 44/44 and the deferred/sponza gate hashes are unchanged.

The next step's boundary is measured rather than guessed: the graph adapter must declare the ACTIVE cascade
layers only (the pass writes `min(cascades, targets)` layers, and the lighting read has to cover exactly the
same range or the graph's content validation refuses the plan), so the adapter takes the layer count as an
input; and the runtime must skip its hand-back barrier on graph frames, where the graph's lighting barrier
replaces it.

## What stage 1 changes about the plan

1. Stage 2's first task was to explain the mid-run flip, and it is done: the term that refuses the
   frame is now named in the log, the mechanism is the renderer's own one-frame toggle lag (frame 0
   runs without the scenario's features, so the graph covering it is correct), and the attempted
   first-frame exclusion was rejected by `test_render_graph_post`. What remains for stage 2 is the
   coverage rule below, not a fix.
2. The declaration work (material textures/IBL, per-mesh addresses, meshlet tables with
   atomic writes, indirect commands) stays necessary - the 7 graph-only scenarios cover
   deferred, SSAO-off, shadow-single, sponza, unlit, glossy motion and the material sweep,
   and their accesses are still declared only through the shared read-only bundle.
3. The scenario gate now has a per-scenario path inventory as evidence, which the next
   stages can diff against: a stage is done when its scenario moves to "graph only" for the whole
   run AND stays byte-identical - and the inventory, not the hash file, is what says whether the
   graph was on the path at all.
