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

The shadow pass is now MIGRATED on stale-map frames. `graph_shadow_frame` is frozen with the eligibility
and includes the pass's own reuse test (a reused map is not written, so the graph must not declare its
producer); when it holds, `record_post_graph` imports the map with UNDEFINED prior contents (the first
frame's image really is in UNDEFINED layout, and the producer's write is allowed to discard it), appends
the pass as lane 9 through `graph::shadow`, and records it with the barrier-free `record_content` entry;
the legacy stage and the runtime's hand-back barrier are skipped, because the graph now emits both the
per-layer transitions and the read transition the lighting pass needs. A frame that does not record the
pass keeps the Stage 2H split (sampled import + declared reads).

Two measurements shaped it, and both are recorded as limitations rather than worked around silently:

- a LAYER-RANGED write declaration (the active cascade run) makes the graph's `prepare()` refuse the frame
  with `unsupported` - the transition never reaches the backend, so the adapter declares the whole image.
  That is safe (a declaration is an ordering promise, not an operation: no discard is recorded, the spare
  layers keep their previous contents, and nothing samples them) but imprecise; making the range exact is
  a follow-up for the execution/backends path;
- the import's prior CONTENTS matter: with the sampled prior state of Stage 2H, the first frame's image is
  still UNDEFINED natively and the submit is rejected with a layout mismatch - the graph-recorded frame
  needs `initial_contents::undefined`, which is exactly what the pass's legacy per-layer barrier spelled.

Evidence: CTest 44/44; the gate round with the graph on is byte-identical for all fourteen scenarios
(matched 14, mismatched 0) with zero validation findings, and `test_render_graph_lighting` asserts the
producer/consumer ordering for the shadow image (42 checks, 0 failed).

## Stage 2I: TAA needs two graph passes, and the split is in

The TAA resolve is the next pass to migrate, and the first eligibility exclusion to remove. Measured: it
CANNOT be declared as ONE graph pass. The resolve writes the HDR target as a colour attachment and then
copies that same image into the history image as a transfer source, so a single declaration would hold two
accesses to one resource with different scopes - which the graph's model refuses (one access per resource
per pass; anything else is `conflicting_access`). The migration therefore records TWO graph passes from the
one pass object: the resolve and the history copy.

The enabling half is in: `taa_pass::record_content()` (the resolve, no barriers) and `taa_pass::record_copy()`
(the HDR -> history copy, no barriers), while the legacy `record()` keeps its attachment/input barriers and
its transfer round trip - the same split the cluster sort, the shadow pass, the deferred pass and the bloom
chain use, so one submission never runs both barrier paths. The legacy path is unchanged: CTest 44/44, and
both the plain `deferred` scenario and the TAA scenario (`deferred_taa_fxaa`) keep their gate hashes.

Remaining for the item: the two graph adapters, the wiring (lanes, plus the scene-colour/history/velocity
declarations), and removing `taa_active()` from the eligibility predicate.

The graph side of that is now in: `graph::taa` exposes `append_resolve` (scene colour, velocity, depth and
history as sampled reads, the HDR target as the attachment write) and `append_history_copy` (the HDR target
as a transfer read, the history image as a transfer write), and `test_render_graph_lighting` composes the
two: 5 and 2 accesses, a memory dependency on the HDR target before the copy, and the resolve earlier than
the copy in the execution order (50 checks, 0 failed; CTest 44/44). Nothing calls the adapters from the
runtime yet, so no rendering behaviour changed.

Remaining after that: the runtime wiring and removing `taa_active()` from the eligibility predicate.

The wiring is in, and it is deliberately DORMANT: `main_graph_frame` still excludes TAA, so `graph_taa_frame`
(`main_graph_frame && taa_active() && the pass resolved`) can never be true yet. What the wiring does:

- the geometry and lighting passes write the TAA WORKING image (output-target family 5, the "scene colour")
  on a TAA frame and the HDR image (family 0) otherwise - one `scene_target` choice threads through both
  appends and their target assertions;
- the history (family 7) is imported with defined, sampled contents and, after the graph records, the runtime
  hands it back from transfer-destination to sampled - because the state the graph's copy leaves OUTLIVES the
  plan, and a state that outlives the plan is exactly what the graph cannot own (the legacy path's hand-back
  has the same reason);
- two lanes record the split pass: `payload->record(10)` calls the pass's `record_content` (resolve) and lane
  11 calls `record_copy` (history copy) through the same `taa_pass` object.

Nothing changed behaviourally: CTest 44/44, and `deferred`/`deferred_taa_fxaa` keep their gate hashes
(`deferred_taa_fxaa` still falls back with "TAA active", which is the expected dormant state).

The next step - the one that makes it live - is removing `taa_active()` from `main_graph_candidate()` and
gating on the real thing.

That flip was attempted, bisected, and then COMPLETED - the bisection is the whole answer to "how far it got",
and the two defects it isolated are fixed:

- 1 frame: byte-identical between the paths;
- 2 frames: byte-identical;
- 3 frames: DIFFERENT - and with the harness's image rotation, frame 3 is the first frame whose per-image
  HISTORY is valid (frame 1 writes image 0's history through the copy, frame 2 image 1's, frame 3 reuses
  image 0).

So the resolve, the working-image switch, the history copy, the runtime hand-back and the collected result
were all correct WITHIN a frame, and the defect was in the cross-frame history chain. Two causes, each fixed:

1. **The history copy was CULLED.** The copy pass's only output is the history image, which no pass in the same
   frame reads, so the value-root culling dropped the pass - the history was never written and the resolve
   behaved as if every frame were a first frame. Evidence: ignoring the history on both paths made them
   identical; the copy's recorder diagnostic never printed. Fix: the history image is exported, so the copy
   pass stays in the plan (next frame's resolve is its consumer).
2. **The history's first use has no layout.** The graph can only import "defined contents in a KNOWN layout",
   and an image no path has transitioned is still UNDEFINED - sampling it was a validation error (measured:
   `expects VK_IMAGE_LAYOUT_GENERAL ... current layout is VK_IMAGE_LAYOUT_UNDEFINED`). Fix: the runtime issues
   that one transition on the history's first use, remembers it per image (reset with the generation), and a
   legacy TAA frame records the same fact - keeping the "history is SAMPLED whenever the graph starts"
   invariant the hand-back states.

With both fixed the flip is IN: the gate round is byte-identical for all fourteen scenarios with zero
validation findings, and `deferred_taa_fxaa` records through the graph instead of falling back. The TAA item is
complete: its pass is recorded by the graph, its two accesses are two graph passes, and the only runtime-owned
pieces left are the frame-boundary states that outlive the graph's plan.

## Stage 2J: the G-buffer debug view - enabling split in

The debug view is the next pass to migrate (it is also what makes the `deformation` scenario fall back with
"deferred lighting inactive": the debug view wins over the lighting stage, and the eligibility predicate
requires the lighting stage). Measured shape: the pass reads the three stored G-buffer targets plus the motion
vectors as four sampled inputs, opens ONE CLEAR+STORE instance over the HDR target, binds the channel's
pipeline, pushes the channel/projection/motion lanes and draws the fullscreen triangle. The HDR target's
transition is the FRAME LOOP's, not the pass's.

The enabling split is in: `gbuffer_debug_pass::record_content()` records the instance, push and draw alone,
while the legacy `record()` keeps its four `color_attachment -> shader_read` input barriers and then calls the
content entry - the same split the cluster sort, the shadow pass, the TAA resolve (and its copy), the deferred
pass and the bloom chain use. The legacy path is unchanged: CTest 44/44 and the `deformation` scenario keeps
its gate hash.

Remaining for the item: a `graph::gbuffer_debug` adapter (the four sampled reads + the HDR write), the runtime
lane and the eligibility change (the graph must accept a debug frame by requiring the G-BUFFER pass instead of
the lighting stage, and record the debug view INSTEAD of the lighting lane), plus the graph ownership of the
HDR target transition the frame loop issues today.

The debug view is now MIGRATED. `graph::gbuffer_debug` declares the four sampled reads and the HDR-target
CLEAR write; `record_post_graph` records it as lane 12 INSTEAD of the lighting lane when the frame's debug view
is active; and `main_graph_candidate()` now asks for the G-BUFFER pass (`gbuffer_pass_active()`) instead of the
deferred lighting stage, with `graph_debug_frame` deciding which producer the graph records. The HDR target
transition the frame loop issues for the view belongs to legacy frames only now - on a graph frame the graph's
own declaration emits it.

Evidence: CTest 44/44; the gate round is byte-identical for all fourteen scenarios (matched 14, mismatched 0)
with zero validation findings; the `deformation` scenario - the debug-view scenario - records through the graph
with its hash unchanged (`723569BA0D03640C`), and the pure-graph scenario count is now 9 (the seven before plus
the TAA scenario and `deformation`).

## Stage 2K: the transparent pass - enabling split in

The alpha-blended geometry is the next pass (the `transparent_blend` scenario is the last single-scenario
fallback). Measured shape: the pass LOADs the shaded scene target and the opaque depth, records the blended
leaves into a per-slot secondary and executes it inside one instance; it owns TWO hand-off barriers at the entry
(depth back to an attachment for the depth test, and the scene target's attachment-to-attachment dependency) and
ONE depth hand-back at the exit (two later stages sample that depth: the TAA disocclusion guard and the
composite's edge test).

The enabling split is in: `transparent_pass::record_content()` records the secondary, the LOAD instance and the
execute alone, while the legacy `record()` keeps the entry barriers, calls the content entry, and issues the
depth hand-back - the same split every migrated pass uses. The legacy path is unchanged: CTest 44/44 and the
`transparent_blend` scenario keeps its gate hash.

Remaining for the item: a `graph::transparent` adapter (the scene target as a preserve-write colour attachment,
the depth as a depth-attachment read), the runtime lane, and removing `features.transparent` from the
eligibility - the graph then orders lighting -> transparent -> TAA by the same resource declarations the
hand-off barriers used to publish.

The transparent pass is now MIGRATED, and the depth's graph access turned out to be the one non-obvious choice:
the pass DEPTH-TESTS against the opaque depth, so the graph must see a READ - and `valid_scope` accepts
`depth_stencil_attachment` only for writes, so the honest declaration is `depth_stencil_read` (a read-only depth
attachment - natively GENERAL under the same unified policy the legacy hand-off uses, so the barrier is
equivalent). `graph::transparent` declares exactly that plus the working image as a preserve-write colour
attachment; `record_post_graph` records it as lane 13 after the lighting (or debug) producer; and the
eligibility no longer excludes `features.transparent`.

Evidence: CTest 44/44 (the lighting graph test now also covers the transparent adapter: two accesses, the
producer-before-transparent ordering and the depth transition ahead of the pass); the gate round is
byte-identical for all fourteen scenarios (matched 14, mismatched 0) with zero validation findings; the
`transparent_blend` scenario records through the graph with its hash unchanged (`4D3FD6ACB2953BE4`), and the
pure-graph scenario count is now 10. The only fallback group left is the four laevatain (character/rim) scenarios
- the three character passes.

## Stage 2L: the three character passes - enabling splits in

The last fallback group is the four laevatain scenarios, and their chain is three passes: the character-forward
pass (the character's own leaves, its 描边 outline group and its overlay masks, drawn INTO the frame with LOAD
on the scene target and the depth), the toon screen rim and the goo rim (fullscreen additive effects). Measured
shape:

- `character_forward` owns two entry hand-offs (depth back to an attachment for the depth test, the scene
  target's instance dependency) plus the exit depth hand-back - exactly the transparent pass's pattern;
- the two rim passes own NO barriers: their sampled inputs (the depth and the albedo) are per-swapchain-image
  HEAP SLOTS the shader indexes with its push-constant image index, and the legacy path relies on those images
  already being in a sampled layout. That is the out-of-model binding pattern again (like the material texture
  set): the graph cannot see the heap read, but it CAN declare the access, and declaring it is what makes the
  graph transition and order what the record only indexes.

The enabling splits are in: `character_forward_pass::record_content()` records the LOAD instance and the three
draw groups alone, with the legacy `record()` keeping its two entry barriers, the content call and the depth
hand-back; the two rim passes expose `record_content()` that refuses what the runner would have refused and then
runs their (barrier-free) record body - for them the legacy body IS the content, and the declaration exists so
the graph recorder has one uniform entry. The legacy path is unchanged: CTest 44/44 and all four laevatain
scenarios keep their gate hashes.

Remaining for the item: three adapters (the scene target as a preserve-write colour attachment for all three,
plus the depth read for character_forward and the declared sampled reads of depth/albedo for the rims), the
runtime lanes in the legacy order (transparent -> character_forward -> toon rim -> goo rim -> TAA), and removing
the three feature exclusions.

The three passes are now MIGRATED as one module (`graph::character_forward`, `graph::toon_screen_rim`,
`graph::goo_rim`): all three declare the scene target as a preserve-write colour attachment; the forward pass
declares the depth as a depth-stencil read (the depth test), and the two rims declare the depth and albedo reads
their shaders take through per-image heap slots - the reads are real, only the binding is out of the model, and
declaring them is what makes the graph transition and order them. `record_post_graph` records them as lanes
14/15/16 in the legacy order (transparent -> character forward -> toon rim -> goo rim -> TAA), gated on the same
feature answers the legacy stages use.

One thing the enabling of the lanes needed and the test found: THE CANDIDATE MUST REQUIRE THE OVERLAY'S PASS.
`test_render_graph_post`'s modes 1/2 wire a rim feature active WITHOUT the pass in the chain, and the graph then
refused at build time and failed the frame instead of falling back. The predicate now asks the same question the
legacy stages ask - an active overlay with no pass in the chain is not a graph candidate, so the frame takes the
legacy path (which gates that feature on its pass).

Evidence: CTest 44/44 (test_render_graph_post included - its fallback modes are what caught the missing guard);
the gate round is byte-identical for all fourteen scenarios (matched 14, mismatched 0) with zero validation
findings, and ALL FOURTEEN scenarios now record through the graph - the four laevatain scenarios are the ones
this stage moved, and the pure-graph count is 14 of 14.

## Stage 2M: the default flips - the composed graph is the frame now

`[render] main_render_graph` now DEFAULTS TO TRUE, and the flip is validated by the gate round that does NOT
inject the key: all fourteen scenarios record through the graph and are byte-identical to the legacy reference
(matched 14, mismatched 0) with zero validation findings across the fourteen logs. `test_app_config` asserted
the old default and now asserts the new one - that is the CPU-side statement of the flip.

One honest caveat from the first flip round: `laevatain_old_chain` mismatched its frozen reference once. It is
an animated scenario, and a standalone rerun plus the next full round both reproduce the reference hash
(`84A41141128286CE`) exactly; the round that differed was a timing-shifted capture of the same code (the review's
known "the frozen baseline is stale by design, the same-session two-round compare is the verdict" caveat).

What the default means for the remaining work: the legacy scene tail is now the FALLBACK, not the main path. It
stays for exactly the three things the graph cannot express yet - the ray-traced shadows and MegaLights (their
traces read the TLAS, and the graph has no acceleration-structure resource kind) and the Gaussian splats - and
per-segment retirement is what removes each of those one at a time.

## Stage 2N: the Gaussian splats - enabling split in, the flip waits for a splat asset

The Gaussian chain is ONE pass object with two halves: a GPU compute chain (three dispatches - reset, prepare,
finalize - recorded by the frame snapshot's `record_preparation`, which also emits their buffer barriers) and the
raster (depth test against the opaque depth, alpha blending into the working image, indirect draw from the
snapshot's own command buffer). Its vertex data reaches the shader as BDAs over primitive-owned buffers - the
out-of-model per-draw pattern, already recorded.

The split: `record_impl(io, own_barriers)` is the one body; the legacy `record()` passes `true` (it keeps the two
image hand-offs at the entry - depth back to an attachment, the target's instance dependency - and the depth
hand-back at the exit) and the graph entry `record_content()` passes `false`, so a graph-recorded frame declares
the pass-level boundary (a preserve-write target and a depth read) and the graph emits it. The GPU compute chain
calls its own buffer barriers from inside the same recorder: those buffers are the frame snapshot's private
ones, no other pass touches them, and the graph's boundary is the raster, exactly as it does not see the scene
pass's per-draw state.

The test that pins the legacy command sequence (`test_gaussian_raster`: `barriers == 0` on a push refusal,
`barriers == 2` on a failed begin, three transitions on the success path) caught the first split - a wrapper that
ran the hand-offs before the validation recorded two barriers where the shipped order records none. The flag
form restores the order exactly, and CTest is 44/44.

What is NOT done, and why: a `.ply` Gaussian cloud is the only way to turn the feature on in the app, and the
repository carries no such asset - so no gate scenario can exercise the path, and the migration's gate criterion
(byte-identical vs legacy with the graph on) cannot be met here. The flip of the gaussian exclusion waits for a
splat fixture; everything below it (the adapter shape and the lane) is a mechanical repeat of the transparent
pass's.

## Stage 2O: the Gaussian splats migrate - and the net grows the scenario that can see them

The blocker stage 2N recorded was not the pass, it was the NET: a `.ply` model path is the only way to turn the
gaussian feature on, and the repository carried no such asset, so no scenario could exercise the chain. The fix
is a committed ASCII fixture (`source/tests/fixtures/gaussian_ribbon.ply`, 160 splats on a tilted ribbon, the
same 14-float layout the loader's own test documents) and a fifteenth scenario, `gaussian_splat`, which the
script marks `tier = "extra"` and whose description says what it is for. Its reference hash was seeded from the
LEGACY path (the exclusion was still on, so the run logged "unsupported tail (Gaussian active)") - that makes
the reference the legacy frame by construction.

Then the migration proper: `graph::gaussian` declares the working image as a preserve-write colour attachment and
the opaque depth as a depth-stencil read; `record_post_graph` records it as lane 17, BEFORE the transparent lane,
because the legacy tail runs the gaussian stage before the transparent pass (lighting -> gaussian -> transparent
-> character). The GPU compute chain keeps its own buffer barriers inside the recorder (the snapshot's private
buffers - no other pass touches them), so the graph's boundary is the raster. `gaussian_active()` leaves the
eligibility predicate, and the pass-availability guard family grows nothing new because the gaussian lane is
gated on the same answer the legacy stage uses.

Evidence: CTest 44/44 (the lighting graph test also composes the gaussian adapter now: two accesses, the
producer-before-raster ordering and the depth transition); the gate round is byte-identical for all FIFTEEN
scenarios (matched 15, mismatched 0) with zero validation findings, and all fifteen record through the graph.
The gaussian scenario's hash is the same `8555C8295938D118` the legacy capture seeded.

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
