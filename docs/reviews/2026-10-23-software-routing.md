# Software routing redesign checkpoint

Approved sequence: direct CPU cluster list, triangle-scale routing, GPU routing after HZB. Details in `docs/superpowers/specs/2026-10-23-software-routing-design.md` and implementation plan of the same date.

## Stage 1: direct CPU list

Triangle mode now emits one entry per on-screen software suffix record from the parallel stable split. Normal frames no longer construct tile counts/offsets/CSR duplicates or sort/deduplicate them. Existing GPU layout is retained, but CPU publication only writes the six header words and used entries. Offscreen suffix positions remain part of packed IDs; malformed/capacity failures restore complete hardware runs. Legacy tiled mode retains CSR; explicit screenshots independently rebuild CSR for the CPU oracle.

New helper tests were RED before implementation and pass; full CTest **66/66**. Validation-enabled coplanar-overlap fixture captures at 8/9 frames cover both slots: 119,734 pixels, exact software IDs and depth versus prior triangle producer, CPU depth maximum 2 ULP. The temporary `tmp/sw-direct-tie-8/9` scratch prefix was cleaned; the tracked results JSON is durable and the archived captures are under `D:/deren-assets/2026-10-23-software-raster/gpu-routing-acceptance/`.

Three rounds per far/near camera, same textured asset/settings and 129 timed frames each. Shader bundle hash is unchanged from the preceding triangle prototype. Stages were sequential, not interleaved; GPU/frame variations cannot be attributed to this CPU-only edit. [Raw rows and identities](2026-10-23-software-direct-results.json).

| Metric | Prior triangle far/near | Direct-list far/near |
| --- | --- | --- |
| CPU planning wall | 2.282 / 1.915 ms | 1.820 / 1.626 ms |
| CPU-active frame | 8.36 / 7.88 ms | 8.11 / 7.94 ms |
| GPU frame | 10.52 / 5.53 ms | 10.02 / 6.02 ms |

CPU planning improves approximately 20.2% / 15.1%, while near CPU-active does not establish a gain. GPU programs are unchanged; this is not evidence of a new GPU improvement or stable overall software-raster win.

## Stage 2: triangle-scale routing

Opt-in prototype under validation (`DEREN_SOFTWARE_RASTER_EDGE=1` with triangle mode). Maximum object-space edge lengths are cached once per immutable upload generation, only when requested; records retain the 48-byte GPU ABI and the glTF format is unchanged. Lookup uses first-index/index-count/base-vertex. Missing/malformed/nonfinite metadata returns infinity and stays hardware.

Orthographic screen-edge bound uses projection scale and the model Frobenius bound. Perspective also accounts for the nearest depth of the cluster sphere and off-axis projection Jacobian. Sphere bounds remain in use for near-plane safety and screen coverage; triangle-edge scale is a separate classification value. Long/thin triangles are controlled by maximum edge rather than area. Existing cluster-diameter routing remains the default and the A/B control. The off-axis screen-footprint omission found by independent review was reproduced with a wide-FOV sphere whose center is outside the image but which encloses visible geometry; the regression was RED, then GREEN with a projection-Jacobian sphere footprint. A separate actual-runtime MASK regression exposed missing hybrid policy admission; classification now reuses coarse_allowed and keeps unsupported items entirely hardware. The test also asserts the empty software frame remains active, so a whole-frame refusal cannot masquerade as policy correctness. Full suite **66/66** after both fixes. One review minor remains deferred: the constructor-forgeability test still uses its old argument count.

Validation-enabled opaque overlap fixture at diagnostic threshold4096: 115,446 software pixels, exact CPU/GPU IDs, maximum depth2ULP, identical GPU output in frames8/9. Threshold512 selected no software and was not accepted as raster correctness evidence. The incomplete pre-policy sweep is superseded.

Final same-build interleaved matrix: 3rounds x2cameras x5modes,129GPU samples/run. All rows use the fixed material/footprint policy. [Raw results](2026-10-23-software-edge-results.json).

| Mode | Far GPU | Near GPU | Far CPU-active | Near CPU-active |
| --- | ---: | ---: | ---: | ---: |
| Software off | 9.63 ms | 5.20 ms | 6.97 ms | 6.79 ms |
| Direct list, sphere1px | 10.24 ms | 5.85 ms | 7.49 ms | 7.79 ms |
| Edge0.5px | 9.82 ms | 5.96 ms | 7.70 ms | 7.42 ms |
| Edge1px | 9.54 ms | 5.43 ms | 8.03 ms | 7.90 ms |
| Edge2px | 9.69 ms | 5.69 ms | 7.63 ms | 7.58 ms |

Edge1px routes7147/2591 clusters far/near versus sphere2399/758. Software pass remains0.12/0.09ms at edge1px. The far median advantage over software-off is only0.09ms and run-to-run variation is substantially larger; near remains0.23ms slower and CPU overhead remains. This is not a stable net-gain acceptance. Software and new routing stay default-off/opt-in. Short130frame GPU averages include clock ramp-up; idle P8/210MHz was observed between processes and no fixed GPU clock was imposed.

## Stage 3: GPU survivor routing - acceptance attempt (2026-10-23)

Implemented in the tree as an opt-in prototype (`DEREN_SOFTWARE_RASTER_GPU=1`, triangle path and HZB required) and
now measured/validated on one final build. The ordering contract is the HZB compaction: it classifies visible
candidates, reserves a bounded per-item range of the global software list with CAS (failure leaves that item fully
hardware), emits the hardware survivors and the `(item, original_candidate)` software entries, and atomically
maintains the compute-indirect X/Y words; the software kernel consumes those arguments via `dispatch_indirect` and
the production path never reads a count back to the CPU. Recovery reuses the same mode bit after its final
decision, the temporary eligibility flag (bit 2) is normalized back to the external 0/1/3 flags, and ABI 47/48 add
the enabled buffer-int64 atomics and compute-indirect dispatch (validation in `test_dynamic_link`).

### Hardening landed with the acceptance

* **CPU-oracle gate**: the CPU-producer capture path records whether its independent reference rasterized; the
  readback refuses to publish the GPU image dumps when it did not (`software image capture: CPU oracle failed; GPU
  images not published`). A forced diagnostic (`DEREN_SOFTWARE_ORACLE_FAIL=1`) proves the negative:
   no `-depth/-id/-cpu-depth/-cpu-id`, no manifest, no PNG.
* **Atomic completeness manifest**: every software capture writes `<prefix>-capture.json` LAST, naming each file and
  its byte count; `verify_software_images.py --require-manifest` refuses a prefix without one or whose manifest
  disagrees with the disk (stale CPU/GPU file mixing). The verifier self-test covers the manifest controls.
* **Partition report ordering**: `-route.json` is published as soon as the GPU partition is validated (it needs no
  CPU geometry) and now carries `hardware_primary`, `hardware_recovery` and `recovered_visible`, so a recovery
  frame's evidence names its nonzero counts in the file. GPU image dumps still require the CPU oracle.
* **Deterministic fixtures**: `scripts/measure/make_software_raster_fixture.py` is standalone and deterministic for
  the planar, sloped, coplanar-overlap and occluder shapes. Regenerating the coplanar-overlap shape and cooking it
  with the offline converter reproduces the archived tie fixture byte for byte (glTF `fd34b61e...`, GLB
  `88737225...`).
* **Reproducible capture harness**: `scripts/measure/capture_software_gpu_route.py` owns config/env/markers/
  verification for each acceptance capture (gpu, cpu, fallback, partition-only, no-capture smoke, forced failure),
  records executable + shader hashes in every row, and has a headless `--self-test` registered in CTest alongside
  the fixture generator's (full suite now **68/68**).
* **Validation-enabled all-on smoke** (textured compatibility asset, GPU routing on, far and near): validation clean,
  GPU routing lane logged, one screenshot each.

### Correctness evidence (final build exe `443e0647...`, shaders `f7806b9d...`)

| Item | Capture | Result |
| --- | --- | --- |
| Both frame-ring slots | coplanar tie fixture, 8/9 frames | partition valid slots 0/1, 4 software candidates each; 115,446 covered pixels; CPU/GPU IDs identical; max depth 2 ULP; GPU depth/ID hashes identical across slots; manifests present |
| Forced budget exhaustion | `DEREN_SOFTWARE_GPU_BUDGET=0` / `=1` | valid partitions with 0 / 1 software candidates and 6 / 5 hardware survivors (bounded CAS falls open); budget-1 image verify 7,879 pixels / 2 ULP |
| Nonzero recovery | textured near, 16 frames, 0.5 deg/frame sweep | partition valid, 94 software candidates, hardware primary 7,381, hardware recovery 314, recovered visible 327; 30 covered pixels, 2 ULP |
| Closed-history fallback | 1-frame capture (gate closed) | GPU routing NOT active, valid CPU routing active, 115,446 covered pixels, 2 ULP; software image hashes identical to the GPU-mode fixture capture |
| Oracle-gate negative control | `DEREN_SOFTWARE_ORACLE_FAIL=1` | screenshot refused with "CPU oracle failed; GPU images not published"; no dumps / manifest / PNG |
| Validation smoke | textured far and near, GPU routing, validation enabled | 0 VUID / validation errors |

Raw rows, capture logs, raw images, route/manifest JSON, the runner logs and the exact executable + shader bundle
are archived at `D:/deren-assets/2026-10-23-software-raster/gpu-routing-acceptance/` - **728 files /
1,055,832,427 bytes**, with `sha256-manifest.json` re-read and re-hashed independently after writing
(`missing 0 / mismatched 0 / extra 0`). The earlier pre-continuation `tmp` snapshot remains in the parent
`2026-10-23-software-raster/` archive; the two are separate so the accepted-build evidence is not confused with
the superseded runs.

### Same-build performance matrix (3 interleaved rounds, far/near, 130 frames per run, 129 GPU samples)

Modes: `hardware` = DAG+HZB with software off; `edge_1px` = software on with CPU routing; `gpu_1px` = software on
with GPU survivor routing. All 18 runs used executable `443e0647...` and shader bundle `f7806b9d...` (checked
unchanged at the end of the matrix). Values are medians of three run means; the range in brackets is the run spread.

| Mode | Far GPU | Near GPU | Far CPU-active | Near CPU-active | Far cut wall | Near cut wall |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| hardware | 9.68 ms [9.47-10.41] | 5.73 ms [5.55-6.12] | 8.85 ms | 8.66 ms | 1.45 ms | 1.30 ms |
| edge 1px (CPU routing) | 10.01 ms [9.60-11.09] | 5.07 ms [4.96-6.28] | 9.87 ms | 9.59 ms | 2.25 ms | 2.07 ms |
| GPU routing (edge 1px) | 9.48 ms [8.92-10.64] | 5.31 ms [4.87-6.01] | 9.23 ms | 9.95 ms | 1.62 ms | 1.60 ms |

* GPU routing removes most of the CPU routing cost the stage-2 matrix exposed: cut wall falls **0.63 ms far /
  0.47 ms near** versus CPU edge routing and is only 0.16/0.30 ms above the hardware baseline; the software pass
  itself stays at 0.11/0.09 ms.
* Whole-frame medians: GPU routing is -0.20 ms (-2.1%) far and -0.42 ms (-7.3%) near versus software-off, but the
  run ranges above overlap completely and the same matrix's CPU-routing near mode also lands below hardware; GPU
  and CPU clocks are unfixed (P8/210 MHz idle seen between runs). **No stable net whole-frame benefit is
  established.** A negative result is the honest verdict.
* Screenshot hashes: one unique hash per mode/camera cell except hardware-far (2 of 3), the known small run-to-run
  variation; no correctness conclusion is drawn from final-color equality here.

### Verdict

The GPU survivor routing prototype is **functionally verified** (partition, coverage, recovery, fallback, budget
exhaustion, both slots, validation clean) and **removes the CPU routing overhead** it was built for, but it does
**not** establish a stable whole-frame win for hybrid software raster on the measured textured scene. The software
lane and GPU routing remain **opt-in/default-off**; M4 stays **not accepted**. Remaining limitations: the full-stage
manifest/CSR/hardware/resolve/merge verifier is still not implemented, so the capture-only checks validate the
software producer/partition and not the resolve/merge chain (claim narrowed here); the CPU reference oracle needs
complete retained geometry (the far textured recovery frame's partition validated with nonzero recovery but its
image comparison differed at one pixel, so it is not used as image evidence); no fixed-clock measurement; and GPU
mode requires HZB + triangle producer + int64 atomics + compute-indirect, falling back to the valid CPU routing
when unavailable (verified for the closed-history case).

### Follow-up: shipping-configuration headroom / subpixel-share sweep (E1+E2)

The retained question "is software raster only unprofitable because the test scene lacks tiny triangles?" was
measured on the same textured asset with DAG+HZB fully enabled: 7 configurations (640x360 / 1280x720 / 1920x1080,
far+near, plus one farther camera at 720p) × off/triangle modes × 3 interleaved rounds × 129 GPU samples, 48 runs
on one fixed build. Subpixel share (software clusters/frame ÷ HZB survivors/frame) varied from 12% to 49% while the
software pass itself stayed at 0.04-0.20 ms, but the hybrid frame delta did not track it (Spearman -0.39, n=7,
not significant) and **no configuration showed a win outside its run spread** - the session's GPU clock/thermal drift
(up to ±13% raster spread within one mode) dominates every sub-ms delta. The T1 1.54 ms upper bound does not survive
the shipping configuration intact: HZB already culls 70.6/91.0% of candidates, so the work-fraction estimate is
~0.4 ms (best paired raster delta 2.2 ms, median paired delta 0.33 ms) and the CPU routing cost (+0.5-1.6 ms) has to
be recovered on top. **Verdict: unresolved - the evidence does not support the subpixel-share hypothesis, and the
software lane stays optional/default-off as a closed negative result** (user decision 2026-10-23). Details, tables
and limitations: [software-raster headroom report](2026-10-23-software-raster-headroom.md).
