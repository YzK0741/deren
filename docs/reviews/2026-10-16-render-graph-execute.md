# Render graph bound recording review — 2026-10-16

Later post-chain migration replaces numeric VMA handle identities with stable VMA
allocation-record addresses and adds acquire-scoped frame snapshots. The evidence
below describes this earlier stage; see [post-chain acceptance](2026-10-16-render-graph-post.md).

## Scope

Adds an opt-in `deren.engine.render_graph.execute` module, an optional
`rhi::scoped_synchronization` extension and its Vulkan implementation. Existing
production passes retain their current recording path.

- `identify(lease)` uses backend canonical allocation identity and the core domain.
  Vulkan uses the common VMA handle distributor for buffers/images. Deduplicated
  image wrappers therefore share a graph identity. Zero/foreign/unsupported
  identities fail import or preparation.
- `prepared_graph::prepare` owns the graph snapshot and required leases, validates
  live/export bindings, shape, usage and stages, then validates every bound batch
  before recording anything. The core CPU compiler stays RHI independent.
- `record` validates/records each batch before a noexcept error-returning callback.
  Empty groups also check command ownership/state. Failure stops the traversal;
  the caller discards the partial command buffer.
- `retention()` pins the prepared graph, core and bindings through caller-managed
  GPU completion. Auxiliary callback resources require separate retention.
- Vulkan checks resource/command registry before casts, queue support and exact
  ranges; combined depth/stencil subsets are conservatively unsupported without
  enabled separate-layout features. Owned cluster images only; no WSI adoption.
- `resource_cluster_core` is a small separate build target so the execute adapter
  does not depend on the aggregate engine target or create a future link cycle.

## Independent review

The engine review found no blocking defect; its context-borrowing clarification
and empty-graph command check were incorporated. The backend review found a real
VMA deduplication alias hazard in wrapper-address identity. It was fixed with
backend allocation identity, and a probe alias test checks distinct wrappers
import as one graph resource. A second check covers shared allocator identity. Identity is valid while the
original lease remains alive from import to prepare; handles can be recycled
after destruction, and a standalone compiled graph is not a resource owner.

## Limits

Recording is implemented and compiled, but no production pass has switched to it.
This work does not submit, publish state, add epilogue transitions, determine GPU
completion, retain callback-only views/pipelines/descriptors, or migrate Bloom.
Real-device synchronization validation and image comparisons remain required.
No Linux/MSVC execution, commit/push or old-path removal is part of this step.

## Verification

Windows Clang64, exceptions/RTTI disabled:

- RED failed on the missing execute module; GREEN execution test passes 42 checks.
- Covers canonical alias imports, missing/duplicate bindings, capability refusal,
  image/buffer shape mismatch, foreign commands, late preparation failure with
  no recording, callback failure, no-barrier buffer reads, empty graphs,
  export-only bindings, and retention after original owners are dropped.
- Full Release build with -j12 and complete CTest: 39/39 passed.
- ASan/UBSan build with -j12 and focused graph/Boost/JSON CTest: 10/10 passed.
- Backend artifact gate includes graph core, RHI adapter, execution adapter and
  resource cluster archives: zero backend symbols/sites/owning-STL crossings.
- Full native-zero, documentation/CI-list and dynamic-boundary tests passed.
- Formatting dry-run and git diff --check passed.

Evidence in ignored build trees: build-release-dyn-clang64/render-graph-execute-
{red,build,full-build,full-ctest,final-build,final-ctest,boundary}.log and
boundary.json; build-vessel-asan-clang64/render-graph-execute-
{build,test,final-build,final-test}.log.


