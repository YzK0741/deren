# Image upload size experiment — 2026-10-25

The current two-submit host upload policy has avoidable synchronization overhead. Native host layout
transition plus host copy wins clearly for small images. Staging becomes competitive on some larger
RGBA8 images, but this device does **not** establish a universal size-only crossover. No production
upload policy was changed by this experiment.

## Device and method

- Windows 11, NVIDIA GeForce RTX 4060 Laptop GPU, driver 617.42 (raw 2588573696).
- Release build, actual backend-created device and graphics queue, independent vendored VMA allocator.
- 24 cases, four routes, fresh/reused staging modes, three timed processes, 3 warmups and 21 samples per
  configuration per process: **12,096 timed samples**, 63 per configuration.
- Separate validation-enabled sweep: 384 exact GPU readback comparisons. Each timed process also
  performed 384 readback comparisons outside timers. All four sweeps completed with no skipped case,
  byte mismatch or reported Vulkan validation/backend error (1,536 comparisons total).
- CPU wall-clock completion latency, not GPU timestamp duration. Image creation is included in `total`;
  staging allocation/memcpy/flush, recording, submission and fence completion are included in upload.
  Payload generation, hashing, decoding, region construction, readback and destruction are excluded.
- All routes finish in GENERAL. Sampled + transfer-source/destination usage is common; transfer-source
  permits the byte check and may differ from production images. Device setup is outside timing.

| Route | Timed queue submissions | Meaning |
|---|---:|---|
| `host_current` | 2 | Current policy: GPU transition + wait, host copy, GPU visibility barrier + wait |
| `host_native` | 0 | Host layout transition then host copy; next queue submission publishes writes |
| `staging_shared_usage` | 1 | Staging copy retaining HOST_TRANSFER image usage to isolate usage/allocation effects |
| `staging` | 1 | Staging copy without HOST_TRANSFER, including final transition and fence wait |

Native host copy's zero-submit path follows the [Vulkan host-copy visibility rule](https://docs.vulkan.org/refpages/latest/refpages/source/vkCopyMemoryToImage.html)
and [host layout transition interface](https://docs.vulkan.org/refpages/latest/refpages/source/vkTransitionImageLayout.html).
It is valid for initial upload before publication to GPU work. It does not authorize overwriting an
image still used by in-flight work; that requires resource ownership and completion tracking.

## RGBA8 single mip — reused staging

Values are **median / p95, milliseconds**, allocation + upload. `warm` means staging capacity is
available, not a warm image: every sample creates a new image. The native host route is a proposed
alternative measured here, not the production implementation.

| Case | Payload | Current host | Native host | Staging |
|---|---:|---:|---:|---:|
| rgba8 16 | 0.001 MiB | 0.1209 / 0.1723 | 0.0026 / 0.0058 | 0.0656 / 0.1224 |
| rgba8 32 | 0.004 MiB | 0.1248 / 0.2614 | 0.0032 / 0.0060 | 0.0690 / 0.0957 |
| rgba8 64 | 0.016 MiB | 0.1265 / 0.3422 | 0.0057 / 0.0121 | 0.0690 / 0.1187 |
| rgba8 128 | 0.062 MiB | 0.1360 / 0.2111 | 0.0162 / 0.0240 | 0.0745 / 0.1031 |
| rgba8 256 | 0.250 MiB | 0.1993 / 0.6710 | 0.0657 / 0.0910 | 0.1248 / 0.2534 |
| rgba8 512 | 1.000 MiB | 0.4060 / 0.5803 | 0.2539 / 0.3428 | 0.2776 / 0.5873 |
| rgba8 1024 | 4.000 MiB | 0.9524 / 1.7278 | 0.7417 / 0.9490 | 0.6156 / 1.3791 |
| rgba8 2048 | 16.000 MiB | 3.9103 / 4.5398 | 3.6184 / 3.8559 | 3.4704 / 3.8482 |
| rgba8 4096 | 64.000 MiB | 17.6654 / 21.6690 | 16.1803 / 20.0351 | 16.2977 / 21.0815 |

## Other shapes — reused staging

Same units and timing scope. Side length is shown after the case name; cube cases have six layers,
and `_mips` means a complete chain (257 is a non-power-of-two case).

| Case | Payload | Current host | Native host | Staging |
|---|---:|---:|---:|---:|
| rgba8_mips 64 | 0.021 MiB | 0.2176 / 0.9632 | 0.0085 / 0.0165 | 0.1165 / 0.5119 |
| rgba8_mips 257 | 0.335 MiB | 0.3080 / 0.7265 | 0.0991 / 0.1328 | 0.1905 / 0.3397 |
| rgba8_mips 1024 | 5.333 MiB | 1.5486 / 2.1349 | 1.2330 / 1.8157 | 1.3528 / 1.6859 |
| rgba8_mips 2048 | 21.333 MiB | 5.7344 / 7.6700 | 5.4750 / 7.1060 | 5.4110 / 6.5586 |
| bc7_mips 64 | 0.005 MiB | 0.1887 / 0.4674 | 0.0060 / 0.0116 | 0.1011 / 0.3727 |
| bc7_mips 256 | 0.083 MiB | 0.2001 / 2.0977 | 0.0256 / 0.0402 | 0.1143 / 0.4584 |
| bc7_mips 1024 | 1.333 MiB | 0.8459 / 1.3386 | 0.3654 / 0.4523 | 0.5441 / 0.8566 |
| bc7_mips 4096 | 21.333 MiB | 5.9294 / 6.9251 | 5.2233 / 6.5744 | 6.1038 / 6.5538 |
| rgba16f 64 | 0.031 MiB | 0.2010 / 0.5313 | 0.0111 / 0.0180 | 0.1089 / 0.1636 |
| rgba16f 256 | 0.500 MiB | 0.3695 / 1.0476 | 0.1436 / 0.1921 | 0.2393 / 0.7366 |
| rgba16f 1024 | 8.000 MiB | 2.6171 / 4.0465 | 1.9196 / 2.3578 | 2.3985 / 3.0883 |
| rgba16f 2048 | 32.000 MiB | 8.4307 / 11.7939 | 7.5140 / 11.4330 | 8.4827 / 11.4447 |
| rgba8_cube_mips 64 | 0.125 MiB | 0.2074 / 0.6694 | 0.0292 / 0.0437 | 0.1256 / 0.2931 |
| rgba8_cube_mips 256 | 2.000 MiB | 1.0282 / 1.4506 | 0.4505 / 0.6078 | 0.9127 / 1.0887 |
| rgba8_cube_mips 1024 | 32.000 MiB | 7.9350 / 9.5212 | 7.4970 / 9.4207 | 9.7901 / 11.1802 |

## Fresh staging allocation

`cold` means a fresh staging buffer per sample. It does not mean cold driver, VMA pools, OS caches,
or GPU clocks. Compare these rows within this mode; cases/modes run in fixed order, so cold/warm
cross-mode differences are not a controlled estimate of allocation savings.

| Case | Payload | Current host | Native host | Staging |
|---|---:|---:|---:|---:|
| rgba8 64 | 0.016 MiB | 0.1309 / 0.2279 | 0.0054 / 0.0079 | 0.0707 / 0.1437 |
| rgba8 256 | 0.250 MiB | 0.1897 / 0.2589 | 0.0621 / 0.0807 | 0.1218 / 0.1803 |
| rgba8 1024 | 4.000 MiB | 0.9199 / 1.5071 | 0.7952 / 0.9489 | 0.6277 / 1.3118 |
| rgba8 2048 | 16.000 MiB | 3.2880 / 4.2316 | 2.9678 / 3.9079 | 2.2503 / 3.6345 |
| rgba8 4096 | 64.000 MiB | 14.0916 / 18.0011 | 13.6765 / 18.4991 | 11.7568 / 16.3526 |

## Interpretation and next implementation choice

1. Optimize the existing host path first. For reused-staging comparisons, RGBA8 64² total median drops
   from 0.1265 ms (current host) to 0.0057 ms (native host), versus 0.0690 ms staging. This is removal of
   synchronization overhead, not a rendering frame-rate claim.
2. Preserve staging as an alternative for larger uploads and future batches. RGBA8 2048² total median
   is 3.6184 ms native host versus 3.4704 ms staging (about 4% lower); two rounds' upload medians favor
   staging and round 2 is effectively tied (3.4883 vs 3.4892 ms). RGBA8 1024² aggregate favors staging,
   but round 2 reverses that conclusion.
3. Do not introduce one global byte threshold from these measurements. BC7 full-mip 4096² favors native
   host (5.2233 vs 6.1038 ms total median); RGBA16F and mip/cube cases behave differently. At RGBA8
   4096², staging's upload-only median is lower, but its allocation cost makes total median 16.2977 ms
   versus native host 16.1803 ms. After excluding final iterations, that close total ranking reverses
   (native 16.2927 vs staging 16.2234 ms). Both allocation cost and variability matter; optimizing
   copy time alone can choose the wrong policy.
4. Before enabling automatic selection, measure batched asynchronous uploads and representative scene
   load time with image usage/allocation matching production. A future policy can consider format,
   dimensions/mips/layers, device properties and batch size, with an explicit override. No startup
   auto-tuner or hard-coded threshold is justified yet. Existing mapped uniform/readback buffers are
   outside this texture experiment; GPU-only geometry buffers already use staging.

## Variability, memory and review limits

- Route order rotates, but shape and cold/warm order are fixed. Laptop power/thermal/clock state and
  OS scheduling were not pinned. Some cases have substantial between-round and tail variation; all
  samples are retained rather than selecting a favorable round.
- First warmup and final measured iteration perform readback after each route. Readback is outside
  its own timer but can affect the following route's cache/queue conditions. `sensitivity.csv` removes
  the **entire final measured iteration** (60 rather than 63 samples per configuration) and includes
  each round's median. For example RGBA8 2048² upload remains 3.6038 ms native vs 3.4601 ms staging;
  BC7 full-mip 4096² remains 5.2038 vs 6.0998 ms. Close rankings remain provisional.
- `image_allocation_bytes` is the individual VMA image allocation size. `staging_capacity_bytes` is
  logical buffer capacity, **not** actual allocation/residency or total VRAM. Persistent staging and
  readback survive across cases and are also present during host tests; VMA can retain backing blocks.
  These fields cannot establish route/process peak VRAM or streaming memory savings.
- On this GPU, host-compatible images used memory type 4 (device-local, host-visible, coherent);
  plain staging images used type 1 (device-local). Host performance-query flags were both true.
  `staging_shared_usage` results in the CSV isolate the usage distinction, but neither flags nor this
  copy benchmark measure shader sampling speed. Staging buffer memory type was not recorded.
- This standalone benchmark reproduces current policy using its own allocations/command pool; it
  does not call the full production uploader or measure its mutex contention, content hashing, scene
  scheduling or rendering. Independent review found no blocking copy/barrier correctness defect.

## Reproduction and evidence

```powershell
C:/msys64/clang64/bin/cmake.exe --build build-release-dyn-clang64 --target bench_image_upload -j14
python scripts/measure/measure_image_upload.py --exe build-release-dyn-clang64/bench_image_upload.exe --out build-release-dyn-clang64/upload-bench/new-run --samples 21 --warmups 3 --rounds 3
```

Use a fresh output folder. The opt-in target is excluded from default builds and headless CTest.
The runner rejects missing/duplicate samples, invalid times, wrong submission counts, missing final
readbacks, skipped cases and validation/backend errors. It keeps validation-on timing out of summaries.
`summary.csv` contains both modes, all four routes, component medians/p95; `raw.csv.gz` contains all
12,096 validation-off samples with round IDs. `summary.json` includes device metadata and executable
SHA-256. Per-run logs and device files are preserved alongside them. `SHA256SUMS` covers this evidence.
