# Render graph composition acceptance — 2026-10-16

Implemented the approved composition design, with the user's `ownable` naming.
Resources remain external; legacy rendering remains the default. This delivers
composable graph construction and migrates the existing post adapter to it, not
the G-buffer/lighting main chain itself.

## Changes

- Added `deren.engine.render_graph.compose`: constrained append/bind operations,
  canonical image/buffer lease imports, first-error latch, sealed preparation,
  explicit export/order forwarding and owner retention.
- Reused `pass_parameters` and added `ownable`, `graph_appender`, `pass_recorder`.
  Stored callables can be move-only. Exact noexcept return contracts reject
  mutable-only or loosely convertible recording functions.
- Added full `builder::contains(pass_ref)` membership checking. Named post outputs
  replace module/global pass-index assumptions.
- Extracted pure post::append(inputs, options), returning named output/pass refs.
  Independent compile uses the same implementation. Bloom/clear/composite/resolve
  have separate typed parameters.
- Extended prepared_graph with bound-action recording, retaining the original
  callback mode and one common synchronization loop. All live actions are validated
  before recording; overriding bound actions with a callback is refused.
- Allocated production resolved_io payload at its final address before resolving
  spans. Callables and retained owners follow the existing paced frame retention.

## Review and failures caught

Independent read-only review found no confirmed behavioral correctness issue.
It recommended direct composition entry coverage for buffers, stale frame images,
missing scoped capability and canonical aliases with conflicting shapes; added.
Moved the erased-action preparation bridge behind execute's internal
detail::composition_access, preserving the public interface boundary.

Two additional callable-value regressions were reproduced before fixing: binding
a null recorder pointer incorrectly succeeded, and invoking a typed null appender
caused the test process to fault. Both now return and latch invalid_argument before
invocation. Concepts constrain callable types, not non-null values or ownership.

## Validation

- Release full build with -j12; final CTest **43/43**.
- ASan graph/config/Boost subset **15/15**, including composition tests.
- Expanded archive boundary includes librender_graph_compose.a: **zero** backend
  symbols, owning-STL crossings and engine imports of Vulkan modules.
- Probe tests cover reorder/culling, wrong graph same index, duplicate/missing/type
  mismatched actions, partial append failure, callback failure, barrier-only and
  void append, canonical image/buffer import, stale snapshots, unsupported devices,
  shape conflicts, lifetime retention and compile-time positive/negative concepts.
- Actual NVIDIA RTX 4060 Laptop GPU: five legacy/graph capture pairs, Helmet,
  256x256, 8 frames, fixed scene camera, TAA off, GUI hidden, Vulkan synchronization
  validation enabled. Direct, FXAA, scale-0.5 upscale, Bloom-off/debug and windowed
  branches all produced identical PNG bytes with clean logs. Hashes match the
  previous [post-chain acceptance](2026-10-16-render-graph-post.md).
- Final composed runtime passed three window resizes over hundreds of frames,
  synchronization validation clean, final screenshot **522x324**, matching client
  dimensions. The existing resize harness was reused with the Helmet config.

Evidence in ignored build directories:

- build-release-dyn-clang64/render-graph-compose-{red,build,post-red,post-build,
  full-build,final-build,final-ctest,boundary,gpu,resize}.log and boundary.json.
- Additional callable regression evidence: render-graph-compose-null-red.log and
  render-graph-compose-appender-red.log; final composition tests pass.
- build-vessel-asan-clang64/render-graph-compose-{build,final-build,test}.log.
- build-release-dyn-clang64/render-graph-post-check/results-compose.json,
  *-compose/ captures and resize-compose/ application logs/screenshot.

No commits or pushes. Wider feature combinations and GUI-enabled stress remain
default-switch acceptance work. Next implementation stage can compose the basic
G-buffer/lighting main chain with the existing post::append in the same builder.
