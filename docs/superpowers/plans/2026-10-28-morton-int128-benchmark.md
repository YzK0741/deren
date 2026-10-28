# Morton Int128 benchmark

**Goal:** Compare the existing 96-bit bytewise encoder with Boost.Int128 mask/shift expansion,
preserving every input bit and the existing byte-lexicographic ordering. Benchmark only; do not
adopt a production optimization or change quantization/sort policy.

**Architecture:** A temporary utility-module probe exposes direct encoding and selects the encoder
used by the real BVH builder. All arms use the same dispatch. The benchmark consumes the actual
project BVH/allocator. Restore exact source and CMake bytes with fresh timestamps afterwards.
Pin official Boost.Int128 under tmp, record revision, compiler, flags, source hashes and raw samples.

- [x] Acquire fixed Boost source and preserve production bvh.cppm/CMakeLists.txt bytes.
- [x] Validate full 32-bit axes, endpoint 2^21, random and normalized quantized inputs, identical
  sorted keys, and BVH query membership. Verify the checker rejects an intentionally wrong encoder.
- [x] Measure encoding, precomputed-key sorting and full BVH make+destruction separately. Use
  deterministic random/clustered inputs at 2909/10000/100000 leaves, warmup and reversed orders.
  Include compiler-native unsigned 128-bit masks as a control to separate algorithm from library.
- [x] Independently review method/results, restore production sources, rebuild utility/application,
  run relevant checks and write a report with limitations. No renderer/frame-time claims from this CPU
  microbenchmark; no production Boost dependency is added. No commit/push requested.

Complete: two independent processes, 720 samples total, 24 per case, balanced six-permutation
orders. Correctness checks cover 524447 inputs x three encoders plus sorting/endpoints/real-tree
query membership; corrupted encoder exits 2. Per-iteration memory barriers prevent elimination.
Sorting uses actual vessel deque/node records with box-derived keys. Actual compile commands do
not enable LTO; encoder effective O2/native, consumer O3. All source bytes restored, application
and utility rebuilt, two targeted tests pass, method and numerical report independently reviewed.
Report: docs/benchmarks/2026-10-28-morton-int128.md, raw/summary CSV and compressed evidence.
