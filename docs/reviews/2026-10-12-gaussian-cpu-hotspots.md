# CPU hotspot attribution for the current Gaussian baseline

Source commit: `f57c54b2`. This follows the
[CPU flame graph](2026-10-12-gaussian-cpu-flame.md) and
[current performance baseline](2026-10-12-gaussian-performance-baseline.md).
No rendering implementation was changed for this investigation.

## Finding

There is a substantial CPU-side memory-copy cost in the runtime, even though
Gaussian-specific CPU preparation is only about 0.002 ms. The runtime advances
skin-matrix history every frame, including in this bone-free Gaussian scene:

```cpp
size_t const bytes = skin_previous.size() * sizeof(glm::mat4);
std::memcpy(published, skin_previous.data(), bytes);
std::memcpy(skin_previous.data(), current, bytes);
```

`skin_previous` is initialized to the full `scene_skin_capacity` of 2048 matrices,
not the number used by the scene. Each copy is 2048 * 64 = 131,072 bytes (128 KiB).
The first writes CPU history to the previous-frame mapped buffer. The second
reads the current mapped storage buffer back into ordinary CPU history. Both
execute because the buffers/vector exist; there is no bone-use/count gate.
Gaussian rendering has no skin-matrix consumer.

## Address and source attribution

The existing steady ETW capture has 12,611 joined active-CPU stack samples.
11,261 physical leaves are in ucrtbase.dll (89.30%). In particular:

| Copy call | Inclusive samples | CRT-leaf samples | Share of all active samples |
| --- | ---: | ---: | ---: |
| Publish CPU history to mapped previous buffer | 48 | 48 | 0.38% |
| Read current mapped buffer back to CPU history | 11,184 | 11,183 | 88.68% |

Most CRT leaves are at runtime PC `0x7ffb6145e2bb`, RVA `0xEE2BB`, whose instruction
is `REP MOVSB`. The helper begins at RVA `0xEE2B0`. The CRT export table gives the
same RVA `0xEE2D0` to memcpy and memmove; that dispatch routine branches back to
this copy helper. The helper precedes the export, so merely assigning the nearest
preceding export would incorrectly call it memcmp.

The executable's two return sites are RVA `0x7D88A` and `0x7D89C`. Disassembly
shows both preceding instructions call the memcpy import thunk. The former maps
to the first copy, the latter to the second. DWARF inline/source lookup confirms
`advance_motion_deformations()` in runtime.frames.cppm. A return address can map
to the following source statement (line 562) after the inline function ended;
the call instruction and preceding inline line identify the actual copy.

The CRT image's trace stamp `0x2BBEFD73` and size `0x14C000` match the current
System32 binary, whose SHA256 is saved. Thus this attribution uses matching
binaries, exact call instructions and source; it is not a guessed function name
or the old failed DbgHelp symbol pass. System PDB downloading was unnecessary.

## Release CPU phase statistics

Reused all nine forced-GPU baseline runs. Each log contains six completed windows
of 60 frames. Discard the first warmup window and retain the next five: 15 window
means / 900 frames per scenario. The table is the median of those 15 rounded
60-frame means, not a per-frame median or P95. Logs round to 0.01 ms. These phase
windows and completed-slot CSV samples have slightly different boundaries and
should not be treated as paired per-frame measurements.

| CPU wall-clock phase, ms | Static | Rotation | Near |
| --- | ---: | ---: | ---: |
| Pace/acquire | 0.04 | 0.04 | 0.04 |
| Begin recording | 3.67 | 2.55 | 3.64 |
| Scene recording | 0.03 | 0.08 | 0.03 |
| Post recording | 0.10 | 0.11 | 0.11 |
| Submit including present | 2.72 | 2.74 | 3.30 |
| Queue submission subphase | 0.02 | 0.03 | 0.02 |
| Present subphase | 2.70 | 2.71 | 3.28 |
| Total nonoverlapping phases | 6.58 | 5.53 | 7.11 |

Queue and present are subphases of submit; do not add them to submit again.
Present duration includes API/driver/presentation waiting and is not an estimate
of active CPU computation. Likewise, active-sample percentages cannot be
multiplied by total frame duration to obtain copy latency. Individual copies were
not separately timed; the 2.5-3.7 ms figure describes the begin phase, whose active
samples are overwhelmingly in the mapped-buffer read.

The phase statistics also explain much of the earlier frame spread. Below, begin
is the arithmetic mean of each run's five 60-frame means; frame/GPU are the
300-row CSV medians, so they are different statistics:

| Near run | Begin, ms | Present, ms | Frame median, ms | GPU total median, ms |
| --- | ---: | ---: | ---: | ---: |
| 1 | 2.552 | 3.284 | 5.952 | 2.745 |
| 2 | 3.672 | 3.288 | 7.081 | 2.749 |
| 3 | 3.636 | 3.290 | 7.060 | 2.747 |

The near GPU median changes by less than 0.004 ms while begin changes by about
1.12 ms and frame median by about 1.13 ms. This supports a CPU-side explanation
for that repetition spread; it is not a controlled before/after optimization
measurement. Hardware clocks and normal desktop activity remain uncontrolled.

## Memory policy and next step

The skin buffers use storage_coherent. Its allocator uses CPU_TO_GPU,
HOST_VISIBLE | HOST_COHERENT and HOST_ACCESS_SEQUENTIAL_WRITE. It is designed for
CPU writes, and does not require HOST_CACHED. Actual selected memory-type flags
were not queried here; slow uncached/write-combined mapped reads are a plausible
explanation, not a separately verified hardware conclusion. There is no GPU
readback command involved: the CPU directly reads a mapped allocation.

It is premature to characterize this renderer as purely GPU limited. The CPU
path has a large avoidable cost that is unrelated to Gaussian sorting or SH math.
The next change should keep skin history from its CPU upload source, operate on
the used range and avoid history work when no bones are used. It must preserve
previous/current deformation semantics and frame-slot safety for animated mesh
scenes; disabling the function globally is not an acceptable production fix.
Then use interleaved baseline/candidate measurements and image/motion-vector
checks before claiming an FPS gain.

## Retained evidence

`docs/benchmarks/2026-10-12-gaussian-flame` now also holds cpu-hotspots.json,
cpu-phases.json, the executable callsite disassembly and CRT helper disassembly,
covered by SHA256SUMS. Existing folded stacks, trace provenance and flame graphs
remain unchanged. No new baseline or speedup claim was invented, and no commit
or push was performed.
