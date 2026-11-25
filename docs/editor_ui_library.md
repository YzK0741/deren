# Editor UI library

Status: design ready for an initial implementation. The first deliverable is
M0 below. This document defines the architecture and milestone order for the
new UI. [The migration plan](editor_ui_plan.md) provides background and a
scene-editor parity checklist. Where its earlier implementation sketch or
phases differ, use this document. [Editor progress](editor_progress.md) records
the existing animation tooling and [the reference notes](ui_library_reference.md)
record prior art.

## 1. Scope and ownership

Build an editor-owned C# UI for dense surfaces such as the Dope Sheet, curve
editor, Outliner, Details, and dialogs. Share editor models, commands, and file
formats between both editor frontends. Select one frontend at startup with
`--editor-ui=classic|next`. Keep the classic editor as the default until the
migration passes its parity checks. The existing game HUD continues to use
RmlUi.

C# owns element identity, layout, input routing, focus, and the paint list.
Native code owns the window integration, GPU resources, and command recording.
The overlay uses Deren's existing RHI and output-overlay hook. Use the
editor-private managed bridge and follow the repository's module and naming
conventions. The first release targets a single editor window and includes
Latin and CJK text entry. A public toolkit API, HTML/CSS compatibility, and
detached OS windows are outside the first release.

## 2. Element tree and layout

Build a declarative widget description when the UI needs updating and
reconcile it into a retained element tree. Elements own cached layout, focus,
pointer capture, text edit state, and scroll state. Reuse an element by sibling
key and type. A group of unkeyed siblings uses type and position, so a
reorderable list must provide stable keys. A keyed reorder preserves element
state. Reject duplicate sibling keys in development and release focus and
capture when the owning element is actually removed.

Keep editor-state reads separate from widget construction. Rebuilding a
description must have no editor side effects. Reuse typed frame storage and
stable handlers where measurements justify it. Closures and collection
expressions can allocate, so measure managed allocations under real UI load.

Start with row and column measure/arrange, fixed and intrinsic size, weighted
growth, gap, padding, and clipping. Define sizes in logical pixels and convert
to framebuffer coordinates at submission. Cache measurements by constraints
and relevant content or style revision. A parent must rearrange children when
its final rect, spacing, or child order changes, even if their measured sizes
are reusable. Add shrink, min/max and scrolling as needed by real screens.
When adding flex shrink, distribute negative space by shrink factor times
basis and redistribute after a child reaches its minimum. The layout is a
documented subset of flexbox.

Use typed theme values in M0. Later stylesheets can add variables, type/class/id
selectors and interaction states. Cache selector matches only with documented
dependencies. With descendant or child selectors, a parent class change or
reparenting must invalidate affected descendants. Theme reload validates the
new theme before replacing the current one. Size-affecting changes invalidate
layout and visual-only changes invalidate paint.

## 3. Frames and the native bridge

Reuse the host's event order and the output-overlay boundary. Relevant entry
points are `source/app/glfw.cppm`, `source/app/run.cpp`, and
`source/ui/rml_overlay.{cppm,cpp}`. The RmlUi overlay is a reference for
per-frame RHI resource ownership.

1. The window layer queues input in order with logical coordinates, window
   focus and scale. Dispatch it on the editor main thread against the most
   recent arranged tree. If geometry or input scope changed, arrange the
   affected tree before the next coordinate-based event.
2. Editor commands and previews update the model before scene preparation.
   Reconcile and arrange changed UI, then emit an ordered paint list.
3. Submit one frame packet to the native bridge. Its dimensions identify
   logical size and framebuffer size separately. It contains ordered draw
   ranges, instance and glyph spans, and any pending atlas changes. Native code
   copies borrowed managed spans before submission returns. Define fixed-width
   ABI layouts, a size/version field, and range validation at this boundary.
4. The render side consumes the packet using the acquired frame slot. Reuse
   upload buffers and descriptors only after the engine has made that slot
   safe. A skipped or failed frame must leave the last valid UI state usable
   and carry pending atlas updates into the next successful submission.
5. Record the overlay at the existing output hook after scene tone mapping.
   Keep editor callbacks out of the recording path. Native code binds the
   current resolved scene image when it sees a viewport draw command. A frame
   without that image draws a placeholder. Managed code never stores a native
   texture pointer. Rebuild affected resources after resize, format changes,
   or swapchain recreation.

The first implementation can run these phases on the current host thread.
If recording later moves to another thread, it consumes the copied packet.
A future multi-window backend needs an explicit platform design for per-window
focus, scale, pointer capture, presentation, and resource lifetime. Moving a
subtree between roots covers only the layout portion.

## 4. Rendering and dense surfaces

Maintain painter order across panels, glyphs, canvas, menus, and popups.
Intersect nested clips and merge only adjacent draw ranges with the same
pipeline, texture, and scissor. Test overlapping translucent geometry and text
before profiling batch counts. Draw instanced rounded rectangles, borders and
focus rings with analytic coverage. Use the existing overlay target's transfer
and blend conventions. Specify the packet's color and alpha conventions and
test a known translucent swatch over the scene to catch double conversion and
premultiplication errors. Glyph coverage is linear alpha.

Represent the timeline as virtualized channel rows and a canvas. Maintain
keys in timeline order. Use the visible channel range and binary-search the
visible time interval per channel for painting and hit testing. Hit testing
and drag feedback use the same screen/time transform, clip and snap rules.
Handle empty channels, keys near clip boundaries, reverse drags and zoom while
scrubbing. The library owns paint and interaction primitives. The animation
editor owns key identity, selection, edit constraints, and persistence.

Compose the full overlay each frame used for presentation. Retained elements
may cache their paint commands when this reduces CPU time. A separate future
optimization may retain rendered pixels with explicit rules for invalidating
previous and current bounds, translucent overlaps, and surface recreation.
Use profiling to decide whether that extra pass and surface pay for themselves.

## 5. Focus, capture, and edit transactions

Route pointer hits in visual order with popups in their own overlay root.
Modal input stays within the active modal. Restore focus to the invoking
control when a modal closes and that control still exists. Route text input
and editing chords to the focused field before panel and global shortcuts.
A captured gesture receives pointer moves and release beyond its original
bounds. Cancel on Escape, window focus loss, or removal of its owning element.

Use four operations for editable controls and canvas gestures:
`begin`, `preview`, `commit`, and `cancel`. Begin captures the editor's
original state. Preview applies a reversible change and updates rendering.
Commit records one undo entry for the complete gesture. Cancel restores the
original state and produces no entry. A no-op commit produces no entry.
The editor owns the undo history and the UI owns local text editing history
while a field is active. Cover both pointer and keyboard input.

## 6. Text and IME

For M0, use the existing editor font for labels and numeric readouts. Define
the text service boundary early so adding shaped editable text leaves the
widget API stable. The first editable fields must distinguish text storage,
editing boundaries, and screen positions.

- Use C# text indices for selection and edits, constrained to extended
  grapheme boundaries. Use shaped runs and cluster-to-source mappings for
  hit testing and caret placement. Handle ligatures, combining marks, and
  surrogate pairs without corrupting the backing string.
- Shape runs with HarfBuzz and rasterize with FreeType. Choose fallback fonts
  by grapheme or shaping run as appropriate. Keep line breaking and bidi
  paragraph ordering as explicit stages. utf8proc can provide Unicode
  properties and segmentation. Mixed-direction editing requires a UAX #9
  implementation, such as FriBidi, plus visual caret and selection tests.
  Full RTL editing is outside the Latin/CJK release gate.
- Key glyph atlas entries by font face, glyph, raster size, and raster mode.
  Upload coverage before draws that sample it. Atlas pages or regions stay
  alive until frames that reference them retire. Define what happens when
  an atlas fills and during DPI changes. Start with the simplest strategy
  that passes atlas growth and resize tests.
- Run a Windows IME prototype alongside M0 through the existing window
  integration. Deliver composition start, update, commit, and cancel to the
  focused element in event order. Show preedit and selection, place the
  candidate window at the caret with the correct DPI and window origin, and
  ensure the committed text arrives exactly once across native composition and
  GLFW character events. Cancelling leaves the committed text untouched.
  One composition commit is one text undo step. Verify using a real Windows
  IME as well as synthetic events.

## 7. Milestones and acceptance

### M0 - First Dope Sheet workflow

- Add the opt-in frontend, one overlay, and a basic element tree with the
  layout and input needed by the Dope Sheet. Reuse the existing animation
  editor's asset preview, playback, and editor command boundaries.
- Draw a timeline with a viewport, scrollable channel rows, and keys visible
  within the current time range. Support selection, scrubbing, and moving an
  existing group of keys. Use the repository's animation persistence path
  where available. If writing motions is still absent, include the smallest
  motion save/reopen path for this workflow.
- Acceptance: open an asset with an existing clip, move selected keys, see the
  preview update during the gesture, cancel a second move, undo and redo the
  first move, save, then reopen and confirm the same keys and pose. Repeat at
  100% and 150% scale and after resizing. Verify the classic launch path.
- Produce a real Windows IME prototype report. Its results guide M1 and do
  not require introducing an editable text field in the M0 timeline.

### M1 - Text and reusable controls

- Ship shaped Latin/CJK text, clipboard, selection, undo and IME. Add the
  required edit controls, menus, modal dialogs, splitters, and virtualized
  lists. Extend theme and layout support as screens need it.
- Acceptance: edit and save a mixed Latin/CJK asset name using a real IME.
  Verify candidate placement after moving or scaling the window, cancellation,
  focus restoration, and one-step text undo. Use synthetic input for
  repeatable regression checks.

### M2 - Scene editor parity

- Migrate the Outliner, Details, Content Browser, viewport controls, and
  dialogs in small slices. Reuse document operations and the existing editor
  selftest matrix.
- Acceptance: both frontends pass the relevant editing, save/reopen, and Play
  checks. The new frontend also passes keyboard, clipping, and scale checks.
  Switch the default after parity. Remove classic editor code in a separate
  change after verifying that game HUD scripts still work.
- Status: the scene screen in `source/editor/ui/scene_screen.cs` is the editor's
  default frontend now (`--editor`), with `--editor-ui=classic` keeping the
  RmlUi shell reachable and `--editor-ui=animation` keeping the Dope Sheet. It
  has folding and multi-select, the filtered outliner with visibility marks and
  double-click add, a searchable project browser, draggable row reparenting,
  editable details lanes for every classic component, real scrolling for the
  inspector, draggable dividers whose sizes live in `editor-layout.json`,
  viewport click picking, Play, Save/reopen, the status footer, the output tab,
  the environment rows with their map lists, and the behaviour rows (a script
  binding, its parameters and the overrides of an expanded instance).
  `--editor-ui-scene-selftest` covers all seventeen of those paths at 100% and 150%.
  Removing the classic editor code is a separate change, after the game HUD
  scripts are checked against the RmlUi overlay the game still uses.

### Later - Profiling-driven work

Add a curve editor based on the timeline canvas. Consider stylesheet hot
reload, visual transitions, in-window docking, and partial repaint once the
first screens establish their needs. Detached windows and full RTL editing
have their own platform and text requirements.

## 8. Verification and performance

- Add layout and reconciliation tests for clipping, constraints, and key
  reordering. Test painter order, stale frame resources, skipped frames,
  resize/DPI transitions, and focus or capture loss mid-edit.
- Add a timeline fixture with at least 50,000 keys, including a dense visible
  range. Record the visible row/key counts, paint and upload sizes, and
  verify that scrolling and zooming process the visible ranges.
- Report median and p95 timing for build/reconcile, layout, paint, native
  upload, and GPU overlay. Also report managed bytes allocated per frame,
  draw calls, and uploaded bytes. Record dimensions, scale, GPU, and workload.
  Measure idle, scrolling, and key dragging with the same fixture.
- Initial reference targets at 1600x900 framebuffer pixels and 150% scale:
  0.5 ms build/reconcile for 1,000 visible elements, 0.3 ms layout, 0.5 ms
  paint, and less than 8 MB transient geometry. Treat them as budgets to
  validate. Draw count is diagnostic and never overrides painter order.
- Prefer deterministic geometry and input assertions. Capture reference
  screenshots for painter order and text only with a controlled font, scale,
  camera, and animation clock.

## 9. Dependencies

Reuse the existing FreeType build under its current license terms. Add
HarfBuzz (MIT) during the shaped-text work. Evaluate utf8proc (MIT) and
libunibreak (zlib) when their functionality is needed, and include the license
notices for any vendored dependency. Evaluate FriBidi and its distribution
terms only if full mixed-direction editing is scheduled.
