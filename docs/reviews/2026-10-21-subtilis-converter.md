# subtilis glTF DAG converter - Task 8 acceptance evidence (2026-10-21)

Plan: [CPU glTF DAG converter](../superpowers/plans/2026-10-21-subtilis-gltf.md) Task 8.
Design: [subtilis glTF/CPU converter design](../superpowers/specs/2026-10-21-subtilis-gltf-design.md).
Task 1-7 history: [converter checkpoint](2026-10-21-subtilis-converter-checkpoint.md).
Tool usage: [tools/subtilis/README.md](../../tools/subtilis/README.md).

Status: Task 8 evidence complete. The conversion path and the independent full validator were
also fixed for a real-asset correctness defect found by this task (section 3); that fix is in
`source/asset/subtilis/dag.cpp` plus a regression test, separately reviewable from this document.
A fresh whole-change reviewer (plan Task 8 bullet 6) is still pending and is the Lead's step, not
part of this evidence.

## 1. What was run

Everything below ran on the Developer machine: Windows 11, MSYS2 clang64 (clang 23.1.3, libc++,
CMake 4.4.4, Ninja), 16 logical CPUs, Python 3.14.7 + NumPy for the measurement scripts. The
converter binary for the matrix is `build-subtilis/deren-subtilis.exe`, 1,698,304 bytes,
SHA-256 `fa975534352852cbb66e1d1ce0525cfb17d6a8a5d0bfdf9f12f2f10a581233e4` (Release,
`cmake --build build-subtilis -j 12`).

Matrix (all with identical converter defaults, only `--threads` differs):

| Asset | Source | Size | Geometry | Why this asset |
| --- | --- | --- | --- | --- |
| small | generated `small.glb` | 60,320 B | 2,048 triangles / 1,089 vertices | fast smoke fixture (one primitive) |
| large | generated `large.glb` | 3,687,348 B | 131,072 triangles / 66,049 vertices | synthetic large grid: 15 rounds, 246 cross-group repartitions |
| bistro | `D:/deren-assets/bistro/bistro-merged.gltf` + 180,854,920 B `bistro-merged.bin` | JSON 289,200 B | 4,209,006 triangles / 254 primitives | real Bistro geometry, material-merged, geometry-only |

The plan-specified real asset `D:/deren-assets/bistro/bistro-geometry.gltf` exists (1,844,243 B,
551 primitives, 1,753,630 triangles) but every one of its primitives carries TANGENT. The
converter profile models POSITION/NORMAL/TEXCOORD_0 only, so all 551 primitives are ineligible and
`deren-subtilis build` exits 4 with "no eligible targets, or --strict rejected skipped primitives"
(recorded in `results/specified-input.json`). That is profile-correct behaviour, not a silent skip;
the attribute profile was deliberately not widened for this task. The closest real asset that
already matches the profile is `bistro-merged.gltf` (pristine derivation recorded in
`results/real-asset.json`: from `bistro.gltf` SHA-256 `c0972d6c...`, 2,909 upstream draws merged to
254 material primitives, geometry only). Its 23 non-OPAQUE primitives (20 MASK, 3 BLEND) stay
skipped by the profile; 231 primitives / 3,256,779 work triangles are converted.

Per asset: `--threads 1` and `--threads 12`, 3 independent rounds each (`rounds=3`), all 18 runs
recorded in `results/measurements.json`; medians are computed from all three rounds, never
best-only. The scripts are `scripts/measure/make_subtilis_converter_fixtures.py`,
`measure_subtilis_converter.py` and `verify_subtilis_converter.py`; the gate rejects missing
rounds, missing stage rows, missing cross-group DAG, medians not derived from every round, and
non-identical payloads (`verify_subtilis_converter.py gate --self-test` demonstrates the 11
negative controls; with the rule engine disabled 10 of them are accepted, i.e. the harness fails
closed).

## 2. Timing, memory and DAG results (medians of 3 rounds)

External wall is the measured process wall (startup + document parse + build + package write);
`reportWallMs` is the sum of the per-primitive builder walls inside the report; `accumulatedMs` is
the sum of the exclusive stage worker-task times (thread time, NOT CPU cycles) and is reported
separately by design.

| Asset | threads | external wall median | per-round external wall (ms) | reportWallMs median | accumulatedMs median | sampled peak working set median | output bytes |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| small | 1 | 50.1 ms | 295.8, 50.0, 50.1 | 9.3 ms | 8.6 ms | 8.3 MiB | 97,712 |
| small | 12 | 49.4 ms | 49.3, 50.5, 49.4 | 8.9 ms | 10.0 ms | 11.0 MiB | 97,712 |
| large | 1 | 1,559.1 ms | 1,576.8, 1,559.1, 1,519.0 | 1,101.3 ms | 994.7 ms | 57.9 MiB | 6,696,416 |
| large | 12 | 1,013.4 ms | 988.7, 1,013.4, 1,037.4 | 565.9 ms | 1,493.1 ms | 134.7 MiB | 6,696,416 |
| bistro | 1 | 35,586.7 ms | 35,586.7, 34,054.0, 36,433.3 | 22,391.9 ms | 20,755.1 ms | 1,683.7 MiB | 247,474,732 |
| bistro | 12 | 14,598.5 ms | 14,598.5, 14,281.3, 15,082.7 | 34,021.1 ms | 31,500.5 ms | 1,740.1 MiB | 247,474,732 |

Honest readings:

- The first `small --threads 1` run is a cold-start outlier (295.8 ms; later runs 50 ms). It is
  kept in the record and does not move the median; no round was dropped.
- End-to-end external wall improves 1.5x on the synthetic large grid (1,559 -> 1,013 ms) and
  about 2.4x on Bistro (35.6 -> 14.6 s). These are measured end-to-end numbers on this machine
  only; they include document IO and output writes and are not a claim about other hardware or
  about GPU/CUDA (no CUDA path exists or was measured).
- `accumulatedMs` rising on the parallel arms (large 995 -> 1,493 ms; Bistro 20.8 -> 31.5 s) is
  expected worker-time inflation under contention: it is accumulated task time, not a parallel
  wall clock, and is exactly why the report keeps it separate from `reportWallMs`/external wall.
  No speed threshold is asserted anywhere.
- The converter-level pool holds prepared primitive results before committing them in key order,
  so the 12-thread arm has a higher sampled peak (Bistro +56 MiB, large +77 MiB). The sampled
  peak is a 20 ms-polling lower bound, not a process-memory cap; a Linux CI run with LSan/peak
  RSS accounting is still the right place to bound this.

Structural DAG results (median per run, identical across rounds and thread counts):

| Asset | converted | skipped | LOD0-only primitives | max rounds | cross-group repartitions | final groups | payload SHA-256 (1/2/12/rounds identical) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| small | 1 | 0 | 0 | 3 | 1 | 3 | `451dcd9a...6ec36d` |
| large | 1 | 0 | 0 | 15 | 246 | 345 | `b4e6946b...b48717` |
| bistro | 231 | 23 | 57 | 10 | 556 | 4,618 | `b235aa40...40b47` |

Rejection reasons accumulated over the six runs per asset (the Task 8 support change made the CLI
report JSON expose each primitive's nonzero `rejected` counters - an external-report-only change, no
payload/identity impact; the payload build metadata already carried the same structural counters):

| Asset | noProgress | unsafeTopology | numericError | boundaryChanged |
| --- | ---: | ---: | ---: | ---: |
| small | 0 | 0 | 0 | 30 |
| large | 0 | 1,878 | 0 | 3,294 |
| bistro | 32,964 | 33,270 | 66 | 15,522 |

## 3. Real-asset defect found and fixed (separate from the Task 8 evidence)

**Symptom.** The first full `bistro-merged` build (all Tasks 1-7 committed) exited 4 with
`deren-subtilis: reduction boundary or components differ`. The failure was raised by the
independent full validator (`make_asset`, `source/asset/subtilis/dag.cpp:241`) after the builder
had accepted a reduction; per-primitive probing showed the first failure on primitive 0/2,
round 0, group 10 (fine region 1,785 indices, coarse 891 indices) and many more primitives
affected.

**Root cause.** The full validator judged a reduction region with a single graph built from
attribute classes (position+normal+UV), using it for the geometric boundary, the component count
and the "pinched components" rule. The builder - and design spec section 3.4 ("position
equivalence is used to discover geometric adjacency; it does not weld the actual UV/normal
vertices") - uses two graphs: position classes define geometric boundary/components, attribute
classes define the seam identity of the attribute boundary. On real Bistro seam geometry a
seam-sharing vertex is one attribute class that appears in two attribute-connected components
(legitimate: the two sides of a UV seam that reuse a shared vertex), so the attribute-graph rule
rejected both the fine and the coarse region as "pinched" even though the builder's two-graph
boundary signature was valid and equal. The builder was right; the independent validator rule was
wrong.

**Fix.** `dag.cpp` now classifies position and attribute classes separately
(`classify_vertices`), builds two independent boundary graphs (`analyze_class_graph`), checks
degenerate/duplicate/non-manifold edges in both, checks `pinched reduction components` and the
component count only on the geometric (position-class) graph, and compares the fine/coarse regions
on both directed boundary multisets plus the geometric component count. The region equality
message is unchanged (`reduction boundary or components differ`). No builder, wire-format,
identity or timing change.

**RED -> GREEN evidence.**

- RED (original validator restored from git, fixed code set aside):
  `test_subtilis_dag_builder` fails with `FAIL ...: seam_built` (4699 checks, 1 failed), because
  `build_serial` on the new deterministic seam-sharing fixture fails with
  `reduction boundary or components differ`.
- GREEN (fixed validator): same test passes, 4705 checks, 0 failed, and reports
  `seam-sharing full-validation regression: 26 groups, 11 rounds, 17 cross-group repartitions,
  23 rejects`.
- Regression fixture: a 64x64 grid with a UV seam down the middle whose interior seam vertices are
  duplicated with shifted U while the two endpoint rows share one vertex; this is the minimal
  synthetic reproduction of the attribute-pinch-in-two-graphs condition. It is generated in
  `source/tests/test_subtilis_dag_builder.cpp` (no binary asset needed, part of `test_subtilis
  _dag_builder`).
- Real asset after the fix: all 254 `bistro-merged` primitives pass full validation; the complete
  asset builds at `--threads 1`, 2 and 12 with byte-identical payloads
  (`b235aa40...40b47`, 247,474,732 B) and is part of the matrix above. Before the fix the
  full-asset build produced no output at all.

Independent validation of the fixed path is in section 5; the builder's serial and parallel paths
were already byte-identical (Task 7) and the full validator now agrees.

## 4. Stage profile / CPU hotspots

Per-stage medians of accumulated worker-task time (ms), `--threads 1` / `--threads 12`. The
`candidate` row is inclusive (it contains boundary+simplify+recluster); it is excluded from
`accumulatedMs`, so nothing is double-counted. `partition`/`candidate`/`commit` task counts equal
the per-round reduction tasks; `leafCluster`/`topology`/`validate`/`encode` run once per converted
primitive and are complete on every successful build (the gate enforces all eleven rows).

| stage (inclusive?) | small t1 / t12 | large t1 / t12 | bistro t1 / t12 |
| --- | ---: | ---: | ---: |
| sourceFingerprint | 0.4 / 0.4 | 23.2 / 22.7 | 989.0 / 1,466.3 |
| leafCluster | 1.2 / 1.2 | 86.6 / 92.9 | 2,568.9 / 3,956.0 |
| topology | 0.9 / 0.8 | 54.5 / 51.8 | 2,212.5 / 3,315.6 |
| partition | 0.6 / 0.6 | 119.9 / 121.9 | 1,982.3 / 3,040.8 |
| boundary | 2.7 / 3.5 | 388.2 / 742.4 | 6,679.7 / 9,975.7 |
| simplify | 1.0 / 1.3 | 119.6 / 211.4 | 1,719.6 / 2,793.0 |
| recluster | 0.3 / 0.4 | 44.1 / 74.5 | 788.3 / 1,184.2 |
| candidate (inclusive) | 4.6 / 7.5 | 656.8 / 1,203.3 | 10,787.3 / 16,368.4 |
| commit | 0.0 / 0.0 | 1.0 / 1.5 | 17.5 / 41.8 |
| validate | 1.5 / 1.6 | 153.6 / 153.3 | 3,794.8 / 5,575.7 |
| encode | 0.0 / 0.0 | 0.9 / 0.9 | 17.7 / 27.6 |

Hot spot: candidate preparation dominates (boundary re-analysis is the largest exclusive
component, then simplify/recluster), followed by full validation and leaf clustering. This matches
the Task 8 measurement goal; it is a profile, not a benchmark claim, and no optimisation was
attempted in this task. The report does not instrument document parse/normalize or the package
write as stages: for Bistro the difference between external wall (35.6 s) and the per-primitive
report-wall sum (22.4 s) is dominated by those uninstrumented IO paths and is not attributed to a
stage.

## 5. Khronos core validator (pinned, tool-only)

The official glTF-Validator release `2.0.0-dev.3.10` (win64) was downloaded from the official
GitHub release, the archive SHA-256 was recorded and the executable SHA-256 verified before use:
archive `c5068f51205deedc28acc3529ee7e11ee60e853454f673093398eba80142202c`,
`gltf_validator.exe` `4388a152ff90b68c6430ae03862e05e257a9d50a500ed7d0eb1cd420dc75ff96`.
The pin lives in `scripts/measure/khronos_validator_manifest.json`; the tool stays a separate
acceptance dependency (not in any CMake target, not vendored, not part of the engine build). The
verify script re-hashes it each run and fails on a mismatch.

`gltf_validator --stdout <file>` was run on the normal converter output and on every export-cut
preview; `issues.numErrors` must be 0 and is never ignored. Own-extension noise is recorded
separately: the validator cannot validate `DEREN_subtilis` and reports one `UNSUPPORTED_EXTENSION`
info for the normal GLB (plus unrelated infos/hints); cut previews have the extension removed by
design.

| File | Khronos errors | warnings | infos | hints | DEREN unsupported info | `deren-subtilis validate --full` |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| small normal | 0 | 0 | 3 | 4 | 1 | exit 0, "validated 1 payload(s), full=true" |
| large normal | 0 | 0 | 3 | 4 | 1 | exit 0, "validated 1 payload(s), full=true" |
| bistro normal | 0 | 0 | 486 | 1,016 | 1 | exit 0, "validated 231 payload(s), full=true" |
| small cuts (0/0.01/0.1/1.0) | 0 each | 0 | small | - | 0 | exit 3 "no subtilis payload" (expected: cut output intentionally has none) |
| large cuts (0/0.01/0.1/1.0) | 0 each | 0 | small | - | 0 | exit 3 (expected by design) |
| bistro cuts (0/0.01/0.1/1.0) | 0 each | 0 | small | - | 0 | exit 3 (expected by design) |

Both validators therefore pass on the normal outputs (core errors 0 and full DAG validation
green). Export-cut previews are core-validator clean; they are not fed to
`deren-subtilis validate --full` because removing the extension is the documented export
behaviour - the script records the observed "no payload" exit code and its meaning instead of
pretending it is a failure or a pass.

## 6. LOD0 and cut checks

For every asset independently (`verify_subtilis_converter.py check`, results in
`results/acceptance.json`, previews in the archive):

- Normal output LOD0 identity: for all 254 Bistro primitives (and the 2 fixture primitives) the
  converter keeps the source primitive's index list byte-for-byte and the decoded POSITION /
  NORMAL / TEXCOORD_0 arrays exactly; node list, scene list/index and material list are unchanged
  from the source document. LOD0 triangle multiset and winding therefore match the source exactly.
- Cut previews at model-space errors 0, 0.01, 0.1, 1.0: all have the source attribute data and
  core DOM semantics; every index is inside the primitive's vertex range; no DEREN extension is
  left; and at threshold 0 the oriented triangle multiset equals the source LOD0 multiset
  (cluster-order-independent comparison, winding preserved).
- Cut triangle counts are monotone non-increasing with a larger error threshold:

| Asset | source/LOD0 | 0.01 | 0.1 | 1.0 |
| --- | ---: | ---: | ---: | ---: |
| small | 2,048 | 1,212 | 1,212 | 1,212 |
| large | 131,072 | 22,178 | 22,178 | 22,178 |
| bistro | 4,209,006 | 3,686,904 | 3,332,400 | 3,214,166 |

- Preview GLBs (standard GLB, ordinary indices, no custom extension) are archived for direct
  manual viewing: `previews/*-cut-{0,0.01,0.1,1}.glb`. Scene/outline/material-seam visual quality
  was NOT algorithmically compared here (no image-comparison tool was run), so this document makes
  no pixel-quality claim for simplified LODs; the source LOD0 identity above is the exact part.

## 7. Independent CPU-tool CI

New `.github/workflows/subtilis-converter.yml` (deliberately separate from `ci.yml`/
`VR_TEST_TARGETS`, so the engine CI contract and `test_docs` are untouched):

- `release-tests`: MSYS2 clang64, configure `tools/subtilis` Release with Vulkan package discovery
  explicitly disabled, assert the generated target list has no Vulkan/CUDA target, build all
  targets, assert the six test executables were produced and exactly six tests are registered,
  run `ctest`.
- `sanitizers`: the same tests in a Debug ASan+UBSan tree with the repo's measured
  `halt_on_error` settings; `detect_leaks` is intentionally not set because LeakSanitizer does not
  exist on Windows (the same measured limitation `ci.yml` records for the engine sanitizer job).
- No Vulkan/CUDA packages, no Bistro download, no external validator download: the job is the
  tool's own small generated fixtures. It is path-filtered to the tool and its dependencies.
- The workflow YAML parses and its assertions were simulated against the local configure/target
  lists (`ninja -t targets all` has no vulkan/cuda entry; `ctest -N` registers 6 tests). It has
  NOT been executed on a GitHub runner in this session (no push per the plan's local-commit rule),
  so its first real run status is not claimed here.

## 8. Limitations / residual risks

1. Windows ASan has no LeakSanitizer, so leak coverage remains a Linux-CI follow-up; the
   sanitizer job documents this instead of silently claiming leak coverage.
2. The converter-level primitive pool holds prepared results before the key-ordered commit, so
   peak host memory on the parallel arm is higher (Bistro sampled +56 MiB). The sampled working
   set is a lower bound; strict peak/leak accounting needs the Linux CI job.
3. External wall includes uninstrumented IO (document parse and package write are not report stages);
   the CPU-accumulated numbers in section 4 are not a wall-clock breakdown.
4. The plan-specified `bistro-geometry.gltf` is not convertible under the frozen profile
   (unmodeled TANGENT); the real-asset numbers are for the documented material-merged,
   geometry-only Bistro variant, with the 23 MASK/BLEND primitives explicitly skipped.
5. A large primitive among many still gets one thread (primitive-level scheduling priority, Task 7
   design); nested budget partitioning remains future work.
6. Parallel speedups are measured end-to-end on this 16-CPU machine only; no GPU/CUDA path was
   added, measured or claimed. The synthetic/large and Bistro numbers are not a quality claim -
   the DAG structural counters and the LOD0 identity checks are the quality evidence.
7. `test_docs` (root engine test) only scans `docs/*.md`, so this report under
   `docs/reviews/` is not part of the manual; the manual entry `docs/subtilis.md` and the root
   README link here instead.

## 9. Archive and reproduction

Heavy artifacts (1,343.8 MiB, 62 files) are archived outside the repository at
`D:/deren-assets/2026-10-21-subtilis-converter/` with `SHA256SUMS.json` (per-file size + SHA-256,
re-hashed after copy: 0 mismatches) and `RECOVERY.md`. Contents: fixtures, canonical normal
outputs, all 18 build reports and logs, all 12 cut preview GLBs, and the full measurement/
acceptance JSONs. The workspace scratch used while producing them was deleted after the archive
hashes were verified. The repository keeps the small result JSONs in
`docs/reviews/2026-10-21-subtilis-converter/` and the scripts under `scripts/measure/`.

Reproduce (from the repository root, Release tree built):

```powershell
python scripts/measure/make_subtilis_converter_fixtures.py --out-dir .subtilis-work/fixtures
python scripts/measure/measure_subtilis_converter.py --converter build-subtilis/deren-subtilis.exe \
  --out-dir .subtilis-work/measure \
  --asset small=.subtilis-work/fixtures/small.glb \
  --asset large=.subtilis-work/fixtures/large.glb \
  --asset bistro=D:/deren-assets/bistro/bistro-merged.gltf \
  --dag-required large --dag-required bistro --threads 1,12 --rounds 3
python scripts/measure/verify_subtilis_converter.py gate \
  --measurements .subtilis-work/measure/measurements.json --out .subtilis-work/measure/gate.json
python scripts/measure/verify_subtilis_converter.py check \
  --measurements .subtilis-work/measure/measurements.json \
  --converter build-subtilis/deren-subtilis.exe \
  --validator D:/deren-assets/gltf-tools-khronos/2.0.0-dev.3.10/gltf_validator.exe \
  --validator-manifest scripts/measure/khronos_validator_manifest.json \
  --work-dir .subtilis-work/acceptance/previews \
  --out .subtilis-work/acceptance/acceptance.json
```

## 10. Status changes in this task

- `docs/superpowers/plans/2026-10-21-subtilis-gltf.md`: Task 8 measurement, real-asset, validator,
  cut/LOD0, CI and evidence/archive checkboxes are marked with the evidence above; the fresh
  whole-change reviewer checkbox stays open for the Lead's separate reviewer.
- `docs/superpowers/specs/2026-10-21-subtilis-gltf-design.md`: status line updated from "not
  implemented" to "implemented; Task 8 evidence recorded"; no requirement changed.
- `docs/subtilis.md` and `README.md`: tool status/entry lines point to this report (the runtime
  feature documentation itself is unchanged).
- `tools/subtilis/README.md` and `scripts/measure/README.md`: measurement/verify/validator usage
  added.
- The converter full-validator defect fix is listed separately in the checkpoint and ledger so the
  Lead can review/commit it apart from this evidence documentation.
