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

## Task 8: real-asset measurement, Khronos validator, CPU CI and evidence (2026-10-21)

Base: `071cfdd5` (Task 7 committed) plus the uncommitted Task 8 worktree. A real-asset full-validation
defect found here was fixed as a separate, reviewable change (see the defect section below). Files:
new `scripts/measure/make_subtilis_converter_fixtures.py`, `measure_subtilis_converter.py`,
`verify_subtilis_converter.py`, `subtilis_converter_gltf.py`,
`scripts/measure/khronos_validator_manifest.json`, new `.github/workflows/subtilis-converter.yml`,
new `docs/reviews/2026-10-21-subtilis-converter.md` + `docs/reviews/2026-10-21-subtilis-converter/`
results JSONs; updated `tools/subtilis/commands.cpp` (external report gains per-primitive rejection counters only;
no payload/identity change), `tools/subtilis/README.md`, `scripts/measure/README.md`,
`source/tests/test_subtilis_parallel.cpp` (report-JSON rejection-counter assertion only),
`docs/subtilis.md`, `README.md`, the plan Task 8 checkboxes and the design spec status line. No root
engine/CMake or wire-format/identity changes; no git mutations (Lead commits).

- Measurement: two deterministic generated fixtures (small 32x32 grid = 2,048 triangles; synthetic
  large 256x256 grid = 131,072 triangles) and the real Bistro asset in a matrix of
  `--threads 1` and `--threads 12`, 3 rounds each, identical canonical build options. Every run,
  report, median and hash is in `results/measurements.json`; medians always use all three rounds.
  Medians: large external wall 1,559 -> 1,013 ms; Bistro 35.6 -> 14.6 s. Sampled peak working set
  Bistro 1,684 -> 1,740 MiB, large 58 -> 135 MiB (the converter pool holds prepared primitives
  before the key-ordered commit). DAG results: large 246 cross-group repartitions / 345 groups;
  Bistro 231 converted / 23 profile-skipped (20 MASK, 3 BLEND) / 57 LOD0-only primitives,
  556 cross-group repartitions, 4,618 groups, rejection counters recorded. Payloads are
  byte-identical across threads and rounds for every asset (one SHA-256 per asset).
- The plan-specified `bistro-geometry.gltf` is recorded honestly: 1,844,243 B / 551 primitives, all
  carrying TANGENT (unmodeled attribute), so 0 eligible primitives and build exit 4. The closest
  real profile-matching asset `bistro-merged.gltf` (pristine derivation manifest recorded) is the
  real-asset arm; the attribute profile was not widened to force the specified file through.
- Measurement gate: `verify_subtilis_converter.py gate` requires every configured (threads, round)
  run, all eleven stage rows per run, finite/non-negative times, `accumulated == sum(exclusive)`,
  real cross-group DAG for the dag-required assets, one payload hash, and medians recomputed from
  every round. `gate --self-test` runs 11 hand-broken measurements and fails unless all are
  rejected; with the rule engine disabled 10/11 are wrongly accepted (RED witness), with it enabled
  11/11 are rejected (GREEN) and the real matrix passes with 0 findings.
- Khronos core validator: official release 2.0.0-dev.3.10 (win64) downloaded from the official
  GitHub release; archive SHA-256 `c5068f51...` and executable SHA-256 `4388a152...` pinned in
  `scripts/measure/khronos_validator_manifest.json` and re-verified by the verify script. It is a
  separate acceptance tool, not a CMake/vendored dependency. `gltf_validator --stdout` reports 0
  errors / 0 warnings on all three normal outputs and on all twelve export-cut previews; the
  unsupported `DEREN_subtilis` extension info is recorded (1 per normal output). In parallel
  `deren-subtilis validate --full` passes on all normal outputs (1, 1 and 231 payloads).
- LOD0/cut checks: source LOD0 indices and decoded POSITION/NORMAL/TEXCOORD_0 arrays are exactly
  preserved; nodes/scenes/materials are unchanged; cut previews at errors 0/0.01/0.1/1.0 keep the
  attribute/core semantics, remove the extension, keep every index in range, reproduce the exact
  oriented LOD0 triangle multiset at error 0, and are monotone non-increasing: Bistro
  4,209,006 -> 3,686,904 -> 3,332,400 -> 3,214,166 triangles. Standard GLB previews are archived;
  no automatic pixel comparison was run, so no pixel-quality claim is made.
- Independent CPU CI: `.github/workflows/subtilis-converter.yml` (separate from ci.yml/
  VR_TEST_TARGETS, path-filtered, no Vulkan/CUDA installs, no Bistro/network fixtures) configures
  the tool with Vulkan discovery disabled, asserts no Vulkan/CUDA target and exactly six test
  targets/tests, builds all targets, runs ctest, and repeats everything under ASan+UBSan with
  `halt_on_error` (no `detect_leaks`: unsupported on Windows, as ci.yml records). The local
  configure/target/`ctest -N` assertions were simulated; the workflow has not been executed on a
  GitHub runner in this session (no push).
- Validation of this finish pass: standalone Release CTest 6/6 (test_subtilis_dag_builder now
  4,705 checks incl. the new seam-sharing regression), standalone ASan/UBSan CTest 6/6. Root
  build files unchanged; root CTest 58/58 remains the relevant regression (Lead runs it).
- Archive: `D:/deren-assets/2026-10-21-subtilis-converter/` with 62 files / 1,343.8 MiB and a
  per-file `SHA256SUMS.json`; re-hashed after copy with 0 mismatches, `RECOVERY.md` included, local
  scratch deleted afterwards. Repository keeps the small `results/*.json` plus scripts and this
  report.

### Real-asset defect fixed during Task 8 (separate reviewable change)

The independent full validator (`make_asset`, `source/asset/subtilis/dag.cpp`) judged reduction
regions with one attribute-class graph (components + "pinched components"), while the design
(spec 3.4) and the builder use two graphs: position classes for geometric adjacency/boundary/
components and attribute classes for the seam identity. Real Bistro seam-sharing vertices are one
attribute class in two attribute-connected components, so the validator rejected the builder's
correct reductions: the full `bistro-merged` build failed with `reduction boundary or components
differ` (first failure primitive 0/2, round 0, group 10, fine 1,785 / coarse 891 indices). Fix:
`dag.cpp` now classifies position/attribute classes separately, validates degenerate/duplicate/
non-manifold edges in both graphs, checks "pinched components" and the component count only on the
geometric graph, and compares fine/coarse regions on both directed boundary multisets plus the
geometric component count. No builder, wire-format, identity or timing change. RED->GREEN: restored
original validator fails the new deterministic 64x64 UV-seam fixture (`FAIL ...: seam_built`, 4,699
checks/1 failed); fixed validator passes it (4,705 checks/0 failed, 26 groups, 11 rounds, 17
cross-group repartitions) and all 254 Bistro primitives now build+validate at threads 1/2/12 with
byte-identical payloads. Files: `source/asset/subtilis/dag.cpp` and
`source/tests/test_subtilis_dag_builder.cpp` (only). This must be reviewed/committed separately
from the Task 8 evidence documentation.
## Fresh-review disposition (review 2026-10-21, review report docs/reviews/2026-10-21-subtilis-converter-review.md)

Reviewed commit: `20e05cb9` (Task 8). The review found no Critical issues, one Important finding (I-1)
and minors M-1..M-9. I-1 was fixed in the follow-up worktree change described below; the minors are
recorded with the Lead's decisions and the reviewer's section-6 residuals are carried into this record.

### I-1 fixed: one GLB relocation-safety precondition for both GLB writers

Requirement: design spec line 168 and tools/subtilis/README ("unknown binary-reference semantics must
not be guessed; refuse that GLB output"). The rule used to run only on the non-self-contained/merging
path: `serialize_embedded_glb()` short-circuited self-contained GLB inputs to `make_glb()` with no
extension check, and `pack_glb()`'s allowlist was guarded by `!self_contained`. A self-contained GLB
with an unknown extension (even one whose URI became dangling after relocation) therefore converted
successfully while the same extension in a non-self-contained glTF correctly failed (review I-1).

Fix (smallest defensible shape, no wire/identity change): a single
`result<void> glb_relocation_safe(document const&)` in `tools/subtilis/resources.cpp` (declared in
`tools/subtilis/document_internal.hpp`) is now called from BOTH GLB writers - the top of
`serialize_embedded_glb()` (covers the self-contained fast path before it branches) and the top of
`pack_glb()` (the `!self_contained` guard is gone, so the two writers cannot disagree). It scans the
document DOM recursively (skipping arbitrary `extras`): every name in `extensionsUsed`/
`extensionsRequired` and every object under any `extensions` member must be in the existing known
relocatable allowlist (`DEREN_subtilis`, `KHR_mesh_quantization`, `KHR_texture_transform`,
`KHR_materials_unlit`, `KHR_lights_punctual`); unknown/invalid entries return a typed
`asset::error_code::io` error with the existing message ("unknown extension prevents safe GLB
relocation; use a glTF package"), which the CLI reports as exit 5. DOM-based scanning also hardens
M-1: a non-conformant extension object that is not listed in `extensionsUsed` no longer bypasses the
rule (strict decision, covered by a test).

RED evidence (pre-fix `20e05cb9` code, probes rebuilt from the archived known-extension
`outputs/small.glb` by editing only the JSON chunk):
- self-contained GLB + `extensionsUsed:["VENDOR_binary"]` + root `VENDOR_binary.uri="vendor-blob.bin"`,
  no images (fast `make_glb` path): CLI exit 0, 97,768-byte output written.
- same + data-URI PNG image (forces `pack_glb`): CLI exit 0, output written and still containing
  `{"VENDOR_binary":{"uri":"vendor-blob.bin"}}` relative to the new location (dangling after relocation).
- extension object present but not listed in `extensionsUsed` (M-1 non-conformant): CLI exit 0.
- non-self-contained glTF with the same extension: CLI exit 5 (the pre-existing correct arm).
- New regression tests against the pre-fix code: `test_subtilis_document` 190 checks / 2 failed
  (`CHECK(!refused)`, `CHECK(!image_refused)`); `test_subtilis_converter` 588 checks / 13 failed
  (self-contained GLB exit-5/absent-output expectations, typed-io assertions on both writers, and the
  unlisted-extension arm).

GREEN evidence (fixed worktree):
- same three probes: CLI exit 5 with "unknown extension prevents safe GLB relocation; use a glTF
  package"; no output file is created; a pre-existing target stays byte-identical with `--overwrite`;
  no staging leftovers.
- `--format gltf` package output on the same self-contained GLB still succeeds and preserves
  `extensionsUsed:["VENDOR_binary"]` and the extension object (package path untouched).
- both GLB writers report the same typed io error through the host API (`tool::write_package` and a
  direct `tool::pack_glb` call) for the self-contained fast, self-contained merging and non-self-contained
  arms; known extension `KHR_mesh_quantization` remains accepted.
- positive control known-extension cooked GLB (archived `outputs/small.glb`, sha256
  `451dcd9a13950045d47ace7c89fe2b7245961a65af9accfbd1966615976ec36d`): before and after the fix the
  conversion exits 0, reports `reused=1`/`converted=0`, the output is byte-identical
  (same `451dcd9a...` sha) and `deren-subtilis validate --full` passes. The real-asset matrix output is
  also unchanged (bistro-merged rebuild sha `b235aa40...40b47`, 247,474,732 bytes).
- tests GREEN: `test_subtilis_document` 192 checks / 0 failed (reviewed baseline 153; the new arms add
  39 checks), `test_subtilis_converter` 587 checks / 0 failed (reviewed baseline 518; the I-1 arms add
  69 checks). With the pre-fix code plus the new tests the same binaries reported document 190/2 failed
  and converter 588/13 failed (the RED run counts one extra check because an arm proceeds past a
  failing expectation before aborting).
- fixture bookkeeping needed by the new rule: `make_fixture()` now produces a fixture without unknown
  extensions by default (so existing GLB-output arms exercise the safe path), with
  `make_fixture(source, true)` for the preservation/refusal arms; the rich package-preservation fixture
  re-adds its own unknown optional extension explicitly, so the glTF-package preservation coverage is
  kept.

### Minors M-1..M-9 disposition (Lead decisions; not all implemented)

- M-1 (extension object not listed in `extensionsUsed` bypassed the allowlist) - HARDENED as part of
  the I-1 helper (DOM scan above; strict refusal test added). Not merely recorded.
- M-2 (`..`/parent URI reads) - explicit decision: accepted/documented (path traversal within the
  caller-readable filesystem; the tool is not a sandbox and input byte limits still apply). Not changed.
- M-3 (`publish_replace`/`publish_no_replace` dead helpers) - acknowledged dead code; removal not in
  this bounded fix. Not changed.
- M-4 (unused variable `canonical` in `scripts/measure/verify_subtilis_converter.py`) - FIXED (line
  removed); gate `--self-test` still 11/11.
- M-5 (`--help` omission in the CLI usage output) - acknowledged; not in scope. Not changed.
- M-6 (plan Task 8 bullet literally names bistro-geometry.gltf while the accepted real asset is
  bistro-merged.gltf) - recorded in the Task 8 evidence report (specified-input.json shows the
  TANGENT-ineligible result; real-asset.json documents the substitution); no code change.
- M-7 (DLL note) - acknowledged as recorded, non-blocking.
- M-8, M-9 - acknowledged non-blocking as recorded by the reviewer.

### Reviewer section-6 residuals carried into the acceptance record

6.1 Windows ASan has no LeakSanitizer (Linux LSan remains the follow-up; the sanitizer CI job documents
this). 6.2 Converter peak memory is a 20 ms-polled working-set lower bound, and meshoptimizer
temporaries are not provably bounded; Linux CI peak-RSS/LSan is still the right place to bound it. 6.3
The independent CPU CI workflow was never executed on a GitHub runner in this session (local-commit
rule); only its assertions were simulated locally. 6.4 The cross-volume staging test arm exists but
SKIPPED in this environment (no second writable volume for the test process); cross-volume behaviour is
reasoned-by-inspection, not measured. 6.5 `bistro-geometry.gltf` is TANGENT-ineligible (recorded, not
silently skipped). 6.6 Multiple simultaneous reduction failures report one of the failing tasks'
typed errors (failure-message determinism not asserted; success-path determinism is). 6.7 Measurement
medians/per-round data were independently recomputed by the reviewer and matched the archived JSONs.
6.8 The earlier seam-sharing validator fix's RED half was manually witnessed (test counts recorded);
the reviewer verified the GREEN from the committed tree. The reviewer also did not re-run the root
engine suite in this environment: root CTest 58/58 remains the unchanged regression and is the Lead's
final gate before committing.

### Review follow-up files changed

`tools/subtilis/document_internal.hpp`, `tools/subtilis/resources.cpp`, `tools/subtilis/document.cpp`,
`tools/subtilis/README.md` (claim now states both GLB input forms), `source/tests/subtilis_document_fixtures.hpp`,
`source/tests/test_subtilis_document.cpp`, `source/tests/test_subtilis_converter.cpp`,
`scripts/measure/verify_subtilis_converter.py` (M-4). No wire-format, identity, root engine/CMake or
`.github/workflows/ci.yml` change. `cmake --build build-subtilis` and `build-subtilis-asan` clean;
standalone Release CTest 6/6 (1.47 s) and ASan/UBSan 6/6 (25.21 s) green; `test_docs` 395 checks/0
failed. No commit from the implementer: the Lead reviews and commits.