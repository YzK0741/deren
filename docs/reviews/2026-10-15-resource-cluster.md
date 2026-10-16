# Engine resource cluster verification

First incremental render-graph primitive: engine-owned resource clusters, public
RHI factories, owning leases and explicit runtime frame-slot retention. Existing
registry, snapshots and pass chains remain active. No production pass migrated.

Validation on Windows / Clang64 23.1.3:

- RED: the new test failed compilation because deren.engine.resource_cluster
  was missing; runtime integration separately failed for the missing factory and
  frame retention methods before those methods were implemented.
- Release full build succeeded with -j12.
- Release CTest: 29/29 passed; test_resource_cluster: 51 checks, zero failures.
- AddressSanitizer + UndefinedBehaviorSanitizer Debug build of
  test_resource_cluster succeeded with -j8; its CTest passed.
- Artifact backend boundary: zero backend symbols, zero reference sites, zero
  owning-STL crossings, zero engine/application imports of backend-owned modules.
- Independent code review found no actionable defect. git diff --check passed.

The focused test covers owned names, null devices, failed creation preserving
earlier members, same-device sharing and deduplication, foreign-device rejection,
view-to-image lifetime, escaped leases keeping the core alive, ordinary teardown
without wait_idle, the runtime retention window and completed-slot release.
Frame failure and shutdown retention are inspected in code, not exercised by
dedicated new tests. No Linux renderer build or GPU render comparison was run;
the production resource paths were not migrated in this change.

Detailed logs remain in the ignored build directories:

- build-release-dyn-clang64/resource-cluster-red.log
- build-release-dyn-clang64/resource-cluster-runtime-red.log
- build-release-dyn-clang64/resource-cluster-full-build.log
- build-release-dyn-clang64/resource-cluster-full-ctest.log
- build-release-dyn-clang64/resource-cluster-boundary.json
- build-vessel-asan-clang64/resource-cluster-asan-build.log
- build-vessel-asan-clang64/resource-cluster-asan-test.log

Usage and ownership constraints are documented in ../resource_clusters.md.
