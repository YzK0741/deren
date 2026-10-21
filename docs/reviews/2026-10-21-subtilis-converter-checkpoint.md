# Subtilis converter checkpoint — 2026-10-21

Plan: [CPU glTF DAG converter](../superpowers/plans/2026-10-21-subtilis-gltf.md).
Base: d900c48f. Current-checkout inline execution, local commits, no push.

## Task 1: standalone codec and identity

- Extracted existing vstd/shared utility/mimalloc and CPU asset dependency target
  definitions into shared CMake helpers; existing target/module names retained.
- Standalone Release configuration explicitly disables finding Vulkan. Generated
  dependency graph contains no engine, promise/RHI, Vulkan, or CUDA targets.
- Wire v1 codec handles seven sections, explicit little-endian fields, checked
  counts/ranges, zero padding, JSON metadata and finite scalar records. The
  returned payload remains untrusted; it is not a validated DAG.
- Streaming SHA-256 adapted from the existing test implementation. Source/build
  identities have independently generated Python struct/hashlib golden vectors;
  NIST empty/abc/multiblock/million-a vectors pass.
- yyjson 0.12.0 fixed commit vendored with license and SHA256 manifest; all three
  source Git blob IDs match upstream. Mutable JSON parsing uses utility memory.
- MSYS2 simdjson 5.0.4 considered: retain vendored 4.6.9 for fastgltf and yyjson
  for document mutation. Switching to an immutable parser does not remove the
  need to preserve and modify arbitrary original JSON.

Validation: witnessed codec RED (encode missing behavior), then Release GREEN;
all standalone targets build with `-j12`, codec test passes (0.15s). Root Release
configuration/build succeeds; 58/58 CTest pass in 37.21s. First sandboxed root run
had two `os.link` WinError 5 errors in test_backend_boundary; rerun outside that
restriction passes unchanged tests. No product behavior changed to bypass it.

Ruling: source_digest/build_digest return `result<digest>` to reject nonfinite
geometry/invalid options as typed errors before hashing. Callers must unwrap;
canonical valid-input encoding remains fixed and documented.

## Task 2: immutable validated DAG and CPU cuts

- Validates table limits/ranges, reciprocal producer/consumer relationships,
  fine-to-coarse group order, strict triangle progress, exact leaf/root sets,
  finite metrics, nested LOD spheres, and all reference cells (including unused ones).
- Full factory binds the source digest, checks leaf triangle multiplicity/winding,
  cluster/group sphere containment in double precision, and per-reduction directed
  boundary/component preservation with normal/UV seam identities. Owned input
  survives destruction of caller copies. No mutable payload API is exposed.
- CPU cut supports model-space thresholds and standard right-handed ZO
  perspective/orthographic projections using column-major matrices. Nonuniform
  scale/shear uses a conservative operator-norm bound. Invalid views/near-plane
  crossing fall back conservatively; capacity never truncates a cut.
- Metric decisions are closed along group dependencies before scanning clusters.
  The private closure helper is independently tested with inconsistent raw
  decisions, including idempotence; no public test hooks were added.
- Actual cross-group fixture reduces 24→16→12 triangles and exercises a mixed
  cut `{5,7,8}` as well as full LOD0/intermediate/root cuts. Tests include 128
  seeded thresholds and 128 seeded camera distances, plus inward/outward f32
  sphere rounding, malformed references, and geometry changes.

Validation: witnessed initial DAG RED; an additional invalid unused-reference
fixture also failed before its check was added. Release 2/2 CTest pass (0.13s);
ASan/UBSan Debug 2/2 pass (0.76s). Root shared build configuration did not change
in this task; its Task 1 58/58 run remains the relevant regression check.

## Task 3: document preservation and fixture file loop

- Added an owning mutable document, stable mesh/primitive keys and instance/skin
  classification. Original DOM, array IDs and embedded BIN prefix are preserved.
- Embedded GLB and data-URI glTF decode via fastgltf after independent bounded
  window/sparse-index checks. No node transforms are baked into geometry.
- Unknown nested JSON and u64 precision, nodes/scenes/materials/accessors,
  original primitive fields and animation tracks survive extension attachment.
- The hand-authored DAG now completes attach→GLB→read→decode→full factory→cut.
  Supported required DEREN metadata is filtered only in the parser copy; original
  declarations survive. Unknown/unsupported required extensions and invalid
  required/used relationships fail. Unsupported optional payload leaves LOD0 readable.
- Tests cover signed-normalized quantization/strides, sparse/no-base accessors,
  overflow/bad windows, nonindexed triangles, missing UV, texture requirements,
  unmodeled attributes, morphs/alpha modes, static+skin instances and rigid animation.
- Found fastgltf 0.9 `validate()` dereferences sparse bufferView indices without a
  bounds check. A negative fixture reproduced an ASan heap-buffer-overflow at
  upstream fastgltf.cpp:999. Converter preflight now checks those references and
  analogous animation references before calling upstream; vendored code unchanged.
  Additional RED→GREEN regressions cover direct attachment to an ineligible
  primitive and required-extension version/subset handling.

Validation: initial document RED witnessed. Latest Release 3/3 pass (0.27s),
ASan/UBSan 3/3 pass (1.04s). Root dependencies unchanged since Task 1.
External-resource loading/packaging and transactional output remain Task 6;
embedded-mode helper rejects resource relocation it cannot yet preserve.

## Task 4: spatial clustering and constrained reduction candidates

- LOD0 and reduced meshlets use meshoptimizer with the profile's 256-vertex /
  85-triangle limits and cone weight zero. Original vertex IDs and source
  triangle multiplicity/winding remain intact; bounds round outward.
- Partitions use meshopt_partitionClusters and stable group/member order.
- Reduction uses meshopt_simplifyWithAttributes with immutable source data,
  primitive-diagonal normalization, fixed normal/UV weights, explicit seam/border
  locks and isolated floating-point status. Only strictly smaller valid candidates
  are accepted; no-progress/unsafe regions retain their original representation.
- Position and attribute boundary signatures plus source-component ownership are
  checked again after simplification. This builder topology remains independently
  checked by Task 2's full asset validator when assembled in Task 5.
- Topology reuse adds an optional private cached-topology argument to reduce_group,
  avoiding a whole-source analysis for each candidate; four-argument callers retain
  the original convenience form. This is not a public consumer API.

Validation: witnessed initial leaf-clustering RED. Curved-grid reduction, exact
LOD0 coverage, partitions, shared boundaries, UV/normal seams, disconnected
components, duplicate/nonmanifold faces, invalid options/nonfinite indices and
scale invariance pass. Release 4/4 CTest (0.20s), ASan/UBSan 4/4 (1.23s).

## Task 5: serial DAG construction and working CLI

- Serial rounds partition the active frontier, prepare immutable candidates and
  assign accepted global IDs in stable order. Declined regions carry forward;
  local error accumulates with dependency error and nested spheres round outward.
- A generated 32x32 grid yields eight groups and two producers whose coarse
  clusters feed distinct consumers. The test scans actual references; a hand DAG
  or an all-LOD0 result cannot satisfy this witness. Repeated payload bytes match.
- `build`, `inspect`, `validate --full` and `export-cut` now work for self-contained
  GLBs. Exported indices match CPU cut selection at three thresholds, and decoded
  source attributes remain exact. Tests cover skipped/strict/zero-target cases
  and explicit LOD0-only reporting.
- Destination-volume staging validates the generated GLB before publishing without
  replacing an existing target. Full packaging, overwrite/reuse policy, external
  resources, parallelism and real-scene/CI acceptance remain Tasks 6–8.

Validation: Task 5 initial builder/CLI RED recorded in the execution workspace.
Release 5/5 (0.37s), ASan/UBSan 5/5 (2.10s). Actual executable smoke:
build/report, inspect, full validate and cut
export all exit 0; preview instance/attribute identities checked independently.
Usage and current limitations are in `tools/subtilis/README.md`.

Ruling: minimal validated, no-replace staging was pulled forward from Task 6 to
make Task 5's CLI output safe to try. It costs a later packaging refactor; full
transaction/resource guarantees are still Task 6 acceptance requirements.

## Task 6: complete (finish pass 2026-10-21)

Base for the finish pass: `d3e371b7` (includes the paused partial work of `dbfc37c0` and `d3e371b7`).

- Transactional glTF directory-package output implemented end-to-end. `--format glb|gltf`
  (default glb) selects the output; `--format gltf -o out.gltf` creates the package
  directory `out/` containing `out.gltf` plus relocated resources (`out.bin` for the
  embedded/appended buffer, `bufferN.bin` for external buffers, `imageN.png`/`imageN.jpeg`
  for external PNG/JPEG images, all referenced by percent-encoded relative URIs). The
  package stages into `.subtilis-package-*` under the destination's parent, every staged
  file is SHA-256-verified against what was written, the staged document is fully
  validated, and the package is published by directory rename. Overwrite moves the old
  target aside, renames the staging directory in and removes the old target, rolling the
  old target back if publication fails. Existing targets are refused without
  `--overwrite`, source-inside-package and report-inside-package are refused, and the
  single-file GLB path keeps its semantics (now explicitly refusing directory and
  read-only targets).
- Test-only fault-injection seam `package_detail::sink_ops` (production entry points pass
  `nullptr`; no environment variable or config key can inject): forced short write (error
  reported), silent short resource write (caught by the staged hash verification), close
  failure and injected rename failures all leave source and prior-target bytes untouched,
  clean staging and return typed io errors. The GLB writer uses the same seam.
- Resource/DOM checks: per-file staged hashes, full staged validation, and exhaustive
  DOM/resource-identity checks cover external buffers/images, base64 data URIs (kept
  inline unchanged), percent escaping, multiple buffers, the rich document (buffers,
  bufferViews, images, animations, unknown extensions, array IDs and payload views) and
  the unknown-extension refusal arm for GLB with a working glTF package fallback.
- Validation: standalone Release CTest 5/5 (0.36s, converter 516 checks PASS); standalone
  ASan/UBSan gate result below. The cross-volume check is present but reports SKIPPED in
  this harness because the test process receives an access-denied probe on `D:` (see the
  test NOTE); same-volume staging-on-destination-parent logic and publication renames are
  covered by the injected-path tests. Root build files unchanged since Task 1; root
  CTest 58/58 remains the relevant regression.
- No commit from the implementer: the Lead accepts and commits.

## Task 7: complete (2026-10-21)

Base for the task: `1e3df1e7` (Tasks 1-6 complete). Files: new
`source/asset/subtilis/parallel.cpp` and `source/tests/test_subtilis_parallel.cpp`; modified
`source/asset/subtilis/builder.cppm`, `builder.cpp`, `builder_internal.hpp`, `reduce.cpp`,
`tools/subtilis/CMakeLists.txt`, `tools/subtilis/commands.cpp`, `tools/subtilis/README.md`;
contract-only updates in `source/tests/test_subtilis_dag_builder.cpp` and
`source/tests/test_subtilis_converter.cpp` (the two old "threads must be 1" assertions). Root
engine/CMake, the wire format and canonical build identity are untouched.

- Deterministic scheduler: private `builder_detail::run_ordered(count, threads, estimates,
  max_known_work_bytes, prepare, commit)` in `parallel.cpp`. Prepares run concurrently, exactly
  once, each writing only the caller's per-index slot; all prepares complete before any commit;
  commits run on the calling thread in ascending index order, never in completion order and
  never interleaved with concurrent prepares. Dispatch is coarse-work-first purely for load
  balance. A prepare error cancels not-yet-started tasks, runs no commit and returns a typed
  error (no partial cut). Worker threads are always joined; no detached tasks or cross-build
  state. Concurrency is a deterministic function of the estimates and budget:
  `min(threads, count, max_known_work_bytes / max_estimate)`; a budget that cannot hold one
  maximum task is a typed `capacity` error; threads=1 keeps the serial reference behavior on
  the calling thread. `execution_options::threads` is the single execution budget.
- Two scheduling levels, never nested: several eligible primitives use a bounded work pool over
  coarse primitive tasks (each inner build serial, document mutated only in the commit callback
  in primitive-key order); a single primitive (or a single group) gets the whole budget and
  parallelizes its per-round reduction groups (candidates prepared against immutable inputs, then
  validated/committed in partition order). The serial path preserves its original
  prepare-then-commit interleaving and rebuilds index spans just-in-time (a first refactor that
  precomputed them before interleaved commits reproduced a stale-span mismatch; fixed and RED
  evidence recorded).
- Workspace/budget: shrinking `build_limits::max_known_work_bytes` lowers
  `build_report::planned_concurrency` (monotone, witnessed 2 -> 5 on the grid fixture) without
  changing any successful asset's payload/reductions; too little budget rejects the whole
  primitive with `capacity`. The converter uses per-primitive accessor-count estimates for its
  pool admission. Known-work accounting covers source records, owned payload/workspace and
  conservative per-task estimates; meshoptimizer internal temporaries are not a provable
  process-memory bound (stated in the report's `knownWorkNote`).
- Stage profiling: `build_report::stages` has eleven fixed rows (`sourceFingerprint`,
  `leafCluster`, `topology`, `partition`, `boundary`, `simplify`, `recluster`, `candidate`
  inclusive, `commit`, `validate`, `encode`) with per-stage accumulated worker-task time
  (thread time, not CPU cycles), `inclusive` flags and task counts; `accumulated_ms` sums only
  exclusive rows so nothing is double counted; `threads`, `planned_concurrency`,
  `known_work_limit_bytes` and the wall time are reported separately. Thread/timing/memory
  statistics never enter the payload, build metadata string, source hash or build hash.
- CLI: `--threads N` accepts any positive integer, omitted defaults to
  `min(max(hardware_concurrency,1),12)` (12 on this machine), `0`/negative/non-numeric stay usage
  errors (exit 2). The report JSON now carries `threads`, per-primitive `plannedConcurrency`,
  `knownWorkLimitBytes`, `accumulatedMs`, `stages` and the `knownWorkNote` boundary statement.
  Reused payloads report an empty `stages` array because no build ran.
- Determinism: `build_serial` remains the reference. For threads 1/2/12 (and 1000, budgeted
  down) the validated source geometry, DAG (`indices`/`clusters`/`groups`/`references`/`roots`),
  stable IDs, `sourceHash`/`buildHash`, build metadata and `encode(payload)` are byte-identical;
  only the report's thread/timing fields differ.
- RED/GREEN: `test_subtilis_parallel` was added to the standalone CMake and built first;
  RED witnessed as compile errors for the missing `stage_profile`, report fields and
  `builder_detail::run_ordered` (e.g. `error: no member named 'run_ordered' in namespace
  'deren::asset::subtilis::builder_detail'`). After implementation the same target built and
  passed.
- Validation (this finish pass): `cmake --build build-subtilis -j 12` exit 0;
  `ctest --test-dir build-subtilis --output-on-failure` 6/6 PASS (1.03s, `test_subtilis_parallel`
  580 checks PASS, Release); `cmake --build build-subtilis-asan -j 12` exit 0 and
  `ctest --test-dir build-subtilis-asan --output-on-failure` 6/6 PASS (19.58s, ASan+UBSan Debug).
  CLI spot evidence on a generated 1,152-triangle self-contained GLB and a 3-primitive variant:
  `deren-subtilis build ... --threads 1|2|12` (and the no-`--threads` default) produced
  byte-identical outputs (single SHA-256 per fixture family); `validate --full` 1/1 and 3/3
  payloads pass, `inspect`/`export-cut` exit 0, report structural fields (levels, rounds,
  known-work peak, stage rows) identical while `threads` differs. Root build files unchanged
  since Task 1; root CTest 58/58 remains the relevant regression.
- Residual risks / not covered here: (a) ASan on this Windows clang64 toolchain does not run
  LeakSanitizer, so "leak-free" is evidenced by repeated success/failure teardown loops and
  by construction (joined workers, RAII slots) rather than LSan; a Linux CI sanitizer job in
  Task 8 should add real leak/race detection. (b) The multi-primitive converter pool prepares
  every primitive before committing any, so peak host memory can exceed the old one-primitive-
  at-a-time loop; admission estimates bound concurrency but not a strict process peak (Task 8
  measurement should record it). (c) A large primitive among many small ones gets one thread
  (primitive-level priority by design); nested budget partitioning is future work. (d) A
  prepare failure cancels remaining tasks and reports one typed error; which of several
  simultaneously failing tasks supplies the error context is not part of the determinism
  contract (successful assets are). (e) Stage `cpuMs` is accumulated task time, not hardware
  CPU-time counters; Task 8's measure script must not treat it as CPU-cycle profiling.
- No commit from the implementer: the Lead accepts and commits
  (`perf: parallelize Subtilis asset construction deterministically` expected).

Remaining work: Task 8 (real assets, Khronos validator, independent CPU-tool CI, final fresh
whole-change review, docs/archive). Tasks 1-7 are complete as described above; do not redo them
and do not claim final converter acceptance.
