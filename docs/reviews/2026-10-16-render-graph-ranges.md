# Render graph subresource ranges

Bounded extension of the existing CPU graph: texture mip/layer/aspect ranges and
buffer byte intervals, with range-aware initialization, content versions, culling
and live hazard dependencies. No parameter visitor, barrier plan, RHI execution
or production migration was added. Existing working-tree edits were preserved.

## Behavior

- Imports accept shape metadata and canonicalize aliases against the complete
  parent shape. Invalid or inconsistent metadata rejects transactionally.
- Typed ranges default to the whole shape; remaining counts resolve before use.
  Empty, invalid-aspect, out-of-bounds and overflowing requests reject early.
- Same-pass disjoint operations are allowed. Same-mode overlaps form a disjoint
  union; conflicting overlaps reject. Read-write never consumes its own new value.
- Sparse intervals/rectangles split at actual access boundaries. State is not
  allocated per byte or per mip/layer. Fragment growth depends on access patterns.
- Export roots select ranges. Content reads, hazard edges and content errors
  identify overlap ranges. Explicit order edges have no resource range.
- final_ranges is an owned, disjoint cover of declared logical contents. defined
  describes the whole object; mixed producers yield invalid_index in last_producer.
  One writing pass advances one resource version, even with disjoint writes.

Compatibility choice: omitted shape retains A1's symbolic single mip/layer/color
or single buffer byte. These defaults do not infer real GPU sizes; future bindings
must validate explicit shapes against the real resource. Existing refs and public
module name are unchanged; internal result records gained owned range information.

## Verification

Windows Clang64 23.1.3, C++23, exceptions/RTTI disabled:

- RED: the new test failed compilation on absent texture/buffer range types.
- GREEN: focused range, A1 and Boost tests passed after implementation.
- Range test: 489 checks. Includes deterministic 128-trial dense rectangle oracle,
  partial initialization/invalidation, aspect/layer independence, per-range roots,
  disjoint versus overlapping WAR, same-pass read-write unions and invalid inputs.
- The dense reference uses 24 individual subresource cells, independently of the
  production sparse partition. It checks the required writer set, unique coverage,
  producer, version and defined state; both accepted/rejected exports are exercised.
- UINT64_MAX-sized buffers and UINT32_MAX mip/layer shapes are tested without
  dense storage; their single accessed cell produces only a few sparse records.
- Existing A1 tests, including 20,000-pass chain and OOM child, still pass.
- Boost probe includes actual module partial-buffer/rectangle operations and
  still observes zero ordinary-new calls in its measured path.
- Full Release build with -j12 succeeded; full CTest passed 33/33.
- Final sanitizer build: focused ASan/UBSan tests passed 4/4, including the newly
  added same-pass overlapping read-write regressions.
- Artifact backend check includes librender_graph_core.a and remains zero;
  native-zero CTest passed. No RHI/runtime/native import added to the graph core.
- Independent read-only review found no actionable findings. No deferred minors.

Evidence logs live in the ignored build trees:
build-release-dyn-clang64/render-graph-ranges-{red,focused,full-build,full-ctest,
boundary}.log and boundary.json; build-vessel-asan-clang64/render-graph-ranges-
{build,final-build,test}.log. Final extra read-write checks were added after the
full run and passed focused Release and final ASan/UBSan tests; production code
did not change afterward. No Linux/MSVC or GPU/image validation was run.

Usage and result semantics: docs/render_graph.md. Parameter visitors and abstract
synchronization remain the next A2 increments. No commit/push or old-path removal.
