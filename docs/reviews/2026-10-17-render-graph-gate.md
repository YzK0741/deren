# Render graph adoption: stage 1, the opt-in slice measured at gate scale

Stage 1 of the adoption plan: run the EXISTING opt-in main graph through the render gate
before writing any new declaration, because the delivered slice's evidence was a single
DamagedHelmet frame set (256x256, 8 frames, one camera) and could not say which scenarios
the graph actually covers.

## Method

Two rounds against the SAME binary, same session, same driver, differing only in the
injected `[render] main_render_graph` key:

```
pwsh -File scripts/windows/check_render.ps1 -Full -BuildDir build-release-dyn-clang64 -MainGraph
pwsh -File scripts/windows/check_render.ps1 -Full -BuildDir build-release-dyn-clang64 -Compare <round A hashes>
```

Two small harness changes were needed and are part of this commit:

- `-MainGraph` injects `main_render_graph = true` into every generated scenario config
  (a scenario that names the key itself still wins, because TOML rejects a duplicate).
- every run's `debug.log` is now copied next to its screenshot as `<scenario>.debug.log`.
  Without it the next scenario deletes the log, and the activation/fallback inventory IS
  that log.

## Result 1: which path each scenario ran (from the runs' own logs)

| scenario | path in its 40-frame run |
| --- | --- |
| deferred, deferred_ssao_off, glossy_motion, metal_rough_glossy, shadow_single, sponza, unlit | **graph only** |
| deferred_taa_fxaa, deformation, laevatain_goo_toon, laevatain_goo_toon_body, laevatain_no_sidecar, laevatain_old_chain | **graph first, then legacy** (both lines present) |
| transparent_blend | **legacy only** |

The first inventory pass (taking the first matching line per log) read as "13 of 14
activated". That is an artefact of the logs: the activation line is printed ONCE
(`main_graph_reported`, runtime.post_graph.cpp:360) and only after `prepared->record()`
returned ok, so its presence means "at least one frame was recorded through the graph" -
not "the run used the graph". Six scenarios contain BOTH lines, activation first and the
fallback 6-8 log lines later, so their run recorded a graph frame and then switched to the
legacy tail for the rest; the logs do not carry a frame index, so the number of graph
frames is not recorded, but it is not the 40 the round ran.

Why the eligibility flips mid-run is NOT answered here. The predicate is live state, not
configuration: `taa_active()` is `taa_on && pass_ready("taa") && deferred_lit_active()`
(runtime.frames.cppm:1574) and `feature_active()` delegates to a backend predicate over
per-frame feature facts (runtime.cpp:996). A term that becomes true after the first frame
- pass readiness, megalights activity, a character stage's facts - flips the frame from
graph to legacy. That is the first thing stage 2 must pin, because it decides whether the
gate's A/B evidence covers the graph path at all for those scenarios.

Consequence for the plan: today the graph covers the WHOLE run in 7 of 14 scenarios. The
other six are mostly legacy and must not be counted as graph-covered; `transparent_blend`
is the only clean fallback (the documented exclusion works).

## Result 2: byte identity, at full SHA-256

Every scenario's frame is byte-identical between the graph round and the legacy round -
14 of 14, compared as full 256-bit SHA-256 of the captured PNG (13 from the two rounds'
`*.actual.png` sets plus `deformation`, re-run in both configurations because it produced
no `.actual.png`). Examples:

```
deferred            FAE60C0B1D8C119188236E31...   identical
sponza              24D55DE5C6E814EEB87EDC5D...   identical
transparent_blend   4D3FD6ACB2953BE4BDB59831...   identical
deformation         723569BA0D03640C16B3C62AD88A648E03A3457793344F030AD44BBAECF36F81  identical
```

Honest reading: for the six mixed scenarios this mostly compares legacy against legacy
(39-40 of their 40 frames each), so the identity there is weaker evidence than it looks.
The seven graph-only scenarios are the ones where the comparison really is graph vs legacy.

## Result 3: the frozen baseline is stale on this machine (not a graph finding)

The rounds report `changed: 13` because they are compared against the machine-local
reference set, and that set no longer matches this machine/driver. The control that
separates the two:

- graph OFF, `deferred`, `-Compare frozen` -> hash `FAE60C0B1D8C1191`, MISMATCHED against
  the frozen `972A31EC5FF55C87`;
- graph ON, same scenario -> the SAME `FAE60C0B1D8C1191`.

So the frozen fourteen are stale here (the harness header already warns that references
are per-driver values), and the meaningful verdict in this session is the two-round
`-Compare <round A>` one, which reported `matched 14, mismatched 0` / `PASS (14 of 14)`.
Re-baselining the frozen list is a separate, deliberate act (`-Update`) and is NOT done
here.

## Result 4: documentation corrected while measuring

- the stage plan's checkboxes for 2.2/A2, 2.3/B and 2.4/C were still unchecked although
  the 2026-10-16 records accept them; 2.5/D and 2.6 gained an explicit status note;
- `docs/render_graph.md`'s limits paragraph still said execution binding/recording were
  pending while the same file's later sections document them as delivered;
- the Boost review's "not implemented here" line now points at the A1 record that
  superseded it the same day.
- the graph manual's exclusion list needs one caveat that this round measured: those
  stages select the legacy tail WHEN THE PREDICATE SAYS THEY ARE ACTIVE (live state), not
  when their config key is true - `deferred_taa_fxaa` sets `taa = true` and still recorded
  a graph frame.

## Evidence in this directory

- `maingraph-hashes.txt`, `legacy-hashes.txt`: the two rounds' per-scenario hashes.
- `maingraph-logs/<scenario>.debug.log`: the activation/fallback lines, one copy per run.
- `maingraph-configs/<scenario>.toml`: proof the key was injected (`main_render_graph = true`).
- `maingraph-deformation.png`, `legacy-deformation.png`: the one scenario re-run for a
  full-hash comparison.
- The full capture set (13 `.actual.png` per round, ~4 MB each) stays in the ignored build
  directory `build-release-dyn-clang64/render-check/`; the SHA-256 values above are the
  evidence, and the round is reproducible with the two commands in "Method".

## Addendum: the flip explained - the scenario's toggles land after the first frame

The fallback line now names the term that refused the frame (`runtime.frames.cppm`, printed once),
and the three mixed scenarios answered:

```
deferred_taa_fxaa   main graph: unsupported tail combination (TAA active) ...
deformation         main graph: unsupported tail combination (deferred lighting inactive) ...
laevatain_goo_toon  main graph: unsupported tail combination (character forward) ...
```

Readiness is NOT the cause: the TAA resolve pipeline is created at log line 642 and the
character-forward pipelines at 518-530, both well BEFORE the graph frame at 1004/1612 - so
`pass_ready()` was already true. What changes is the FEATURE STATE, and it changes because these
toggles do not reach the runtime from the configuration directly: they are copied into the GUI
binding struct and mirrored into the runtime by the frame loop
(`source/app/main.cpp:2520-2535` - `gui.character_forward = settings.render.character_forward;`
and the comment beside it says a config value that never reached the binding "would be overwritten
by the binding's default on the very first frame"). The first frame's eligibility is frozen before
that mirroring, so frame 0 is evaluated under the binding DEFAULTS:

| scenario | frame 0 was evaluated with | the rest of the run with |
| --- | --- | --- |
| deferred_taa_fxaa | TAA off | TAA on |
| deformation | G-buffer debug view off (its config sets `gbuffer_debug = true`) | debug view on, so deferred lighting inactive |
| the four laevatain_* | character forward off | character forward on |

Consequence, and it is worse than "not covered": the single graph-recorded frame of those six
scenarios is a frame the scenario's own settings would have EXCLUDED - it was rendered with the
binding defaults, not with the features the scenario is named for. The A/B identity still holds
(both rounds take the same frame-0 path and the same legacy tail), but those six must not be
counted as graph coverage, and `deformation`'s entry is really "legacy only, after one pre-toggle
frame".

Two candidate fixes, both small, and they need a gate round because they change which path frame 0
takes (and TAA history is temporal):

1. mirror the bindings into the runtime BEFORE the first frame's eligibility is frozen (the real
   fix: today every binding lands one frame late, which is a general startup-ordering property,
   not a graph property);
2. refuse the graph for the run's first frame (a graph-side workaround that leaves the underlying
   one-frame lag alone - it makes the coverage honest without touching the frame loop's ordering).

Fix 1 is the honest one and matches the comments already in `main.cpp`; fix 2 is the minimal-risk
one. Neither is implemented here: this stage's job was to measure, and either choice changes runtime
behaviour that the gate must re-witness.

## What stage 1 changes about the plan

1. Stage 2's first task is not a declaration: it is to explain and pin the mid-run
   eligibility flip (which term changes, and whether a graph-recorded first frame followed
   by legacy frames is acceptable per the plan's "a partially recorded frame cannot re-run
   the legacy path" rule - the switch is decided before recording, so it is legal, but 1
   graph frame out of 40 is not coverage).
2. The declaration work (material textures/IBL, per-mesh addresses, meshlet tables with
   atomic writes, indirect commands) stays necessary - the 7 graph-only scenarios cover
   deferred, SSAO-off, shadow-single, sponza, unlit, glossy motion and the material sweep,
   and their accesses are still declared only through the shared read-only bundle.
3. The scenario gate now has a per-scenario path inventory as evidence, which the next
   stages can diff against: a stage is done when its scenario moves from "graph first, then
   legacy" (or "legacy only") to "graph only" AND stays byte-identical.
