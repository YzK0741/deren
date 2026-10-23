# Software routing redesign checkpoint

Approved sequence: direct CPU cluster list, triangle-scale routing, GPU routing after HZB. Details in `docs/superpowers/specs/2026-10-23-software-routing-design.md` and implementation plan of the same date.

## Stage 1: direct CPU list

Triangle mode now emits one entry per on-screen software suffix record from the parallel stable split. Normal frames no longer construct tile counts/offsets/CSR duplicates or sort/deduplicate them. Existing GPU layout is retained, but CPU publication only writes the six header words and used entries. Offscreen suffix positions remain part of packed IDs; malformed/capacity failures restore complete hardware runs. Legacy tiled mode retains CSR; explicit screenshots independently rebuild CSR for the CPU oracle.

New helper tests were RED before implementation and pass; full CTest **66/66**. Validation-enabled coplanar-overlap fixture captures at 8/9 frames cover both slots: 119,734 pixels, exact software IDs and depth versus prior triangle producer, CPU depth maximum 2 ULP. Raw prefixes `tmp/sw-direct-tie-8/9`.

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

## Stage 3: GPU survivor routing

Implemented as an opt-in prototype (`DEREN_SOFTWARE_RASTER_GPU=1`, triangle path and HZB required), pending final acceptance. HZB compaction classifies visible candidates and uses bounded per-item reservations; exhausted reservations retain hardware survivors. Software consumes indirect arguments without production CPU count readback. RHI ABI48 adds compute indirect dispatch; graph declares route/list/argument dependencies.

Screenshot-only owned snapshots independently validate the actual GPU HW/SW partition and rasterize actual SW candidate IDs on CPU. Final validation-clean overlap fixture covers both slots: 115,446 software pixels, exact CPU/GPU IDs, depth maximum2ULP, byte-identical slot outputs. Budgets0/1 produce valid partitions with0/1 software candidates. Raw evidence: `tmp/sw-gpu-final-*`. These are correctness checks, not performance evidence.

Paused at user-requested five-hour remaining5%. Independent review found CPU dynamic dispatch-count lag and missing per-record vertex base in the CPU oracle. Fixes are applied; vertex-base regression was observed failing before the fix, but latest rebuild and post-fix tests remain pending. Previous full suite passed66/66 before final changes. GPU performance matrix and explicit nonzero recovery/invalid-history acceptance remain outstanding; no net gain claimed.
