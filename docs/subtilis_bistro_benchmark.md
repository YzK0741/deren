# Subtilis paging benchmark on Bistro

## Setup

- Base revision `73c90e905143004a967196012b0dd8dcc07bfe29`. The split-timing runs use local instrumentation on that revision. Each shader A/B pair uses a single executable and changes only the GPU cut shader. The experimental shader and instrumentation sources are outside this data-only report.
- NVIDIA GeForce RTX 4060 Laptop GPU, driver 621.01, 1280 x 720. Parallel visibility recording, 1 px cut threshold, and a camera starting at `66.3886,70.8066,108.4211,3.1147,11.2400,12.7235` with 0.7 degrees of yaw per presented frame. VSync, HZB, TAA, and Vulkan validation were disabled for timing.
- Each run presents 180 frames. Frame median and P95 exclude the first 30 frames. Aggregate tables report the median across three runs unless stated otherwise. Split GPU timing excludes the first complete timing window and weights later complete windows by their frame counts.
- The same cooked Bistro asset supplied both routes. The unpaged control removes the page directory while preserving the seven common sections byte for byte across 464 payloads. The paged asset has 786 non-pinned index pages.

## Paging budget sweep

| Cut mode | Arena / upload per frame | Frame median (ms) | Frame P95 (ms) | GPU session mean (ms) | Installed pages | Evictions | Steady frames with LOD0 feedback |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| CPU cut | N/A | 27.73 | 29.01 | 28.79 | 0 | 0 | 0/150 |
| GPU cut, unpaged | N/A | 27.40 | 29.18 | 28.74 | 0 | 0 | 0/150 |
| GPU cut, 64 MiB | 64 / 8 MiB | 29.74 | 30.96 | 31.54 | 577 | 0 | 3/150 |
| GPU cut, 16 MiB | 16 / 8 MiB | 32.63 | 34.69 | 33.85 | 291 | 35 | 150/150 |
| GPU cut, 1 MiB | 1 / 1 MiB | 35.43 | 37.28 | 36.44 | 17 | 1 | 150/150 |

The feedback count uses CPU collection frames because request readback is asynchronous. An item can contribute again on a later frame. Relative to unpaged GPU cut, the 16 and 1 MiB runs emitted 12.7% and 23.3% more visibility triangles over a session. The 64 MiB runs uploaded 23,638,380 bytes each. Their ending captures differed from unpaged GPU cut by 17, 17, and 16 RGB pixels.

## Split GPU timing

The previous visibility interval included GPU cut work. The split marks place the cut and its barriers before the raster interval.

| Cut mode | Frame median (ms) | Frame P95 (ms) | GPU cut and barriers (ms) | Visibility raster (ms) | Resolve (ms) |
| --- | ---: | ---: | ---: | ---: | ---: |
| CPU cut, single control run | 27.65 | 32.36 | 0.000 | 28.500 | 0.153 |
| GPU cut, unpaged | 27.48 | 30.69 | 0.223 | 27.284 | 0.143 |
| GPU cut, 64 MiB | 29.29 | 32.48 | 2.866 | 26.312 | 0.140 |

The cut interval grew by 2.643 ms and the raster interval fell by 0.971 ms. The median frame interval grew by 1.82 ms. A separate copy capture covered 518 submissions and 23,638,380 bytes. GPU copies and their barriers took 1.441 ms in total across the run, with 0.0019 ms median and 0.0076 ms P95 per submission. The copy timestamps start inside the independent graphics submission and exclude CPU staging and submission delay.

## Page lookup experiments

The workgroup cache holds 1024 page IDs in 4 KiB of shared memory. It reuses the ID found during residency checking and retains the original lookup for larger cuts. The precomputed variant records the ID at geometry upload when one page covers a cluster's complete index window, and still reads the snapshot's residency state. All 29,738 cluster windows in the cooked Bistro asset fit a single index page.

| 64 MiB A/B | Original cut (ms) | Variant cut (ms) | Original frame median / P95 (ms) | Variant frame median / P95 (ms) |
| --- | ---: | ---: | ---: | ---: |
| Workgroup cache | 2.860 | 2.859 | 29.652 / 31.007 | 29.607 / 31.192 |
| Precomputed page IDs | 2.870 | 2.857 | 29.728 / 31.424 | 29.756 / 31.341 |

Each timed run installed 577 pages. The workgroup cache differed from its reference by 16, 13, and 0 RGB pixels in the ending captures. The precomputed variant differed by 10, 17, and 13 RGB pixels. The cut and frame timings showed no repeatable gain from either variant. The original shader was restored and its recompiled SPIR-V hash matched the reference.

## Per-run measurements

Frame and GPU timings are in milliseconds. The GPU column is a session mean for the budget sweep and a later-window cut interval for the split and A/B runs. A/B rows pair the same round index.

| Series | Case | Round | Frame median | Frame P95 | GPU | Installs | Evictions |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Budget sweep | CPU cut | 1 | 27.151 | 28.655 | 27.360 | 0 | 0 |
| Budget sweep | CPU cut | 2 | 27.732 | 29.009 | 29.140 | 0 | 0 |
| Budget sweep | CPU cut | 3 | 27.898 | 29.207 | 28.790 | 0 | 0 |
| Budget sweep | Unpaged GPU cut | 1 | 27.401 | 29.181 | 28.270 | 0 | 0 |
| Budget sweep | Unpaged GPU cut | 2 | 27.309 | 29.059 | 28.740 | 0 | 0 |
| Budget sweep | Unpaged GPU cut | 3 | 27.988 | 29.433 | 29.110 | 0 | 0 |
| Budget sweep | 64 MiB | 1 | 29.744 | 30.893 | 31.540 | 577 | 0 |
| Budget sweep | 64 MiB | 2 | 29.716 | 30.964 | 30.880 | 577 | 0 |
| Budget sweep | 64 MiB | 3 | 29.741 | 31.194 | 32.120 | 577 | 0 |
| Budget sweep | 16 MiB | 1 | 32.026 | 33.684 | 33.570 | 291 | 35 |
| Budget sweep | 16 MiB | 2 | 32.628 | 34.695 | 33.850 | 294 | 38 |
| Budget sweep | 16 MiB | 3 | 33.222 | 34.944 | 34.310 | 258 | 2 |
| Budget sweep | 1 MiB | 1 | 34.643 | 36.378 | 35.480 | 17 | 1 |
| Budget sweep | 1 MiB | 2 | 35.430 | 37.453 | 36.440 | 17 | 1 |
| Budget sweep | 1 MiB | 3 | 35.441 | 37.280 | 36.850 | 17 | 1 |
| Split timings | CPU cut | 1 | 27.647 | 32.358 | 0.000 | 0 | 0 |
| Split timings | Unpaged GPU cut | 1 | 27.452 | 30.759 | 0.239 | 0 | 0 |
| Split timings | Unpaged GPU cut | 2 | 27.478 | 30.693 | 0.220 | 0 | 0 |
| Split timings | Unpaged GPU cut | 3 | 27.676 | 29.931 | 0.223 | 0 | 0 |
| Split timings | 64 MiB | 1 | 29.294 | 32.267 | 2.841 | 577 | 0 |
| Split timings | 64 MiB | 2 | 29.262 | 32.478 | 2.866 | 577 | 0 |
| Split timings | 64 MiB | 3 | 29.531 | 33.647 | 2.887 | 577 | 0 |
| Workgroup cache | Original, 64 MiB | 1 | 29.555 | 30.976 | 2.860 | 577 | 0 |
| Workgroup cache | Variant, 64 MiB | 1 | 29.672 | 31.554 | 2.859 | 577 | 0 |
| Workgroup cache | Original, 64 MiB | 2 | 29.652 | 31.007 | 2.885 | 577 | 0 |
| Workgroup cache | Variant, 64 MiB | 2 | 29.600 | 31.192 | 2.895 | 577 | 0 |
| Workgroup cache | Original, 64 MiB | 3 | 29.711 | 31.040 | 2.842 | 577 | 0 |
| Workgroup cache | Variant, 64 MiB | 3 | 29.607 | 31.150 | 2.853 | 577 | 0 |
| Workgroup cache | Original, unpaged | 1 | 28.018 | 29.393 | 0.217 | 0 | 0 |
| Workgroup cache | Variant, unpaged | 1 | 27.950 | 29.338 | 0.227 | 0 | 0 |
| Precomputed IDs | Original, 64 MiB | 1 | 29.745 | 31.812 | 2.855 | 577 | 0 |
| Precomputed IDs | Variant, 64 MiB | 1 | 29.948 | 32.124 | 2.908 | 577 | 0 |
| Precomputed IDs | Original, 64 MiB | 2 | 29.686 | 31.424 | 2.870 | 577 | 0 |
| Precomputed IDs | Variant, 64 MiB | 2 | 29.654 | 31.187 | 2.857 | 577 | 0 |
| Precomputed IDs | Original, 64 MiB | 3 | 29.728 | 31.210 | 2.877 | 577 | 0 |
| Precomputed IDs | Variant, 64 MiB | 3 | 29.756 | 31.341 | 2.857 | 577 | 0 |

## Vulkan validation

Each row is a separate 120-frame capture with Vulkan validation enabled.

| Run | Arena | Installs | VUID messages |
| --- | ---: | ---: | ---: |
| Budget sweep | 64 MiB | 577 | 0 |
| Budget sweep | 1 MiB | 17 | 0 |
| Split timings and copy timestamps | 64 MiB | 577 | 0 |
| Workgroup cache | 1 MiB | 17 | 0 |
| Precomputed IDs | 1 MiB | 17 | 0 |
