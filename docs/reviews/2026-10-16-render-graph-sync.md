# Render graph abstract synchronization baseline

Extension of the existing A2 CPU compiler, following the agreed single-queue
design. Adds explicit usage/stage scopes, incoming state and optional abstract
barriers without RHI/native imports, ABI changes or production migration.

## Behavior and boundaries

- Typed fields and low-level accesses carry access_scope. Registration validates
  usage, stage, kind, aspect and content-operation combinations. Overlapping
  declarations require identical operation and scope before union normalization.
- Import accepts an optional uniform initial resource_state. Canonical aliases
  must agree. Content validity does not erase outstanding synchronization state.
- compile() remains dependency-only. compile({.synchronization = true}) requires
  complete live scopes and incoming state for used/exported resources, returning
  expected errors on missing declarations or live unsupported invalidate.
- Read-read image layout changes add state_transition edges. Barriers follow
  actual scheduled live execution, including same-layout RAW/WAR/WAW, accumulated
  reader stages and writer visibility to later readers in different stages.
- Sparse range states reuse existing partition operations. final_states covers
  required resources including untouched regions, retaining the current layout
  and conservative writer/reader frontier. Culled accesses do not affect it.
- Incoming mixed subresource state, requested final-state/epilogue transitions,
  host/WSI/readiness, AS/ray/mesh, cross-queue ownership, pass-internal synchronization
  and GPU feature checks remain outside this baseline. Existing RHI recipes have
  not been audited/lowered for these full stage sets/aspects yet. No GPU correctness
  or successful submission is inferred from a compiled CPU plan.

## Independent review and fix

The initial independent read-only review found one P1: a read-only image layout
transition without a logical writer was visible only to its first consumer stage.
The regression reproduced three failed assertions before the fix. This follows
the [Vulkan synchronization specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-image-layout-transitions):
layout transitions can write image memory and must precede new-layout accesses.

The fix conservatively widens that transition's destination to all currently
supported image-access stages and retains this scope in the final frontier. All
layout transitions carry the memory-dependency flag. Logical-writer cases continue
tracking per-stage visibility and chaining through earlier reader scopes. Same-
graph and next-graph regressions pass; focused independent re-review confirmed
the P1 resolved with no remaining actionable findings.

## Verification

Windows Clang64, C++23, exceptions/RTTI disabled:

- Initial RED failed compilation on absent scope/stage/state types.
- Sync test: 272 checks, including roles/stages, canonical incoming state,
  transactional rejection, legacy mode, GENERAL write visibility, read stage
  accumulation, read-only transitions, explicit-order cycles, disjoint ranges,
  typed parameters, culling and conservative frontier handoff.
- Dense byte oracle: 64 deterministic trials of 32 accesses across 16 bytes,
  checking RAW visibility, WAR/WAW stage coverage and unique final-state coverage
  independently of sparse partition storage.
- Existing A1/range/parameter checks pass (58/489/84 checks).
- Boost allocation probe: 284 checks, ordinary-new delta zero in measured path,
  now including actual module synchronization compilation and range splitting.
- Full Release build -j12 and complete CTest 35/35 passed after the review fix.
- ASan/UBSan build -j12 and focused CTest 6/6 passed after the review fix.
- Backend artifact gate explicitly includes librender_graph_core.a: zero symbols,
  reference sites and owning STL crossings. Full native boundary gate passed.
- clang-format dry-run, documentation test and git diff --check passed.

Evidence in ignored build trees: build-release-dyn-clang64/render-graph-sync-
{red,transition-red,build,full-build,full-ctest,boundary}.log and boundary.json;
build-vessel-asan-clang64/render-graph-sync-{build,test}.log. The transition-red
build log confirms compilation; its failing CTest assertions are also recorded in
the conversation. No Linux/MSVC or GPU run was performed. No commit/push or old
mechanism removal. Public contract and limitations: docs/render_graph.md.

Next: diagnostic dump and RHI mapping audit before execution integration.
