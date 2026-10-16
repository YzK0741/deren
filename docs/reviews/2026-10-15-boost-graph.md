# Boost.Graph minimum integration

Decision: use Boost.Graph 1.92.0 for graph storage, reverse/filtered views and
traversal underneath the upcoming render graph. deren still owns resource
semantics, dependency construction, stable scheduling policy and synchronization.
The render graph production module and A1 compiler are not implemented here. (Scope of THIS
increment only: the A1 record of the same day, reviews/2026-10-15-render-graph-a1.md, delivers the
standalone CPU graph core and supersedes this sentence as a statement about the tree.)

## Delivered

- source/third_party/boost: BCP-extracted 1,665-header subset, 16,824,687 bytes,
  original license, per-header SHA-256 manifest and provenance. No header patches.
- Private boost_graph_headers CMake INTERFACE target; no Boost binary library.
- scripts/vendor_boost_graph.py: fixed-entry extraction into a fresh staging
  directory, byte-preserving manifest generation and offline verification.
- test_boost_graph and test_boost_graph_vendor, with sanitizer CI registration.
- Updated graph design and stage-two roadmap to select this library foundation.

## Verification

Windows Clang64 23.1.3, C++23, exceptions and RTTI disabled:

- The integration test first failed on the absent vendored version header,
  ensuring it could not silently use the installed Boost instead.
- Release full build with -j12 succeeded; full CTest passed 31/31.
- test_boost_graph passed 16 checks, including the ordinary-new observer's
  positive control, disconnected branch pruning, duplicate roots, parallel edges,
  insertion-independent stable order, cycles/self-loops, empty graph and a
  20,000-node chain. Injected utility allocation failure terminated the child
  through existing fast_panic without a stack trace.
- Debug ASan/UBSan focused test passed with the same checks.
- The exercised adapted storage/traversal/sort/destruction path made zero calls
  to the executable's observed ordinary new overloads. Storage uses the real
  utility allocator; this is not a replacement allocator mock.
- All 1,665 headers matched the cached pinned MSYS2 package byte-for-byte.
  Extraction repeated with the checked-in script produced the same manifest.
- Ninja's actual compiler dependency list showed no system Boost fallback.
- A temporary module interface using the adapted graph and a client importing
  only that module compiled, linked and ran successfully. The probe stays in the
  ignored build directory; it is not a second graph implementation in engine.
- Git attributes disable text conversion for the vendor subtree; check-attr
  and filtered/raw hash-object checks confirmed autocrlf preserves header bytes.
- Independent review found no actionable defect and reverified the full header
  manifest. Final sanitizer checks passed 2/2 (probe plus vendor manifest).

## Constraints for A1

Keep Boost private. Use the validated custom vector/list selectors, allocated
graph property, reverse/filtered views, and explicit BFS color map/work queue.
The random-access selector currently specializes an internal BGL trait, so the
version pin and upgrade probe matter. Do not assume default DFS, default graph
properties, arbitrary mutations or new algorithms use the project allocator.

Stable Kahn scheduling is project policy and returns expected on cycles; do not
route a normal invalid graph through BGL's throwing topological_sort. Unexpected
Boost exception hooks terminate. MPI/distributed graph support is excluded.
Linux/MSVC builds were not executed locally; production GPU behavior is unchanged.

Detailed build, test, dependency and temporary module logs are under
build-release-dyn-clang64/boost-graph-probe/; sanitizer logs are
build-vessel-asan-clang64/boost-graph-build.log and boost-graph-test.log.
