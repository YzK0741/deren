# Left gray band after maximize/restore

Status: fixed and verified on 2026-10-12. The historical investigation and quota stop below are retained; final cause and acceptance are recorded at the end.

The initial 2026-10-12 repair attempt stopped at the user's requested 3% remaining primary account quota. After the window reset and user resume, diagnosis continued from the HDR evidence below.

User reports a flickering gray vertical band at the left after maximizing and restoring the Gaussian viewer. The supplied image shows the bouquet and debug GUI intact while the background is corrupted at x approximately 0..150.

Tests use current executable, corrected bouquet orientation, user exit camera -12.8916,37.5287,5.1872,0.1190,0.3818,0.3433, and two shader sets: current cached projection, and preserved original Gaussian shaders from before projection caching. The original shaders ignore the cache and do not compute/read it. Other renderer/host code is identical. Windows ShowWindow maximizes to 2560x1494 and restores to 1280x720; the test window is hidden immediately after each operation. The existing F12 capture path is used, with clean synchronization validation.

GUI off, ten maximize/restore cycles for each shader set: final captures pixel-exact, no band. GUI on, max_fps240, one maximize/restore cycle and F12 requested immediately after restore: cached run was clean, but the original-shader run reproduced the user's gray band. The baseline image is `build-release-dyn-clang64/gaussian-maxrestore/baseline-gui-early/screenshot_20261012_041304.png`. The clean cached image is under `cached-gui-early`. Below the panel (y>=220), differences are restricted to x=0..151, span to y719, and reach 73 channel levels; the bouquet region agrees.

The corruption appears in the application's own PNG, so it is not solely an external desktop screenshot artifact. The original-shader reproduction shows that this is not specific to cached vertex projection. This is not an old full-executable baseline, and it does not establish the first revision introducing the problem or rule out shared host state.

Earlier resize synchronization and empty-scene clear bugs were fixed separately; neither their clean validation nor later final-frame resize screenshots prove this intermittent early restored-frame symptom resolved. No speculative production fix was made during this investigation.

Next: capture multiple consecutive frames immediately around restore with GUI on/off and original/current shader sets; inspect fullscreen background/clear and GUI state across frame-ring and swapchain images. Require a reproducing image check before changing synchronization or resource code. Build-tree diagnostic scripts: setup_maxrestore_check.py, check_gaussian_maxrestore_early.ps1. Preserve the user's untracked gaussian-fixture.ply and all existing optimization work.

## HDR isolation during repair attempt

Enabled existing DEREN_GAUSSIAN_HDR_CAPTURE alongside F12 in the original-shader, GUI-on early restore test. One initial run and three repetitions are under `gaussian-maxrestore/baseline-gui-hdr` and `baseline-gui-hdr-1..3`. The first two captures were clean; repetitions 2 and 3 reproduced the gray band, without synchronization validation errors.

In both corrupt captures, PNG at row400/x0 is RGB [119,119,127], while x200 is [47,47,62]. The same frame's HDR, captured after Gaussian drawing and before transparent/post/GUI, is identical at those positions: [0.0374755859375,0.0374755859375,0.052490234375]. Thus the band is introduced after Gaussian rendering. Sorting/projection cache changes are not an evidence-backed fix target.

Next focused probe: retain GUI NewFrame/panel generation but temporarily suppress only ImGui Vulkan draw submission in an isolated diagnostic build. Compare the pre-GUI post output with final output across several early restored frames. This separates post-chain/resource state from ImGui geometry/buffer/texture/state. GUI-off correlation alone does not identify which one fails. Do not guess a synchronization patch from clean validation.

Relevant code: runtime.frames.cppm begins the GUI frame after acquire around line542, record_overlay_if_enabled/draw_overlay_after performs native GUI submission near the end of the frame; graphical_user_interface.cpp record() renders draw data, and on_swapchain_recreated() only updates the min-image hint. ImGui Vulkan backend rotates a frames-in-flight vertex/index buffer ring; backend init sets ImageCount to the configured frames-in-flight count. Buffer-ring misuse and stale post/GUI state are hypotheses, not established causes. These sources were only read, not changed.

## Final cause and fix

Skipping only ImGui Vulkan draw submission reproduced the same band in two of three runs. Generated GUI vertices stayed inside the panel; this ruled out the GUI drawing as the source. Independently compiled post shaders omitting Bloom addition produced three clean runs. Temporary stage logging then identified the concrete failure: Bloom intensity 0.8, recorded 0 passes, inactive 0, unresolved 4, both before and after restore.

`runtime::resolve_pipeline` searched the legacy pipeline registry but returned an empty result after a comment describing lookup in the frame chain. The missing implementation meant the four Bloom passes could not borrow the `post_hdr` pipeline published by `post_composite_pass::named_pipeline`. No Bloom levels were written, while the composite still sampled them with a nonzero weight. Window recreation made their undefined contents visible as a gray strip. Valid layouts and clean synchronization validation do not establish initialized image contents.

The fix preserves legacy registry precedence, then scans the attached chain's passes for the first nonnull named-pipeline answer. The pointer remains borrowed from its owning pass. No Gaussian shader, buffer cache, GUI backend, Vulkan barrier or image format was changed for this fix. All temporary GUI/stage tracing and suppression code was removed after diagnosis.

`test_runtime_injection` now exercises the actual runtime lookup with the headless portable core: missing chain, an empty provider before a valid shared provider, successful `post_hdr`, and unknown name. The chain outlives the runtime. The successful lookup assertion was observed RED before the implementation, then GREEN. Diagnostic native logs after the fix show recorded 4, unresolved 0.

The tracked resize helper now supports `-MaximizeRestore` and `-Camera`: two actions for early capture, six normally, twenty for rapid mode; all end restored. Example using the local bouquet config:

```powershell
source/scripts/windows/check_gaussian_resize.ps1 -Config build-release-dyn-clang64/gaussian-maxrestore/cached/source-gui.toml -WorkDir build-release-dyn-clang64/gaussian-maxrestore/final-early -MaximizeRestore -EarlyCapture -Camera '-12.8916,37.5287,5.1872,0.1190,0.3818,0.3433'
```

## Acceptance and visual change

- Final release build, format and whitespace checks pass; full CTest is 24/24 PASS. Existing Windows hard-link boundary tests run outside the filesystem sandbox.
- Five early maximize/restore runs with native stage tracing are clean; final code with tracing removed passes an early capture and twenty alternating maximize/restore actions with clean synchronization validation. `final-early` and `final-rapid` PNGs were inspected; the left-background probe below y300 has maximum channel difference 0 against the adjacent background, versus the original band up to 73.
- Independent focused read-only review found no blockers in lookup precedence, borrowing/lifetime, tests or helper options. Its optional suggestion to test a second successful provider is deferred; the original failure and null-provider traversal are covered.
- Normal model captures are deterministic, but 13 of 14 frozen hashes differ because Bloom now actually runs. This is a real visual change, not a passing frozen comparison. No references were updated.
- Controlled diagnostic: the same fixed executable with only Bloom addition omitted in a separate shader directory matches all 14 frozen hashes exactly (`gray-band-frozen-no-bloom.log`, `render-check-no-bloom`). This attributes the changed normal captures to restored Bloom, rather than a separate rendering regression.

The user should restart the rebuilt executable. Bloom glows now work as configured, so some scenes may look brighter around highlights. Earlier Gaussian performance reports are historical measurements before this Bloom wiring fix; their whole-frame times should not be presented as current post-fix FPS. Gaussian preparation/raster algorithms were not altered in this repair.
