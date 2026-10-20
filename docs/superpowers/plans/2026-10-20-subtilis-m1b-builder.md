# Subtilis M1b Builder Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans in the current session, TDD per task, and one fresh whole-change review. The user explicitly approved the written builder design and asked to start; preserve the existing inline/current-checkout workflow.

**Spec:** ../specs/2026-10-20-subtilis-m1b-builder-design.md (approved by user).
**Goal:** Produce real, immutable, validated CPU hierarchy geometry using fixed meshoptimizer algorithms, ready for a later upload integration.
**Architecture:** Keep original LOD0 indices/vertices; simplify complete unique-parent regions, lock original boundaries and seams, append coarse indices, pack a bottom-up forest, then call asset::make.
**Tech stack:** C++23 modules, utility::vessel/memory, meshoptimizer v1.3 at 9e1f07b159d3cb777f1c67ed31fc11fd117986f4, existing asset/cut.

## Global constraints

- Baseline db1005fa, current checkout; preserve .vscode and untracked CI checkpoints. No push unless requested.
- One CMake build at a time, -j12, clang64 PATH. Reuse Release and ASan build trees.
- Check 5h quota at milestones; <=5% remaining writes a checkpoint and pauses.
- No RHI/shader ABI, runtime LOD enablement, disk format or new frame passes in this builder slice.
- An unsupported simplification must keep complete LOD0. Invalid input and resource limits return typed errors; OOM remains fast_panic.

## File responsibilities and interfaces

- source/third_party/meshoptimizer/: unchanged pinned source, LICENSE.md, VENDOR.md with per-file SHA256; private meshoptimizer_vendored static CMake target.
- geometry/subtilis_builder.cppm: exported build API, immutable move-only built_geometry, options/metadata/errors; no upstream types.
- geometry/subtilis_builder_internal.hpp: private decoded source, logical node and candidate workspace shared by implementation TUs.
- geometry/subtilis_builder.cpp: input validation/copy/normalization, stable source hash, top-level build, packing and output-budget checks.
- geometry/subtilis_builder_topology.cpp: position equivalence, seam locks, oriented boundary signatures and source components.
- geometry/subtilis_builder_simplify.cpp: one-time allocator install, partition/simplify/recluster adapter and iterative frontier generation.
- tests/test_subtilis_builder.cpp: API/limits/normalization and actual hierarchy/cut checks. CMake + CI register it and add builder sources to tidy gate.

API in deren::engine::subtilis:

- build_input {meshlet_build_input geometry; uint32_t normal_offset=12, uv_offset=24;}.
- build_options {uint32_t group_size=8,max_depth=16; float target_ratio=.5f,max_error=.01f,normal_weight=.1f,uv_weight=.01f; uint64_t max_output_bytes=256*1024*1024;}.
- build_error {invalid_input, invalid_options, capacity, invalid_geometry}.
- build_statistics: nodes/roots/clusters/depth, lod0/coarse triangles, rejected topology/no-reduction/budget counts, per-level frontier triangle totals, source_hash and schema_version; include complete options and pinned revision metadata in built_geometry.
- built_geometry const accessors: vertices(), indices(), hierarchy(), options(), statistics(), vertex_stride(), vertex_count(), lod0_index_count(). Own all returned bytes; move-only and no writable spans.
- expected<built_geometry,build_error> build(build_input const&,build_options const&={}).

## Task 1: Dependency, validated source and complete LOD0 result

- [x] Write test_subtilis_builder for empty input, one triangle, owned input lifetime, 16/32-bit indices, nonzero first_index/base_vertex, byte-unaligned spans, bad windows/attributes/nonfinite values/options and output budget. Observe missing module/API RED.
- [x] Vendor fixed src + license from verified commit with manifest, no demos/GLTF/codecs in the build. Add static target to vendored-warning exemptions; keep upstream headers private. Initialize allocation callbacks once before adapter calls.
- [x] Implement public types and input checks using checked arithmetic/memcpy. Normalize indices to uint32/base0; retain source vertex bytes. Preserve LOD0 order and 85-triangle windows, computing conservative finite bounds in double then float rounding.
- [x] Return validated leaf-only forest for this task; source hash serializes descriptor fields/options explicitly. Handle empty forest and moves without stale cached counts. Compute output-byte budget before publication.
- [x] Build/run test_subtilis_builder plus existing subtilis tests GREEN; commit this independently usable source/LOD0 contract.

## Task 2: Safe original-region simplification and forest packing

- [x] Add positive regular-grid test requiring real parent levels and fewer coarse triangles AND records; observe leaf-only RED. Add open boundaries, seam duplicates, disconnected/degenerate/nonmanifold input and invalid candidate cases.
- [x] Decode aligned position/normal/UV arrays. Build exact position classes (+0/-0 normalized), seam locks, source-triangle adjacency/components; unsafe topology stays finest with statistics.
- [x] Partition frontier using complete logical nodes, stable ordering, target group_size. Merge source triangle IDs and current coarse runs; lock original-region boundary classes and all seam copies. Call attribute simplification with absolute errors and source-scaled weights, no destructive/pruning fallback.
- [x] Verify candidate source components and original oriented boundary multiset before accepting. Recluster at <=85 triangles/<=255 vertices and expand local indices to original vertex IDs. Accept only strictly smaller triangle and cluster counts within child worst-cut budget; reject without mutating payload/tree.
- [x] Accumulate parent error=max(children)+step with upward float rounding; iterate until no progress/depth. Pack reverse BFS levels, preserving sibling order and contiguous children; every root retained. Call asset::make before publishing result.
- [x] Run real hierarchy, mixed-depth packing and existing CPU cut/publication tests GREEN; commit.

## Task 3: Determinism, mixed-cut proof and integration verification

- [ ] Add a small multi-level grid with group_size=2; enumerate bounded legal cuts and compare combined oriented boundary multisets with LOD0. Check no simultaneous parent/child and valid index windows. Verify 3+ levels, unequal-depth branches, depth stop and budget failure without partial payload.
- [ ] Add repeated-input equality (records/groups/indices/metadata), concurrent first-use allocator initialization, source ownership after input destruction and conservative sphere containment. Feed built hierarchy into select_cut/prepare_hierarchy to demonstrate fewer records for a valid coarse view and complete forced finest.
- [ ] Run complete Release build + CTest, exact relevant ASan/UBSan tests, and tidy on changed builder sources. Tests requiring hardlinks/subprocesses use the established permissions.
- [ ] Fresh review of whole diff against spec; repair important findings with RED/GREEN. Update design status/evidence and next upload entry; commit validated results, no unsolicited push.

## Review focus

- Only original-region boundaries prove mixed-LOD compatibility; current frontier borders alone do not.
- Arbitrary partition order must not break contiguous child intervals, including unequal-depth roots.
- A rejected candidate must leave no orphan records or index window, and no source component may disappear.
- All length/index arithmetic is checked before upstream asserts, and output budget is not a claim about temporary allocator peak usage.
- Bounds use the stored rounded center; hierarchy data and index payload must remain immutable together.

## Ledger

Pre-flight: Task 1 produces normalized source + output API consumed by Task 2; Task 2 produces validated hierarchy consumed by Task 3. Source indices and asset first_index use one uint32 payload. No interface conflicts found.
Ruling: Continue native inline/current checkout under the user's explicit instruction to start the approved scheme; avoid another permission round for routine implementation decisions. The spec is binding; ledger any refinement needed by measured tests.
Ruling: Preserve existing LOD0 order/85-triangle runs rather than re-clustering the finest layer; coarse layers use meshoptimizer. This keeps fallback geometry directly comparable.
