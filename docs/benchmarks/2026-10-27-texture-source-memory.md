# Texture source memory: ownership and import lifetime

2026-10-27. Bistro, 1280 x 720. The optimization releases import-only decoded pixels,
shares immutable resolved mip owners with lazy/progressive streaming and drops application-side
material references after import. Texture content, mip policy and GPU budgets are unchanged.

## Retained allocations

The following values are GiB (2^30 bytes), from separate memory captures; these runs are not
performance measurements. CPU means current mimalloc usable bytes counted by the project
allocator, including vessel capacity, not whole-process memory. Each profile has one before/after
pair, so these are observed values rather than a multi-round confidence estimate.

| Profile | CPU steady before | CPU steady after | CPU lifetime peak before | CPU lifetime peak after | GPU steady before / after |
|---|---:|---:|---:|---:|---:|
| Full texture residency | 5.322 | 0.252 | 5.332 | 5.315 | 3.020 / 3.020 |
| Visibility streaming, moving | 7.568 | 2.499 | 7.574 | 5.315 | 1.449 / 1.449 |
| Progressive mip streaming, fixed | 7.568 | 2.499 | 7.574 | 5.315 | 1.152 / 1.152 |
| DAG + HZB + progressive mips, moving | 7.640 | 2.570 | 7.645 | 5.280 | 1.154 / 1.154 |

All four profiles save approximately 5.069 GiB of retained project allocations. This is not all
the removed streaming copy: raw decoded pixels account for about 2.43 GiB; eager import also
stops retaining resolved mip chains, while streaming shares its required mip source instead of
duplicating it. Streaming still retains roughly 2.25 GiB of source data for future uploads.
The loader's complete resolved set and the runtime's deduplicated set are not identical.

Later validation found and corrected an extra undeclared mip appended to rectangular images by
the consuming generator; see the [feedback-copy follow-up](2026-10-28-feedback-copy-readback.md).
Its literal byte-count tests also cover the copying path. Both arms of the follow-up contain the
correction and observe a roughly 2.97 GiB import peak; historical tables here retain their original
capture values rather than mixing importer revisions.

OS observations support the direction, using Windows GetProcessMemoryInfo sampled every 100 ms.
Progressive working set falls from 7.739 to 2.737 GiB, and private bytes from 10.137 to 4.767 GiB.
These counters include memory outside vessel, allocator caches and runtime/driver allocations;
they must not be equated with live texture payload. Working-set peak is a process lifetime OS
counter; CPU allocator peak likewise includes import. Before the consuming-import change below,
the remaining import peak was about 5.3 GiB.

## Consuming import: moving decoded pixels into mip0

The ownership work above still left the decoded RGBA set and the generated mip chains alive at the
same time during import. The application now calls the consuming material-resolve overload when no
toon sidecar needs raw pixels:

- RGBA8 textures move their decoded buffer into mip0 instead of copying it.
- 3/1-channel textures convert once, then release the decoded source.
- The mip chain owns mip0 directly; it no longer copies the base level into a temporary and then
  into the result.
- Textures that no material references are released before mip generation.

A 240-frame resource-sampler capture of the same textured-DAG Bistro asset, separate from timing,
measured the project-allocator lifetime peak:

| mode | CPU peak before consuming import | CPU peak after | steady after | GPU peak local |
|---|---:|---:|---:|---:|
| eager, textured DAG | 5.280 GiB | **3.094 GiB** | 0.324 GiB | 3.319 GiB |
| two-tier streaming (`--mode progressive`, without `--mips`) | no matched two-tier control | **3.090 GiB** | 2.688 GiB | 1.438 GiB |

The matched-asset eager observation is a roughly **2.19 GiB (41%) lower initialization peak**.
The 5.315 GiB eager value in the earlier table uses the raw Bistro asset; the textured-DAG eager
control in the isolation captures is 5.280 GiB. The later streaming capture has `mips: false`,
so it must not be used to claim unchanged steady CPU/GPU allocations against the earlier progressive
mip capture. It independently observes a 3.090 GiB import peak. The app's post-resolve release now
reports 0 B of decoded pixels, because the referenced
textures were moved and unreferenced ones were released before the peak. The loader consuming path is
covered by tests that compare every resolved slot byte-for-byte against the ordinary copy path.
These are single-run allocator observations, not WDDM accounting; the peak still includes the
transient per-texture downsample buffer and the mip chains themselves.

## Timing and images

Two clean reversed-order timing rounds, 360 requested frames per run with the first 60 intervals
discarded, give 602 intervals per arm for progressive mips on the raw Bistro asset:

| Arm | Mean ms | P95 ms |
|---|---:|---:|
| Before | 20.408 | 21.846 |
| After | 20.417 | 21.644 |

This is no measurable frame-time gain or regression in this small sample. Initial timing captures
that overlapped measurement-helper tests are excluded. Memory sampling, validation and operation
profiling are absent from the accepted timing runs.

Eager and visibility-streaming moving before/after final images are pixel identical. The original
progressive memory pair differs in 14,809 pixels, RGB MAE 0.008538 on the 0..255 scale, maximum 30,
with 12 pixels exceeding 8. The moving composed pair differs in 14,442 pixels, MAE 0.008528,
maximum 35, with 24 pixels exceeding 8. These are reported, not silently accepted as exact matches.

Further inspection of the clean fixed-camera timing rounds establishes that both old and new
binaries produce the same two pixel arrays: round 1 is identical across binaries, and round 2 is
also identical across binaries. The within-binary round difference is exactly the original
14,809-pixel difference. Thus that difference is reproducible without changing memory ownership;
it is not evidence of an ownership regression. Its rendering/capture source is still unresolved,
and final images do not prove correctness of every intermediate frame.

## Boundary and restoration verification

Only explicitly immutable owners opt into sharing. The source vector must match the entire
input span; borrowed/subspan/mismatched sources are copied. Merely converting a mutable
shared_ptr to const would leave writable aliases, so import rejects that implicit opt-in.
The regression mutates the original mutable source after snapshotting and checks the retained
upload bytes remain unchanged. Loader tests verify immutable owners survive decoded-pixel
release and metadata remains intact. Toon-sidecar input keeps raw pixels conservatively.

The original restoration probe switches streaming off at frame 80 while asynchronous jobs can
still be active. At completion all 456 wanted non-pinned textures are full resolution with zero
pending jobs or upload failures. Vulkan validation is clean. This exercises restoration through
the disable-streaming transition; it is not an exhaustive long-duration unload/reload stress test.
Single/partial-mip non-power-of-two validation probes are also clean.

After the mutable-owner boundary correction, a fresh full build and all 73 CTest tests pass.
The runtime ownership suite has 105 checks, loader 254 (including consuming-import byte-equivalence),
and progressive texture suite 32.
The [same-binary DAG/HZB comparison](2026-10-27-amnis-isolation.md) adds boundary-corrected frozen-build moving and
restoration validation and separates the remaining CPU cost from these memory changes.

## Evidence and limits

- [Memory CSV](2026-10-27-texture-source-memory.csv)
- [Consuming-import peak CSV](2026-10-27-texture-import-peak.csv)
- [Clean timing CSV](2026-10-27-texture-source-timing.csv)
- [Compressed manifests, traces, summaries and image differences](2026-10-27-texture-source-memory-evidence.json.gz)
- Raw captures: `tmp/texture-source-memory/`; consuming-import captures:
  `tmp/move-init-peak/`, `tmp/move-init-peak2/`, `tmp/move-init-peak-progressive/`.
  Analysis: `tmp/analyze_texture_source_memory.py`.
- Fresh boundary build: `tmp/texture-owner-boundary-build.log`; full regression after correction:
  `tmp/amnis-isolation-build.log`, `tmp/amnis-isolation-ctest.log`.

Historical before/after captures use frozen binaries taken before the mutable-owner correction.
The corrected branch preserves the glTF immutable-owner sharing path; boundary-corrected frozen-build
measurements and validation are in the linked isolation report and predate consuming import. No claim is made that historical hashes identify
the latest build. GPU figures measure resource allocation, not OS GPU residency. CPU figures
exclude unrelated standard allocations. Peak and steady counters answer different questions.
