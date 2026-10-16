# Frontend semantic states and Vulkan lowering

Implements the user's clarified design: frontend layout descriptions may differ
from Vulkan native layouts as long as behavior is preserved. No frontend GENERAL
policy is introduced. The earlier audit recommendation to unify their names is
withdrawn in the current manual/spec/roadmap and noted in the historical audit.

## Implemented scope

- New handle-free data module deren.promise.rhi.synchronization defines independent
  endpoint stage/access sets, abstract image states, explicit aspect/mip/layer and
  buffer byte ranges. Existing virtual recording ABI and legacy descriptors are
  unchanged; the data module does not imply a backend capability/entry point.
- Separate engine module deren.engine.render_graph.rhi converts synchronization-
  compiled plans through lower_scopes into indexed RHI scope packets. It retains
  order/resource/pass association, validates references/ranges/order/known flags,
  and uses utility/vessel allocation. The CPU graph core stays RHI-independent.
- Independent backend module deren.vulkan.synchronization maps RHI transitions
  into unbound VkImageMemoryBarrier2/VkBufferMemoryBarrier2 templates. Ordinary
  abstract layouts map to GENERAL; UNDEFINED and PRESENT remain distinct.
- Source and destination stage unions are mapped independently. Generic read/write
  masks become Vulkan MEMORY_READ/MEMORY_WRITE. Same-native-layout barriers are
  retained, including read-only abstract transitions. No stage_hint guessing or
  role-pair reduction is used. Explicit stencil subsets are preserved.
- Bad flags/aspects/layouts, empty/overflowing ranges and values colliding with
  Vulkan remaining-count/whole-size sentinels return errors rather than changing
  range meaning. Native conversion itself allocates no memory or calls Vulkan.
- Frontend plans, dumps and final abstract handoff states are not rewritten.
  Backend-native state spelling is available through map_layout.

The new static converters are exercised directly by tests. No production recorder
consumes them yet. Native handles are null templates, not bound executable
commands. Real object/device/format/usage/feature/queue validation, lease retention,
native state provenance, recording and submission publication remain required.
No success here establishes GPU synchronization correctness.

## Verification

Windows Clang64, C++23, exceptions/RTTI disabled:

- RED: new test failed compilation on the absent graph-to-RHI module.
- GREEN: lowering test passes 214 checks, covering ordinary layout pairs,
  UNDEFINED/PRESENT exceptions, independent/mixed endpoint masks, explicit
  depth/stencil, exact image/buffer ranges, invalid input, sentinel/overflow checks,
  unchanged frontend state and full graph -> RHI -> Vulkan conversion.
- Includes compute write -> fragment read -> transfer write: the later barrier
  retains both source stages and read/write accesses after GENERAL collapse.
- Includes compute-written buffer -> vertex/indirect reads and read-only abstract
  layout transitions whose native old/new layouts are equal.
- Existing graph/range/parameter/sync/dump and JSON tests remain passing.
- Boost allocation probe: 286 checks, ordinary-new delta zero in measured path,
  including graph-to-RHI scope-plan construction.
- Full Release build -j12 and complete CTest 38/38 passed.
- ASan/UBSan build -j12 and focused CTest 9/9 passed.
- Backend artifact gate explicitly includes both librender_graph_core.a and
  librender_graph_rhi.a: zero symbols, reference sites and owning STL crossings.
  Full native-zero, documentation/CI-list and dynamic-boundary tests passed.
- Formatting dry-run and git diff --check passed. Independent read-only review
  found no actionable correctness defects. Its optional mixed-frontier test was
  added and passed in the final full/sanitizer runs.

Evidence in ignored build trees: build-release-dyn-clang64/render-graph-lowering-
{red,build,full-build,full-ctest,boundary}.log and boundary.json;
build-vessel-asan-clang64/render-graph-lowering-{build,test}.log.
No Linux/MSVC/GPU validation, commit/push or old-path removal.

Current API and remaining recording requirements:
[RHI semantic mapping](../render_graph_rhi_mapping.md).
