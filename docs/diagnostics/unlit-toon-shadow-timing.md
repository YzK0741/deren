# Laevatain GPU timing report and unlit shadow fix

Measurement date: 2026-11-06.

## Defect and fix

The application disabled the shadow producer whenever deferred lighting was
unlit. Character-forward shading remained enabled and continued sampling the
map. Camera movement refitted and uploaded the light matrices while the map
retained the old projection, producing incorrect dark regions on skin.

The shadow feature now stays active when either deferred lighting is lit or
character-forward shading is active and ready. The shadow checkbox and pipeline
readiness remain required. Pure unlit without character shading still skips the
producer. Startup unlit policy is applied before the first frame's feature
snapshot, instead of after the first submission.

## Timing corrections

- Graph-recorded shadow draws now close their timestamp interval in the graph
  callback. Previously their work was charged to the following HZB interval.
- Character pipeline groups mark surface, inverted-hull outline, iris-through-hair,
  and overlay boundaries inside the existing primary rendering scope. The graph
  then marks the screen-space rim and TAA resolve/history copy independently.
  Previously these character groups were charged to the TAA interval.
- Optional groups fill skipped positional marks without shifting later labels.
  The timestamp pool remains within its existing 32-mark capacity.
- The overlay names GPU stages and separates CPU pacing/wait from recording and
  submission/presentation wall time. The detailed logs retain all stages.

## Local measurements

Release build, RTX 4060 Laptop GPU, 900x1000, Laevatain default pose, TAA and
FXAA enabled, VSync disabled, validation enabled, no GUI. Values are the final
one-second windows of 300-frame runs, excluding startup windows. The orbit
advances camera yaw by 0.15 degrees per frame.

| GPU interval | Static (ms) | Orbit (ms) |
| --- | ---: | ---: |
| Shadow | 0.00 | 14.41 |
| HZB culling | 0.03 | 0.00 |
| Scene/GBuffer geometry | 11.22 | 10.89 |
| Deferred lighting | 0.81 | 0.54 |
| Gaussian preparation timing bucket | 2.13 | 1.84 |
| Toon surface | 15.73 | 15.74 |
| Toon inverted-hull outline | 10.21 | 10.13 |
| Iris through hair | 0.05 | 0.12 |
| Toon overlay masks | 0.01 | 0.00 |
| Screen-space toon rim | 0.06 | 0.06 |
| TAA | 0.31 | 0.31 |
| Debug interval | 0.01 | 0.05 |
| Composite / tone mapping | 0.64 | 0.70 |
| FXAA | 0.12 | 0.16 |
| Total | 41.33 | 54.96 |

The static window contains 26 frames over 1.03 seconds; the orbit window
contains 20 frames over 1.04 seconds. Cached static shadows require no redraw.
Intervals below the two-decimal reporting precision appear as 0.00 ms.
The existing Gaussian-preparation bucket is a scheduling interval: this preview
contains no Gaussian object, so its value does not establish Gaussian rendering
cost. Ray tracing, visibility-buffer raster/resolve, software visibility,
Gaussian sort/finalize/raster, transparency, bloom and tail all reported 0.00 ms
in these two windows.

### CPU wall-time phases

| CPU interval | Static (ms) | Orbit (ms) |
| --- | ---: | ---: |
| Pace / wait | 39.85 | 53.10 |
| Begin | 0.11 | 0.14 |
| Scene recording | 0.00 | 0.00 |
| Post recording | 1.29 | 1.55 |
| Submit, including presentation | 0.10 | 0.12 |
| Total | 41.36 | 54.91 |
| Total excluding pace / wait | 1.51 | 1.81 |

CPU values are the final 60-frame averages, not exactly the GPU's one-second
windows. They measure wall time, not CPU utilization. Nested culling,
shadow/cluster recording, queue submission and presentation sub-phases must not
be added to the total again. The large pacing interval includes frame-slot and
image-acquisition waits; its size alone does not identify which wait dominates.

### Findings and optimization order

1. Toon surface and inverted-hull outline account for about 63% of the static
   GPU frame. Investigate these draw groups first.
2. Scene/GBuffer geometry accounts for another 27% of the static frame. The
   character is subsequently re-shaded, so geometry/pass duplication merits
   investigation; this report does not attribute the whole scene cost to waste.
3. Camera movement adds approximately 14.4 ms of shadow rendering. Prioritize
   cascade/caster and geometry costs for interactive camera motion.
4. TAA is approximately 0.31 ms, under 1% of either measured GPU frame. Its old
   apparent cost came mainly from incorrect timing attribution.

These measurements identify expensive intervals; they do not yet isolate
vertex/mesh execution, fragment work, memory traffic or pipeline stalls within
those intervals.

### Local evidence

Raw summaries and complete logs are stored under ignored `tmp/`:

- [Measurement summaries](../../tmp/perf-shadow-audit/warm/results.json)
- [Static run configuration](../../tmp/perf-shadow-audit/warm/static/config.toml)
- [Static run log](../../tmp/perf-shadow-audit/warm/static/deren-debug.log)
- [Orbit run configuration](../../tmp/perf-shadow-audit/warm/orbit/config.toml)
- [Orbit run log](../../tmp/perf-shadow-audit/warm/orbit/deren-debug.log)

Both measured runs reported zero Vulkan VUID messages. These evidence links are
local artifacts and are not included in a fresh checkout.

## Validation

`python scripts/gates/game_toon_unlit_shadow.py` moves the camera throughout
each run and checks four cases: unlit with toon, pure unlit, toon with TAA off,
and unlit/toon with shadows disabled. It checks shadow production, named timing
boundaries, skipped stages, and absence of Vulkan VUID reports. The old build
failed the unlit/toon case; the fixed build passes all four.

Targeted CPU tests: `test_profiling`, `test_subtilis_frame`,
`test_render_graph_frame`, `test_render_graph_post`, and
`test_render_graph_lighting`.
