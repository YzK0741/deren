# Subtilis offline converter

This standalone CPU tool builds the `DEREN_subtilis` shared-vertex cluster DAG.
The original glTF primitive remains the LOD0 fallback. It does not yet add a
renderer feature or require Vulkan/CUDA.

Build with CMake 4.3+, Ninja and a C++23 standard-library-module toolchain. For
the project's MSYS2 CLANG64 environment:

```powershell
$env:PATH = 'C:/msys64/clang64/bin;' + $env:PATH
cmake -S tools/subtilis -B build-subtilis -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=ON
cmake --build build-subtilis -j12
ctest --test-dir build-subtilis --output-on-failure
```

```powershell
./build-subtilis/deren-subtilis build input.glb -o cooked.glb --threads 12 --report build.json
./build-subtilis/deren-subtilis build input.glb -o reference.glb --threads 1
./build-subtilis/deren-subtilis inspect cooked.glb
./build-subtilis/deren-subtilis validate cooked.glb --full
./build-subtilis/deren-subtilis export-cut cooked.glb --error 0.01 -o preview.glb
```

`--threads N` accepts any positive integer. Omitted, it defaults to
`min(max(hardware_concurrency, 1), 12)`. The thread count is a single execution
budget for the whole build: it never enters the wire payload, the source/build
identities or any canonical build option, and production runs with different
thread counts are byte-identical.

Deterministic scheduling works on two levels and never nests two pools:

- Several eligible primitives: a bounded work pool over coarse primitive tasks
  (largest known-work estimate first), each inner build serial. Each task
  prepares into its own preallocated slot; only the scheduling thread appends
  the payload in primitive-key order.
- Exactly one primitive (or a single reduction group): the primitive gets the
  whole budget and parallelizes its per-round reduction groups. Candidates are
  prepared concurrently against immutable inputs, then validated and committed
  in partition order, so DAG relations and stable cluster/group/root IDs never
  depend on task completion order. A rejected candidate or failed task consumes
  no later visible ID; a failed prepare runs no partial cut (`--threads 1` keeps
  the serial reference interleaving).

Known-work budgeting covers the source records, owned payload/workspace and a
conservative per-task estimate. The parallel scheduler admits
`min(threads, tasks, budget / max task estimate)` tasks at once; shrinking
`execution_options::limits.max_known_work_bytes` lowers that planned concurrency
or rejects the whole primitive with a typed `capacity` error, without changing
the reductions of any asset that still succeeds. This known-work accounting is
not a provable process-memory bound: meshoptimizer's internal temporaries are
third-party allocations and are reported as such, not counted as exact peaks.

`export-cut` writes ordinary glTF indices for the selected model-space error
threshold and removes the consumed extension. Material, attributes and instances
keep their original identities. Exported files can be opened by standard GLB
viewers. Unreferenced old binary ranges are retained to preserve other IDs.

Current milestone: local glTF/GLB and base64 data URI input, GLB or glTF
directory-package output, deterministic bounded multi-core build. Local buffer
URIs support percent escaping. Known buffer references can merge into GLB;
PNG/JPEG resources can embed without changing their bytes. Unknown extensions or
buffer metadata that cannot relocate safely cause an explicit error for GLB and
are preserved by the glTF package.

`--format glb` (the default) keeps the embedded single-file output: `-o
cooked.glb` writes one GLB. `--format gltf` writes a relocatable package
directory: `-o cooked.gltf` creates `cooked/` containing `cooked.gltf` plus its
resources (`cooked.bin` for the embedded/appended buffer, `bufferN.bin` for each
original external buffer, `imageN.png`/`imageN.jpeg` for external PNG/JPEG
images, all referenced by percent-encoded relative URIs). The package stages on
the destination volume and is published atomically: an existing directory or
file is refused unless `--overwrite`, and a failed publication leaves the source
and any previous package untouched.

An existing payload is fully validated and reused when its source/build identity
matches. `reused` counts these primitives; the payload is not appended again.
Mismatching or damaged payloads require rebuilding from the original source.

Ineligible primitives are preserved and counted as skipped; `--strict` rejects
any skip. Zero eligible primitives is an error. A valid asset without accepted
reductions succeeds with `lod0Only` in its report; this does not claim a geometry
reduction. Reports include the requested thread count, per-frontier counts,
rounds, cross-group repartitions, known work estimates, planned concurrency and
wall time. Estimates are not process peak memory.

Each converted primitive also carries structural stage profiling with fixed rows
`sourceFingerprint`, `leafCluster`, `topology`, `partition`, `boundary`,
`simplify`, `recluster`, `candidate` (the only inclusive row), `commit`,
`validate` and `encode`. Per-stage `cpuMs` is accumulated worker-task elapsed
time, not CPU cycles: parallel tasks can add up to more than `wallMs`, which is
why the two are reported separately. `accumulatedMs` sums only the exclusive
rows, so nested/inclusive rows are not counted twice. Stage rows are structural
evidence (which phases ran and how many tasks) and are not a speed assertion or
a benchmark result. Reused payloads ran no build and report an empty `stages`
array. A top-level `knownWorkNote` states the third-party-memory boundary.
Reports/counters never enter the wire payload or asset identity.

Exit codes: 0 success, 2 command usage, 3 invalid input/validation, 4 construction
or eligibility failure, 5 output failure. Existing asset/report files are refused.
Pass `--overwrite` to replace an existing output/report atomically. Source-file
overwrites are always refused, including equivalent paths. Asset publication uses
a temporary file on the destination volume and validates it before publishing;
the optional report is published afterwards.

The host API exposes `convert_file` with typed options/report and
`write_package`; for `output_format::gltf` the `write_package` target is the
package directory (the CLI maps `-o out.gltf` to `out/`). Package resources are
written through a small publication seam so the test target can inject short
writes, close failures and final rename failures; production entry points never
inject.

The wire format and canonical identity are documented in
[`docs/extensions/DEREN_subtilis`](../../docs/extensions/DEREN_subtilis/README.md).

## Measurement, validation and CI

Real-asset measurement, independent validation and cut/LOD0 acceptance are reproducible with the
scripts in [`scripts/measure`](../../scripts/measure/README.md):

```powershell
python scripts/measure/make_subtilis_converter_fixtures.py --out-dir .subtilis-work/fixtures
python scripts/measure/measure_subtilis_converter.py --converter build-subtilis/deren-subtilis.exe `
  --out-dir .subtilis-work/measure `
  --asset small=.subtilis-work/fixtures/small.glb `
  --asset large=.subtilis-work/fixtures/large.glb `
  --asset bistro=D:/deren-assets/bistro/bistro-merged.gltf `
  --dag-required large --dag-required bistro --threads 1,12 --rounds 3
python scripts/measure/verify_subtilis_converter.py gate `
  --measurements .subtilis-work/measure/measurements.json --out .subtilis-work/measure/gate.json
python scripts/measure/verify_subtilis_converter.py check `
  --measurements .subtilis-work/measure/measurements.json `
  --converter build-subtilis/deren-subtilis.exe `
  --validator D:/deren-assets/gltf-tools-khronos/2.0.0-dev.3.10/gltf_validator.exe `
  --validator-manifest scripts/measure/khronos_validator_manifest.json `
  --work-dir .subtilis-work/acceptance/previews `
  --out .subtilis-work/acceptance/acceptance.json
```

`measure` runs the asset × thread-budget × round matrix and records every round plus medians;
`verify gate` fails on incomplete stage/round data, missing real cross-group DAG, payloads that are
not byte-identical, or medians not derived from all rounds (`--self-test` demonstrates the negative
controls). `verify check` runs the official pinned Khronos core validator (2.0.0-dev.3.10, hashed
in `scripts/measure/khronos_validator_manifest.json`; separate acceptance tool, never part of the
CMake build or vendored) and `deren-subtilis validate --full` on normal outputs and export-cut
previews, plus LOD0/cut geometry identity checks. Acceptance evidence, per-round numbers,
limitations and the out-of-tree artifact archive are recorded in
[docs/reviews/2026-10-21-subtilis-converter.md](../../docs/reviews/2026-10-21-subtilis-converter.md).

Independent CI for this tool is `.github/workflows/subtilis-converter.yml`: it is deliberately
separate from the engine `ci.yml`/`VR_TEST_TARGETS` contract and needs no Vulkan/CUDA install, no
Bistro download and no network asset; both a Release build+tests job and an ASan+UBSan Debug job
run the six standalone test targets.
