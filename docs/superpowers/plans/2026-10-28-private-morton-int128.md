# Private Morton Int128 integration

User approved production adoption with Boost.Int128 confined to BVH internals. Execute inline using
executing-plans and test-driven-development; one independent final review. Preserve the existing
workspace and all unrelated uncommitted changes: the current renderer baseline depends on them.

Design: private `source/utility/BVH/morton_encoding.hpp` owns constexpr mask expansion and returns
exactly twelve little-endian bytes. Include it only in the BVH global module fragment and a private
unit test; do not export a Boost type, generic integer alias or new utility API. Keep quantization,
byte sorting, node layout and internal-node computations unchanged. Pin the benchmarked official
commit in an independent vendor subtree with original Git blob bytes, license, manifest and offline
verification. CMake exposes that include root only to static_utility and the private test.

- [x] Add permanent private-encoder oracle/boundary/random/ordering tests; observe missing-helper
  RED before implementation. Add real-BVH membership coverage via existing utility tests if needed.
- [x] Vendor exact upstream headers and license, record provenance, add offline CTest verification.
- [x] Implement mask expansion, retain existing compile-time golden vectors and verify GREEN.
- [x] Build production and tests, run relevant/full checks, independent review; capture a real
  animated scene before/after with validation and clean timing, retaining hashes/settings/images.
- [x] Document actual results and platform limits. No commit/push requested.

Completed 2026-10-28: [integration report](../../benchmarks/2026-10-28-private-morton-int128.md).
Production animated captures show 15.1% / 6.6% / 8.5% lower near/far/sweep mean frame and
0.68-0.98 ms lower CPU begin; the pinned-pose control shows no begin benefit (+0.004 ms) and
+0.14 ms frame noise. Far/pinned images are exact; near/sweep retain the known capture-order
difference. Release CTest 75/75, ASan encoder 100161/0, clang-tidy clean, offline vendor verification
passes. The generated per-run CSV and compressed evidence now back the report; no commit was made.

Ruling: use existing dirty workspace rather than a fresh checkout that omits the accepted renderer
baseline; all edits are scoped and validated. No performance thresholds in correctness tests.
Full 32-bit random inputs and endpoint 2^21 remain covered; no exposed int128_t/uint128 alias.
