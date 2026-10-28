# Deferred operation stream CPU optimization - 2026-10-27

The common ABI 53 operation recorder had avoidable CPU work on ordinary and composed paths.
This change reduces that work without changing feature gates, shaders, texture budgets, geometry
selection or the RHI vtable ABI. It belongs to shared command recording, not to amnis or subtilis policy.

## Findings and changes

1. Every nonempty apply simplified the pending stream, deep-copied the selected operations into a
   second stream, and simplified again. Each apply also allocated several pointer/range work vectors.
   The recorder now reuses per-command-list selection buffers and borrows immutable records from their
   owning streams until replay ends. Interop restoration selects across both source streams without
   copying their payloads. Ordinary applies use the already simplified pending batch directly.
2. Saving logical state first copied every state setter, including thousands of overwritten push
   updates, and then compacted it. State is now selected before copying, so the saved snapshot owns
   only the surviving records and their push data/group references.
3. Push validation appended every written range and scanned the growing history at subsequent work
   operations. Repeated updates could cause quadratic validation work. The recorder now maintains the
   exact sorted union of written bytes. Overlap checks use this union; validation is not disabled.
4. Push liveness uses an inline word-coverage bitmap for common ranges, with the original sparse-set
   behavior retained for large offsets. A warmed selection buffer allocates nothing for common ranges.

All unique draw/dispatch/trace operations retain their order. The recorder still validates the whole
selected batch before lazy materialization and native replay, latches errors, and owns the canonical
state needed after native interop. No previous-camera or static-scene reuse was added.

## Diagnostic attribution

Opt-in `DEREN_RENDER_OPERATION_PROFILE=1` / `--operation-profile` records prepare, validation,
lazy demand, native replay and state saving. Ordinary runs do not read clocks or emit profile logs.
The following checkpoint is the cumulative mean at the same 12,288 nonempty/restore applies in the
180-frame fixed textured-DAG + HZB eager-texture diagnostic. It includes initialization, is not a
steady per-frame cost, and is not the same call-count definition as a timer around every API entry.

| Phase | Before, us/apply | Selection/copy fix only | Final, including range union |
|---|---:|---:|---:|
| Prepare | 45.063 | 7.251 | 6.515 |
| Validate | 36.118 | 35.272 | 2.401 |
| Lazy demand | 1.922 | 1.510 | 1.296 |
| Native replay | 10.246 | 10.441 | 8.813 |
| Save state | 27.653 | 6.598 | 5.259 |
| Sum | 121.002 | 61.072 | 24.284 |

The staged result identifies two separate costs: copying/compaction, followed by repeated range
validation. Diagnostic frame means were 22.23 -> 16.17 -> 10.12 ms, with CPU post dropping from
about 19 ms to 7-8 ms. These diagnostic times are excluded from the formal performance table.
Native timing variation is not evidence of GPU shader optimization.

## Controlled before/after results

Release Clang, RTX 4060 Laptop GPU / NVIDIA 617.42, 1280x720, fixed animation, vsync/GUI/shadow/
TAA/SSAO/bloom/FXAA off. The ordinary paths use Bistro MR; the composed paths use the same archived
textured-DAG Bistro as the composed benchmark, with 1,873 adopted instances. Within each profile,
before and after use the identical asset, configuration and shader bundle.

Two rounds run each before/after pair in reversed binary order; the second round also reverses profile
order. Default PBR requests 180 frames; other profiles request 360. Discard the first 60 presentation
intervals, then pool both rounds (242 default intervals, 602 for each other profile). P95 is computed
from pooled samples. CPU figures average the 60-frame phase windows after the first window, covering
the corresponding normal rendering interval. CPU active is logged total minus pace, not CPU utilization.
These are wall-clock phase/interval measurements, not isolated GPU timestamps.

All timing runs disable profiling, validation and resource sampling. No concurrent build/test/GPU
work from this task runs during a capture. GPU clocks and temperature are not locked, so small deltas
are not reliable improvement claims.

| Path | CPU post before / after, ms | Frame mean before / after, ms | P95 before / after, ms |
|---|---:|---:|---:|
| Default PBR, visibility off, eager textures | 5.397 / **2.295** | 133.450 / 133.388 | 139.241 / 138.850 |
| Ordinary visibility, no subtilis or streaming | 10.373 / **4.901** | 21.104 / 20.351 | 23.904 / 21.670 |
| subtilis DAG + HZB + amnis | 22.419 / **11.170** | 25.625 / **14.381** | 34.732 / **17.177** |
| Same + experimental triangle software/GPU routing | 21.544 / **10.959** | 24.601 / **13.966** | 30.395 / **15.655** |
| subtilis DAG + HZB + amnis, 2deg/frame sweep | 20.004 / **9.920** | 23.320 / **12.961** | 27.436 / **14.836** |

Default CPU post falls **57.5%**, but this particular default-PBR Bistro workload remains GPU-bound
at about 133 ms, so its FPS is effectively unchanged. Ordinary visibility CPU post falls **52.8%**;
its small whole-frame delta should not be generalized. On the CPU-limited composed path, mean frame
time falls **43.9%** (about 39.0 -> 69.5 FPS). The moving composed path falls **44.4%**
(about 42.9 -> 77.2 FPS). Software remains experimental/default-off; the small difference between
the final composed and software profiles does not establish a stable software-raster win.

Round means are retained in the CSV. For example, composed before is 23.081 / 28.170 ms and after
is 13.944 / 14.817 ms. Both rounds improve substantially, but the before spread shows why these are
local measurements, not a fixed percentage guarantee. Progressive runs include asynchronous upgrades
after the first 60 frames; they are not a fully converged-only rendering benchmark.

## Images and remaining limits

- Default PBR before/after is RGB pixel-exact in both rounds, with zero changed pixels.
- Ordinary visibility is exact in round 1. Round 2 has 14,809 changed pixels, RGB MAE 0.008538/255,
  maximum channel difference 30. The **same before binary's two rounds also differ by exactly these
  14,809 pixels**, while the after binary's two rounds match. The source of this pre-existing
  repeatability issue has not been diagnosed in this CPU-cost task.
- Composed and software fixed pairs have 14,823 changed pixels, RGB MAE 0.008548/255 and maximum
  error 30. Moving pairs have 14,442 changed pixels, RGB MAE 0.008528/255 and maximum error 35.
  Similar-sized differences were recorded in earlier texture comparisons, but this does not establish
  their cause in every pair. These paths are not claimed pixel-exact before/after.
- Only final screenshots were compared. This is not full-trajectory quality validation. API semantic
  tests and actual GPU readbacks provide additional evidence for state, ordering and restoration.
- The change retains some scratch capacity per command list to reduce allocation churn. It does not
  address CPU source-texture duplication or resource residency budgets.

## Verification

- Full Release build passed. Full CTest **73/73** passed after the production changes.
- Operation-stream tests first failed for missing reusable-selection APIs and the range-union helper;
  final **131,360 checks** pass. Coverage includes common-range zero-allocation through allocation-failure
  injection, large-offset/partial-overlap coverage, borrowed multi-stream selection, preserved consumer
  observations on generated streams, and 512 random writes checked against a byte-level union oracle.
- Vulkan probes pass: resource groups **358**, texture groups **116**, submission completion **75**,
  frame graph **191** checks. The resource-group probe includes raw native push clobber followed by
  logical state restoration, pipeline switching and actual readback.
- Resource-group probes also pass all **358 checks** with simplification disabled. Its old test assumed
  a dead setter must leave a lazy buffer unloaded even in that developer control; both old/new binaries
  failed that assumption. The fixture now expects both buffers to materialize when dead setters are
  deliberately retained, while checking readback and other behavior in both modes.
- Another pre-existing device assertion still expected a 2048-slot grid after the progressive bank
  extended it to 3072. It failed on the before binary too; the fixture now reflects the current grid.
- Validation-enabled 64-frame composed + experimental software capture exits zero with no reported
  VUID/backend error and passes all requested activation gates. All twenty formal captures also exit
  zero, produce one screenshot and pass activity checks; validation is disabled in formal captures.
- Independent source review found no specific new correctness/lifetime issue in borrowed selections,
  union coverage, work ordering, failure handling, interop restoration or the developer control.

## Reproduction and evidence

The before and after snapshots are `tmp/operation-before-bin/` and `tmp/operation-after-bin/`.
Each arm has one binary identity across its timing captures; both arms share one shader identity.
Before source base is `8849f822` with only the opt-in diagnostic hook. The final arm contains the
CPU changes described here; no commit, merge or push was requested or performed for this task.

```powershell
# Default-path capture; run with each executable and reverse their order in round 2.
python scripts/measure/measure_progressive_textures.py --exe tmp/operation-after-bin/deren.exe --out tmp/recheck-default-after --mode eager --no-visibility --frames 180
# Diagnostic attribution only; do not combine its timings with normal captures.
python scripts/measure/measure_progressive_textures.py --exe tmp/operation-after-bin/deren.exe --out tmp/recheck-operation-profile --mode eager --subtilis --hzb --model D:/deren-assets/2026-10-23-software-raster/subtilis-textured/subtilis-textured.gltf --operation-profile --frames 180
```

Raw logs/screenshots/manifests/traces: `tmp/operation-cpu/`. Tracked [CSV](2026-10-27-operation-stream-cpu.csv)
and [compressed evidence](2026-10-27-operation-stream-cpu-evidence.json.gz) retain timing samples,
phase windows, diagnostics, image metrics, commands/configurations and identities. Test logs:
`tmp/operation-stream-ctest-final.log`, `tmp/operation-device-probes-final.log`,
`tmp/operation-group-optimized-final.log`, `tmp/operation-group-unoptimized-final.log`.
