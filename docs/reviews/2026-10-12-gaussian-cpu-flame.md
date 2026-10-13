# Current Gaussian CPU flame graphs

Artifacts: [interactive viewer](../benchmarks/2026-10-12-gaussian-flame/index.html),
[project-context SVG](../benchmarks/2026-10-12-gaussian-flame/cpu-project.svg),
[full-stack SVG](../benchmarks/2026-10-12-gaussian-flame/cpu-full.svg).

These are new ETW samples of source commit `f57c54b2`, not a rendering of the old
profile. The workload uses the current GPU Gaussian path, existing 562,974-particle
SH3 flower asset, 1280x720, static near camera `0,0,3.0,0.1190,0.3818,0.3433`,
GUI/VSync/validation off, uncapped, TAA/FXAA off and Bloom on at 0.8/0.35. The
configuration otherwise matches the current performance baseline.

## Capture and symbols

The earlier WPR/ETW method was reused: ProcessThread, Loader, SampledProfile and
SampledProfile stacks, exported through xperf. A new optimized DWARF build uses
`-O3 -g -fno-omit-frame-pointer -DNDEBUG` (RelWithDebInfo). This instrumentation
build is distinct from the Release executable used for timing measurements.

The renderer completed 5,000 frames successfully. Only its recorded PID 11100 is
included; the first five seconds and last two seconds of that process's samples
are discarded. The steady window is 6,186,635..33,126,751 microseconds in the trace.
There were 17,062 process samples, 12,612 in the selected window, and 12,611 joined
to a call stack (99.992%). All selected SampledProfile records are unbatched and
have Count=1. Other processes are excluded from the exported artifacts.

llvm-symbolizer resolves addresses using the exact captured DWARF binaries and
per-process load bases from ImageId records, translated to each image's preferred
base. It does not use a different build's layout. Active named modules are
deren.exe, deren_vulkan.dll and shared_utility.dll. Physical sampled stacks do not
necessarily contain separately named frames for functions optimized inline.
Unresolved system/driver addresses remain visible as addresses; no function name
is guessed for them.

## Readability and interpretation

- Both SVGs are 32,000 pixels wide, with 28-pixel rows and 16-pixel labels.
  Full signatures remain in hover titles. The index underneath each graph prints
  every one of the 127 sampled project function names without truncating them.
- Full view retains all 12,611 stacks' samples, represented by 3,301 tree nodes.
  Project view removes external frames and retains 12,498 project-associated
  samples across 254 nodes. No small sibling branches are silently merged away.
- The project classifier uses the actual function's namespace (including return
  prefixes), not a project type occurring inside an STL function's arguments.
  Namespace/module decorations and parameters are shortened only for labels;
  the original symbol name remains available.
- The standalone HTML viewer adapts to the window width. Click a rectangle or
  index row to zoom into its branch; reset returns to the full scope. Search
  filters the complete name index and highlights matching graph frames. The
  toolbar stays available while scrolling. It works as a local file and needs
  no server or external assets.
- Width means inclusive active-CPU sample count, not elapsed frame chronology,
  GPU time or self CPU cost. The filtered project view still includes samples
  taken inside downstream system/driver calls. The index separately lists true
  physical-leaf self samples; inclusive index rows overlap and cannot be summed.

For example, begin_recording appears in 11,290 sampled stacks but has zero samples
as the physical leaf. Most leaf samples in the full view are in ucrtbase.dll.
This graph alone does not identify that C-runtime routine or prove that the body
of begin_recording consumes that time; use the full stack context before forming
an optimization conclusion. GPU stage costs remain in the separate timestamp
baseline report.

Follow-up attribution is now available in
[CPU hotspots](2026-10-12-gaussian-cpu-hotspots.md): exact executable callsites and
CRT instructions identify the mapped skin-history read as the dominant copy.
That report also contains Release phase-window statistics and their limits.

## Verification and retained evidence

The profile build and renderer run passed. Both SVGs parse as XML. A real headless
browser rendered all 127 index rows and 254 initial project nodes. Interaction
checks covered the full view (3,301 nodes), search, branch zoom (147 nodes in the
tested post-process branch) and reset. Normal and zoomed screenshots were checked
for readable names and layout. SVG index entries and archived folded-stack counts
are checked against the input, and SHA256SUMS covers the exported files.

Portable artifacts live in `docs/benchmarks/2026-10-12-gaussian-flame`: the two
SVGs, HTML viewer, losslessly gzipped resolved folded stacks, metadata and checksums.
Raw ETL/CSV, exact profile binaries, capture/symbolization scripts and logs,
renderer and preview images remain in ignored `build-profile-gaussian-clang64`.
The WPR session started for this task was stopped successfully. No rendering code
or timing-baseline samples were changed; no commit/push performed.
