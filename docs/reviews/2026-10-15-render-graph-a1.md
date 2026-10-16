# Render graph A1 verification

A1 adds the standalone render_graph_core library and deren.engine.render_graph
module. It imports no runtime/RHI/Vulkan module, allocates no GPU object, and does
not alter production pass execution. Resources remain outside the graph.

Delivered: opaque typed refs and incarnation checks, canonical physical identities,
owned registration records, transactional rejection, compile-once freeze, content
versions, value-root culling, live RAW/WAR/WAW, explicit order, stable minimum-ready
Kahn scheduling, owned results/diagnostics and normal expected failures.

Boost.Graph storage/reverse/filtered/BFS code is shared privately with its allocator
probe. The actual compiled module now participates in the zero-ordinary-new check;
no Boost type enters the public module interface or RHI ABI.

## Evidence (Windows Clang64 23.1.3)

- Task1 RED: missing module; GREEN: registration/identity/ref/name/freeze tests.
- Task2 RED: 12 failed content/version/culling assertions; GREEN after compiler.
- Task3 RED: 8 failed hazard/order assertions; GREEN after live hazard scheduling.
- Task4 RED: invalid-identity diagnostic lost its resource name; GREEN after owning
  the provided name. Result/diagnostic lifetime and 20,000-pass tests also pass.
- Full Release build with -j12: success. CTest: 32/32, zero failures.
- test_render_graph: 58 checks, including the actual compile OOM child.
- test_boost_graph: 274 checks, including actual module graph construction and
  compilation; observed ordinary-new delta zero across the measured path.
- Debug ASan/UBSan focused tests: 3/3 (render graph, Boost probe, vendor manifest).
- Artifact backend boundary explicitly includes librender_graph_core.a: zero
  backend symbols/reference sites/owning-STL crossings. Native-zero CTest passed.
- Independent whole-change review found no actionable correctness/integration
  defect. No deferred minors. git diff --check passed.

Detailed logs: build-release-dyn-clang64/render-graph-a1-{task1-red,task2-red,
task3-red,task4-red,full-build,full-ctest,boundary}.log and boundary.json;
build-vessel-asan-clang64/render-graph-a1-{build,test}.log.

## Execution decisions

1. Use the primary module's internal types partition for opaque-ref friendship.
   Reversal cost: adjust internal module boundary, no existing external consumer.
2. Separate BGL global-fragment headers from the allocator-aware post-import body.
   Reversal cost: adjust one private include split; no public interface change.
3. Extract shared BGL adapter in Task2, whose value traversal already needs it,
   rather than Task3. Reversal cost: task sequencing only.

The execution ledger preserves these decisions and per-task results. Existing
uncommitted changes were preserved. No commits or pushes were made. No legacy
registry/passchain/runner was removed.

A2 subresource ranges, typed parameter visitors and abstract barrier plans remain
unimplemented. GPU binding, recording, submission retention/state publication and
production Bloom migration remain later phases. Linux/MSVC builds were not run;
CPU validation does not claim GPU synchronization or image equivalence.
