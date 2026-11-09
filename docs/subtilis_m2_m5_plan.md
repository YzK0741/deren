# Subtilis M2/M5 implementation plan

Status: M2 v1 is implemented in this branch and is off by default
(`[render] subtilis_gpu_cut`). M5a (offline page table), M5b (synchronous page
residency manager), and M5c (cut requests, budgeted installs, raster page-id fetch,
LOD0 fallback) are implemented on this branch; the local Release build, full CTest,
boundary, format, and changed-file tidy checks pass. M5c publishes table state on the frame
after a request; it does not yet perform a device-to-device page copy or eviction. The
v1 shader keeps one persistent
workgroup per item (64 lanes, word-held coarsen bitset, iterated closure, contiguous
chunk compaction); a global cross-item node/cluster queue is deliberately deferred
until the flat group table gains explicit child links. `test_subtilis_cut_gpu` exercises
the compute shader on a real device and compares its output with the CPU `select_cut`
oracle.

M2 is GPU cut selection/finalize. M5 is geometry page streaming. This document

- records the current CPU baseline and the exact GPU data path we would add;
- extracts the parts of UE 5.8 Nanite worth copying, and the parts we should not copy;
- stages the work into separate, independently testable PRs;
- calls out the places where a naive "move it to the GPU" would be wrong for Deren.

The order is M2 first, then M5. They are separate PRs. M2 makes the cut result GPU-resident;
M5 makes the input to that cut pageable.

## 1. Current baseline

### 1.1 CPU cut

`select_cut` in `source/engine/geometry/subtilis_dag_lod.cppm:49` walks the DAG groups and
clusters and writes selected 48-byte meshlet records into a shared table.

- The per-group LOD metric is
  `group.error * scale * pixels`, with perspective divided by the conservative closest depth.
- `scale = sqrt(norm1 * normInf)` of `view * model`; `pixels` is derived from the projection
  scale and the viewport.
- Near-plane groups are forced to their finest cluster, unsupported projections force finest.
- A hysteresis threshold of `0.8` applies when the previous decision was not coarsened.
- `close_coarsening` closes producer/consumer dependencies before clusters are emitted.
- A cluster is emitted when its producer is absent or coarsened and its consumer is absent or
  not coarsened.
- Capacity overflow returns `cut_error::capacity`.

`prepare_item` in `source/engine/geometry/subtilis_selection.cppm:101` gates the cut with the
geometry generation and the `coarse_allowed` restrictions: 80-byte vertex stride, normal at
offset 12, UV at offset 24, no transparent/overlay/named pipeline, no skin/morph, and the
visibility flags. It publishes the selected run through a callback, or records a complete
LOD0 fallback.

The CPU cut is parallel and costs roughly 2 ms/frame on the Bistro scene. The frame is
GPU-bound, so M2 is primarily an architecture and CPU-headroom change, not a guaranteed FPS
win.

### 1.2 Frame publication

`record_visibility_items` in `source/engine/runtime/runtime.cpp:2183` builds `plan.block` (a
104-byte push block) and `plan.row` per item. `apply_meshlet_run` in
`source/engine/visibility/visibility.cppm:259` sets the row's meshlet base and table source,
and sets `block.geometry.first_index = run.base`, `index_count = run.count`, `base_vertex = 1`.

The per-frame path is host-visible:

- `meshlet_culled_mapped` is a host-visible meshlet table.
- `mesh_indirect_mapped` is a host-visible indirect command array. The CPU writes
  `groups_x = plan.block.geometry.index_count` at `runtime.cpp:3164`, then records
  `draw_mesh_tasks_indirect`.
- HZB candidate count is written by the CPU as `cut_visible ? plan.run.count : 0` at
  `runtime.cpp:3143-3146`.

The mesh shader does not read the group count from the push block. `visibility.slang:94`
consumes `SV_GroupID` and the indirect command supplies the workgroup count. Therefore a GPU
cut can change the number of dispatched meshlets without changing the push block, as long as
the indirect command is GPU-written and the meshlet table lane is GPU-written consistently.

Limits: 4096 items, 4096 selected meshlets per item, 65536 records in the shared table.

### 1.3 Why M2 and M5 are separate

M2 changes *who computes the cut*. M5 changes *what has to be resident before the cut can run*.
Trying to do both at once would mix two failure modes: cut parity and page residency. The
staged order keeps A/B comparisons meaningful.

## 2. UE reference: what to copy, what not to copy

### 2.1 GPU cut

UE's Nanite cull is a persistent-thread hierarchy traversal. `ProcessNodeBatch` in
`Engine\Shaders\Private\Nanite\NaniteHierarchyTraversal.ush:69` expands nodes, filters them
with `ShouldVisitChild`, writes non-leaf nodes to a candidate node buffer and leaf clusters to
a candidate cluster buffer. `PersistentNodeAndClusterCull` at `:251` keeps node/cluster queue
read and write offsets so a dispatch can process the queue until it is empty.
`NodeCull`/`ClusterCull` at `:372`/`:417` are the non-persistent variants.

`FNodeAndClusterCull_CS` at
`Engine\Source\Runtime\Renderer\Private\Nanite\NaniteCullRaster.cpp:939` is the pass that
consumes the candidate buffers and produces the visible cluster stream and the streaming
requests. Its shared parameter block at `:907` includes `ClusterPageData`, `HierarchyBuffer`,
`CandidateNodes`, `CandidateClusters`, `ClusterBatches`, `OutVisibleClustersSWHW`, and
`OutStreamingRequests`. Raster indirect args and bins are built around `:4201` and `:4515`;
main/post HZB culling is at `:4500`.

The transferable ideas are:

1. A cut pass writes a compact visible cluster stream plus indirect draw arguments; the CPU
   does not read the result back.
2. Work is queued explicitly (node queue, cluster queue), so a variable-size hierarchy does
   not require a fixed one-thread-per-node dispatch.
3. The cut pass is also where streaming requests are produced, because it knows which pages
   the selected clusters need.

The parts not worth copying for Deren v1: Nanite's material/visibility complexity, virtual
shadow maps, and its full page-table indirection. Deren has a much smaller scene budget and a
separate visibility-buffer path already.

### 2.2 Streaming

`FPageKey {RuntimeResourceID, PageIndex}` and `FStreamingRequest {Key, Priority}` are at
`Engine\Source\Runtime\Engine\Public\Rendering\NaniteStreamingManager.h:28-63`. The manager
holds the cluster page data and hierarchy heaps, the registered/resident page sets, page
dependencies, an LRU, and a priority heap. Key APIs are `GetStreamingRequestsBuffer`,
`GetClusterPageDataSRV`, and `GetHierarchySRV`.

`NaniteStreaming.ush:19` `RequestPageRange` writes requests with a packed priority. The manager
installs at most 128 pages per frame (`NaniteStreamingManager.cpp:90`) and selects pages with
`SelectHighestPriorityPagesAndUpdateLRU` at `:2731`; parent/dependency requests and fixups are
at `:2579` and `:2731+`. Root pages are always resident. A hierarchy node whose page is not
resident is not loaded, so the traversal requests the page instead of drawing a hole.

The transferable ideas are:

1. Page identity is stable and independent of the frame.
2. Root/ancestor pages are always resident; children are requested on demand.
3. Missing pages degrade to a coarser complete representation, never to missing triangles.
4. The GPU cut produces requests; the CPU/page manager installs pages and publishes a page
   table; the next frame's cut can use them.

## 3. M2 design: GPU cut selection and finalize

### 3.1 Scope

M2 v1 is enabled only for items that already have a DAG payload and pass the same
`coarse_allowed` gate as the CPU path. Everything else keeps the CPU LOD0 fallback.

Default: off. Enable with a render config flag (working name `subtilis_gpu_cut`) until parity
and performance are proven. This keeps the merged path unchanged for users.

### 3.2 GPU data

Add a GPU cut payload to `uploaded_geometry` (or a sibling object held by the same geometry
lifetime):

- `cut_group`: one record per DAG group: LOD center/radius, error, first child/count, first
  cluster/count, producer/consumer ids or flags. Exact layout follows the CPU DAG after the
  payload is finalized.
- `cut_cluster`: one 48-byte meshlet record per DAG cluster, plus producer/consumer ids. The page id
  reuses the record's four-byte padding lane at offset 12, so the 48-byte ABI does not change.
- `cut_page`: the page table and arena are separate scene descriptors, not a per-cluster parallel
  array. A record's page id is `page_index + 1`, with 0 meaning the draw's index buffer.
- `cut_item`: per-frame item input: model matrix, cut BDA pointers, reservation base/capacity,
  flags, and the page table/arena/request pointers + counts.
- `cut_scratch`: per-item group coarsen flags and a selected-cluster bitmask/count.
- `cut_indirect`: device-local mesh-task indirect arguments, one command per item.

The payload is built once when the DAG is imported/uploaded and is immutable afterward. It is
not rebuilt per frame.

### 3.3 Dispatch shape

Use one persistent workgroup per item (items are capped at 4096), with 64 lanes. This follows
UE's persistent-thread culling shape while keeping the cut in one dispatch:

1. Each lane holds one 32-bit word of the per-item coarsening bitset (64 words cover the
   2048-group host limit). Direct decisions are written as one masked word per lane, so groups
   sharing a word cannot lose each other's updates.
2. The dependency closure is iterated in group-barrier passes until no decision changes. Each
   lane computes its next word from the previous stable snapshot, then all lanes publish after a
   barrier; decisions only clear, so the passes converge to the CPU's ascending-id result.
3. Cluster emission is a contiguous-chunk compaction: each lane counts its chunk, lane 0
   prefix-sums the 64 counts in chunk order, then each lane writes its selected clusters at its
   prefix. Chunks are contiguous cluster-id ranges, so stable cluster order is preserved.
4. The workgroup writes the item's selected count and the mesh-task indirect command
   (`groups_x`). No CPU readback.

A global persistent-thread queue is still the escape hatch if item-level imbalance appears; the
current item counts do not justify the extra queue buffers and indirect dispatch.

The existing mesh shader reads records by `geometry.first_index + group_id.x`, so the GPU pass
must write the compacted records at exactly that run base. It must also write the same records
that the HZB/visibility consumers expect.

### 3.4 Indirect command and fallback

Today the CPU writes every item's command. M2 changes this to:

- CPU writes commands for non-DAG, unsupported, and fallback items before the cut pass.
- GPU writes commands for GPU-cut items and leaves fallback commands untouched.
- The command buffer is used as a mesh-task indirect buffer and is written by a shader, so it
  needs the backend's shader-write and indirect usage. If the backend cannot expose a
  host-visible buffer with both usages, use a device-local indirect buffer plus a small staging
  upload for the CPU fallback commands, and a barrier before the raster pass.

A `vkCmdDrawMeshTasksIndirect`/equivalent cannot be issued until the compute writes are
visible. The plan adds an explicit buffer barrier between `cut_finalize` and the visibility
raster pass, and does not rely on submission-order luck.

### 3.5 HZB interaction

`runtime.cpp:3143-3146` currently writes the HZB candidate count from the CPU. M2 v1 will run
with HZB candidate culling disabled to isolate cut parity. M2b then moves the count to a GPU
buffer and makes the HZB item record read it.

This is not optional for shipping: without it, a GPU cut that selects fewer meshlets than the
reservation would make HZB scan stale records. The intermediate "use reservation count" would
read uninitialized table entries and is explicitly rejected.

### 3.6 Parity and acceptance

A/B against `select_cut` with the same camera and scene:

- Selected group flags per item.
- Selected cluster ids, in order, and per-item counts.
- Triangle count and index/vertex windows.
- Rendered pixels, depth, and visibility ids on a capture frame.

Additional tests:

- Capacity overflow, near-plane, unsupported projection, non-affine transform.
- Missing/partial DAG payload and device without compute/indirect support -> CPU fallback.
- Repeated frames produce identical selected order (determinism).
- HZB off and HZB on both produce the same pixels as CPU.
- No host read of the cut result in the hot path; assert by API and by frame timing.

Success criteria: cut correctness identical to CPU; CPU cut time removed from the frame (or
reduced to a fallback-only path); GPU pass time measured and reported. FPS improvement is a
bonus, not the acceptance gate.

## 4. M5 design: geometry page streaming

### 4.1 Page unit and scope

Proposed page: 64 KiB, keyed by `(asset_id, page_index)`, with `asset_id` derived from the
content hash plus the profile.

M5 v1 pages DAG hierarchy metadata, cluster records, and the **coarse index payload**. LOD0 and
the shared vertex buffer remain fully resident; they are the fallback representation. This
validates page identity, residency, dependency, LRU, request, and fallback without changing
vertex fetch semantics.

M5 v2 (separate, larger change) adds cluster-local vertex pages. Deren's current coarse
clusters share the LOD0 vertex buffer and append indices into one index buffer, so vertex
paging requires an offline reindex/duplication pass and a new asset profile. That is not an
ABI-only change and must not be smuggled into M5 v1.

Until v2, the memory win is limited to hierarchy/cluster/index bytes. That is still enough to
validate the streaming architecture, but it should be stated honestly in the PR.

### 4.2 Asset format

The offline builder emits:

- `page_table`: one entry per page: byte range, type, dependency page ids, priority hint.
- `resident page payload`: hierarchy/cluster records and coarse indices, aligned to the page
  size.
- `root pages`: the hierarchy roots and the metadata needed to start traversal, always
  resident.
- `lod0 fallback`: unchanged complete LOD0 data, always resident.

Page boundaries must split at cluster boundaries. A cluster's index window must not span two
pages; otherwise index fetch would need a page-table lookup per index.

#### 4.2.1 Wire format (M5a)

Payloads without pages keep the version-1.0 layout: 7 sections and a 352-byte directory.
A payload with pages is version 1.1: 9 sections and a 416-byte directory. The two new
sections are `PAGES` (type 8, 32-byte entries) and `PAGE_DEPS` (type 9, 4-byte page ids).

Each `PAGES` entry is:

```
u32 section;          // page_section: indices..roots
u32 first;            // first element in that section
u32 count;            // element count, never zero
u32 bytes;            // count * section stride, at most 64 KiB
u16 flags;            // bit 0: always resident
u16 priority;
u32 dependency_first; // index into PAGE_DEPS
u32 dependency_count;
u32 reserved;         // zero
```

`PAGE_DEPS` is a sorted, duplicate-free slice per page. Edges point from the consuming page
to the page that stores the element (`groups -> references -> clusters -> indices`,
`leaves/roots -> clusters`), so the dependency graph is a DAG. A page's `bytes` must match
`count * stride` and fit in 64 KiB. Every section must be covered exactly once, in section
order, and every cluster's index window must sit inside one index page. Residency is a
closure: every dependency of an always-resident page must also be always-resident.
`compute_pages` derives the cover; `validate_pages` enforces these invariants on both the
builder output and decoded payloads. This milestone stores metadata only; the render path
keeps every page resident, and M5b adds a synchronous CPU manager over the table without
changing uploads.

### 4.3 Runtime residency

Add a CPU-side page manager:

- registered pages per asset, resident page slots in a global page arena;
- priority heap, LRU, per-frame install budget (start with a small fixed budget);
- dependency rules: root pages always resident; a child hierarchy page requests its parent;
- async transfer: request -> staging -> device copy -> fence -> publish page table entry;
- a page is not visible to the GPU until its copy is complete.

Add a GPU request buffer written by the cut pass. For M5 v1 the request is a page range plus a
priority; the manager coalesces ranges and sorts them before installation.

The cut shader reads a GPU page table. If a needed page is absent, the traversal marks the
group/cluster unavailable and requests it. The item must fall back to complete LOD0 for that
frame, not emit a partial set of clusters. Later, falling back to the nearest resident ancestor
page is allowed, but only if the ancestor is a complete conservative representation.

### 4.4 Geometry fetch with pages

This is the hard part and needs an explicit choice.

Option A (implemented for v1): keep one index buffer per item but make the selected cluster's
record refer to a resident page through the record's four-byte padding lane and a page table. The
48-byte record stays fixed; the `page_id` lane and a `page_base` table let the mesh shader compute
the index address. Every cluster window must be inside one page.

Option B (smaller first step): page at whole-item granularity. One page set per item; an item
is either fully resident or falls back to LOD0. This needs no shader change, but it does not
demonstrate fine-grained streaming.

The plan recommends A for the real M5 and B only as an intermediate test fixture. Option A
requires touching:

- the geometry lanes / mesh fetch for index base resolution;
- the visibility and shadow mesh entries that read the same records;
- the page table binding and the page-id side lane;
- the cut pass that records page requests.

If A proves too invasive, stop and re-scope M5 v1 to B rather than pretending B is A.

### 4.5 Staging

M5a: offline page format and page table, no runtime streaming. All pages resident; assert that
the render matches the pre-M5 path exactly.

M5b: synchronous residency manager, one asset, small budget, LRU, dependency requests. All
pages still loaded before first use to prove the page table. This is implemented as
`page_manager` in `source/engine/geometry/subtilis_residency.cppm`: the version-1.1 page
table is retained by `imported_asset`, `request_all` installs the whole table in dependency
order, requests install a complete dependency closure, and eviction only removes unpinned
pages that no remaining resident page depends on. The render path still uploads every page;
it does not consult eviction yet.

M5c: request-driven page install, budgeted installs, fallback to LOD0, and a page-miss metric.
This is the first milestone that can actually miss a page during rendering. The install is
`request -> next-frame table publish`; the arena already holds every page's bytes, so this slice
does not yet perform a device-to-device copy or wait on a transfer fence.

M5c-1 (the first slice) is implemented in
`source/engine/geometry/subtilis_page_streaming.cppm`: `page_arena_layout` assigns one 64-byte
aligned address range per page, `gpu_page_table_entry`/`gpu_page_request` are the byte-stable
shader ABI, `page_request_buffer` images the cut's bounded request append, and `page_streamer`
turns a queued request into a non-mutating `page_manager::plan_request` followed by an `apply`
on completion. The plan/apply split keeps a page invisible until its transfer fence: the resident
set changes only in `apply`. `pack_page_arena` copies encoded section records into that layout;
it does not reinterpret the engine's padded DAG structs.

The device part is implemented in `source/engine/geometry/subtilis_upload.cpp`: an imported
geometry generation now holds three additional buffers, `scene_page_arena`,
`scene_page_table`, and `scene_page_requests`. `uploaded_geometry` exposes them with
`page_arena()`, `page_table()`, `page_requests()`, `page_count()`, and
`page_request_capacity()`, plus `install_requested_pages(budget, frame)`. Upload leaves only
pinned pages resident; a cut index-page miss appends a 16-byte request, and the host promotes
requests under an 8 MiB per-frame budget before the next cut dispatch. Pages that do not fit stay
`requested` and are compacted forward; the request overflow flag is logged.

M5c-2 keeps the cut's group/reference/cluster reads on the existing full buffers: the 32-byte wire
cluster has no normal-cone axis/cosine, so the cut cannot build the 48-byte output record from the
page arena. The page check covers the **indices** the raster stage needs. The cut binary-searches
the index section of the page table for each selected cluster; it emits `page_id + 1` only when the
whole index window lies in one resident page, otherwise it requests the page(s) and writes
`lod0_count` into the item command. The runtime preloads the LOD0 records at `space.base` for every
GPU-cut item, so that command is a complete coarse fallback rather than a hole. The 48-byte meshlet
record's former `_pad` is now `page_id`; the raster resolves it through the page table/arena
descriptor slots and falls back to the draw's index buffer when it is zero.

The page table and arena are bound once per uploaded generation at
`render_layout::heap_slots::subtilis_page_table` and `subtilis_page_arena`. This slice is still
single-asset because those slots are scene-global; multi-asset streaming needs per-asset arena
offsets or descriptor arrays.

M5d: prefetch and priority tuning. Do not start this before M5c is stable.

Acceptance: zero holes, identical pixels when all pages resident, no page used before its
upload fence, bounded installs/frame, measurable resident-memory reduction, and a stable
fallback when the budget is deliberately lowered.

## 5. PR sequence

1. `docs: add Subtilis M2/M5 plan` (this document).
2. `feat(subtilis): upload GPU cut payload` -- data only, no behavior change; unit tests for
   layout and lifetime.
3. `feat(subtilis): GPU cut selection and indirect args` -- opt-in, HZB off, CPU fallback and
   A/B tests.
4. `feat(subtilis): GPU cut HZB counts` -- move HZB candidate counts to the GPU count buffer.
5. `feat(subtilis): page format and page table` -- offline, all-resident no-op.
6. `feat(subtilis): page residency manager` -- synchronous, one asset, budget/LRU.
7. `feat(subtilis): async page streaming and requests` -- cut request buffer, async installs,
   LOD0 fallback.

Each PR keeps the feature off by default until its acceptance tests pass. No PR bundles M2
and M5.

## 6. Risks and open questions

- **Floating-point parity.** The GPU metric must use the same matrix order and the same
  conservative closest-depth formula as the CPU. A bit-exact match is not realistic; the
  selected set must match, not the intermediate float.
- **Dependency closure.** `close_coarsening` is order-dependent. The GPU implementation must
  use the same group order or prove an equivalent fixed point. Do not rely on atomic order.
- **Determinism.** Atomic append would make record order nondeterministic. Use a prefix sum in
  stable cluster order for captures and A/B.
- **HZB.** Counts and survivors are currently CPU-fed. M2 without the HZB count change is a
  test configuration, not a shippable one.
- **Indirect buffer usage.** A host-visible buffer that is both shader-written and used as an
  indirect source may not be legal on every backend. Plan a device-local fallback.
- **Push block.** `plan.block.geometry.index_count` remains the CPU reservation count for
  fallback items. The mesh shader must continue to take the actual group count from the
  indirect command, not from this field.
- **M5 vertex paging.** Shared vertices mean v1 cannot free most vertex memory. v2 needs an
  offline reindex/duplication profile and a page-table indirection in vertex fetch.
- **Page size and install budget.** 64 KiB and a 128-page budget are starting points from UE;
  measure on the actual scenes before fixing them.
- **Backend differences.** Vulkan and D3D12 differ in indirect signature and barrier scope;
  keep the RHI abstraction explicit rather than embedding backend assumptions in the shader.

## 7. References

### Deren

- `source/engine/geometry/subtilis_dag_lod.cppm:49` -- CPU cut.
- `source/engine/geometry/subtilis_selection.cppm:101` -- admission and fallback.
- `source/engine/runtime/runtime.cpp:2183` -- item recording.
- `source/engine/runtime/runtime.cpp:3143-3164` -- HZB count and indirect command writes.
- `source/engine/visibility/visibility.cppm:259` -- meshlet run application.
- `source/engine/geometry/subtilis_upload.cppm:40-78` -- vertex/index retention.
- `source/engine/meshlet/meshlet.cppm:64` -- 48-byte meshlet ABI.
- `source/shaders/scene/visibility.slang:91` -- mesh entry; group count comes from the
  indirect command.

### UE 5.8

- `Engine\Shaders\Private\Nanite\NaniteHierarchyTraversal.ush:69`, `:251`, `:372`, `:417`.
- `Engine\Source\Runtime\Renderer\Private\Nanite\NaniteCullRaster.cpp:907`, `:939`, `:4201`,
  `:4500`, `:4515`.
- `Engine\Shaders\Private\Nanite\NaniteStreaming.ush:19`.
- `Engine\Source\Runtime\Engine\Public\Rendering\NaniteStreamingManager.h:28-63`.
- `Engine\Source\Runtime\Engine\Private\Rendering\NaniteStreamingManager.cpp:90`, `:2579`,
  `:2731`.
