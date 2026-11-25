# Editor progress

Status of the animation editor and of the editor UI work, as of 2026-11-24.

Repository: `D:\deren`, HEAD `2fd9af2b` ("feat(editor): animate placements independently
and validate projects").

Uncommitted at the time of writing (12 paths):

- modified: `docs/editor_roadmap.md`, `source/app/run.cpp`, `source/editor/README.md`,
  `source/editor/editor_host.cs`, `source/editor/editor_host.selftest.cs`,
  `source/editor/ui/editor.rml`, `source/engine/animation/controller.cpp`,
  `source/engine/animation/controller.cppm`, `source/scripting/rml_ui_api.cppm`
- new: `source/editor/editing/motion_document.cs`,
  `source/editor/editor_host.animation_mode.cs`, `source/editor/editor_host.timeline.cs`

## 1. Animation editor: what exists

### 1.1 Isolated mode (done)

The editor has a second mode, entered with the toolbar button that reads `Animation` in scene
mode and `Scene` in animation mode. Entering puts the project document aside and opens an empty
in-memory preview document (`scene_document { name = "Animation Preview" }` with no roots);
leaving drops the preview and reopens the project document with its selection intact. The preview
is never saved: `save_document()` refuses while the mode is active, `Play` refuses too, and the
project files are untouched (the selftest compares the scene file text before and after).

Files: `source/editor/editor_host.animation_mode.cs`.

### 1.2 Preview placement and camera (done)

The empty preview is filled with one asset placement by `Open Asset` in the Timeline header, which
takes the asset highlighted in the Content Browser. The placement is added as a normal scene node
with `animation_clip` set from the asset and autoplay off, so the existing per-placement player
drives it. The camera starts from a fixed pose (yaw 0, pitch 4, distance 1.7, target
`(0, -0.9, 0)`, vertical fov 50) because the asset bind trails node creation, so
`tree.try_get_bounds` has nothing to frame at that moment.

### 1.3 Timeline strip (done)

A strip under the viewport shows Play/Pause, Restart, the clip name, `time / duration s`, and a
scrub bar that can be dragged. It is driven by the placement's player through
`animation_player.try_get_info`, and text is only written when it changes because every RmlUi text
write re-lays out.

### 1.4 Timeline tab, read-only clip inspection (done)

`source/editor/editing/motion_document.cs` reads a `motion.gltf` directly in C# (no ABI growth):
first animation only, sampler -> accessor -> bufferView resolution, `byteStride`, external buffer
file or `data:` URI, VEC2/VEC3/VEC4/MAT4 float accessors, CUBICSPLINE value triples. It produces
one `motion_key_channel` per glTF channel with node index and name, path, interpolation, parallel
`times[]` and `values[]`.

`source/editor/editor_host.timeline.cs` renders one row per channel in a `Timeline` dock tab
(node name, path, key count, time span). Clicking a row selects it and seeks the preview player to
the channel's first key. The header shows clip name, channel and key counts, duration, and the
buffer file name.

Observed on `build/editor-animation-demo`: `Bibideba - 52 channels, 67028 keys, 42.93 s`, source
`motion.bin`.

### 1.5 Skin and morph buffer recycling (done, bug fix)

Repeated scene/animation switches used to explode the mesh. Cause: every animated placement takes
a slice of the shared skin-matrix buffer (2048 blocks, starting at block 4) and the slice was never
returned when a mode switch rebuilt the placements, so the buffer filled up; worse,
`controller::init()` stamped `push.skin_base` into the primitives before the capacity check, so a
rejected placement kept an out-of-range base and the shader read another character's joints.

The fix lives in `source/app/run.cpp` and `source/engine/animation/controller.cpp`:
`managed_bind_context` owns sorted, merged `free_skin` / `free_morph` span lists; a bind first tries
a free span (first fit) and only then the bump allocator; a zero-sized request succeeds without
allocating; dropped players return their spans; the per-tree reset clears both lists; and a
placement that cannot get a slice calls the new `controller::detach_buffers()`, which rewrites its
meshes to the identity block and rest pose instead of leaving them skewed.

Verified with `deren.exe run . --editor-animation-toggle 10`: no `skin buffer is full` lines, and the
allocated blocks cycle `4, 440, 876` instead of climbing.

### 1.6 Selftest and evidence

`--editor-selftest-animation-mode` covers: empty preview on entry, asset selection through the
content browser plus the Timeline `Open Asset` button, strip Play and Restart, seek to half the
duration, the save guard, the untouched project file, mode restore with the same document and
selection, a plain (non-asset) selection still opening an empty preview, six round trips, and every
animated placement still reporting an available player afterwards.

Screenshots in `build/editor-animation-demo`:
`screenshot_20261124_114234.png` (empty preview), `screenshot_20261124_114525.png`
(`laevatain` loaded, Timeline populated), `screenshot_20261124_112602.png` (two actors intact
after ten scene/animation switches).

### 1.7 M0 of the editor UI library (done)

The animation editor now has its own UI layer, per `docs/editor_ui_library.md`, and one real screen
on it. Nothing in this path touches RmlUi.

Native side:

- `source/ui/ui_overlay.{cppm,cpp}` (module `deren.ui.ui_overlay`): a second overlay beside the
  RmlUi one, reusing the 20-byte vertex and `ui/rmlui.slang`. It owns one 1024x1024 glyph atlas with
  a white texel at (0, 0) for solid fills, FreeType rasterisation at the device density, shelf
  packing, logical-to-framebuffer scaling, per-draw clipping as scissor rectangles, rounded
  rectangles as perimeter fans (SDF is a later slice) and one storage buffer of vertices per frame.
- `source/ui/ui_scripting.cppm`: the manager-side table (`submit`, `measure_text`,
  `query_viewport`), owner-thread checked.
- ABI 47: `startup_options.editor_ui` (0 classic RmlUi, 1 own UI) decided before the renderer is
  created because only the managed project knows its flags; `startup_options.ui_scale` pins the
  editor UI scale and converts pointer input; `query_viewport` reports logical size and scale, since
  `frame_info` carries no size at all.
- `source/app/run.cpp` installs exactly one frontend: `--editor-ui=next` (or `editor_ui = 1`) gets
  the new overlay and no RmlUi document; otherwise the classic path is unchanged.

Managed side (`source/editor/ui/`, C# owns logic and layout, native owns window and GPU):

- `ui_rect.cs`, `ui_theme.cs` (typed theme: colours, metrics),
- `ui_paint.cs`: ordered rect/text commands batched under shared clips, UTF-8 text blob, measured
  cache keyed by text/size/scale, one frame packet per frame in logical units,
- `ui_element.cs`: retained element tree reconciled from the declarative description, row/column
  layout with fixed/intrinsic sizes plus weighted growth, gap, padding, clipping, `find(key)` for
  hit testing,
- `ui_context.cs`: per-frame begin/submit, hit testing in reverse paint order, hover/focus state,
- `dope_sheet_screen.cs`, `dope_sheet_input.cs`, `dope_sheet_selftest.cs`: the Dope Sheet itself -
  header, ruler, virtualized channel rows with keys in timeline order, per-row lanes sharing one
  time transform for painting and hit testing, playhead, transport (Play/Pause, Restart, Undo, Redo,
  Save Motion), scrub bar, footer, and the input that drives it (channel selection, key selection,
  key drag with live preview, scrub drag, Escape cancel, wheel scrolling, one undo entry per
  gesture).

Acceptance evidence (`--editor-ui-selftest`, exit 0, three log lines):

- selection, drag, live preview during the gesture, undo, redo, save and reopen of the moved key
  (52 channels reloaded, times equal within 1e-4 s),
- a second drag cancelled with Escape restores the key and pushes nothing onto the history, and one
  undo then reaches the original time,
- the layout stays inside the window at seven width/height/scale cases, including 150%.

Captures in `build/editor-animation-demo`: `screenshot_20261124_140604.png` (150%, full sheet),
`screenshot_20261124_140810.png` (`--editor-ui=next`, 100%), `screenshot_20261124_140821.png`
(classic path for comparison). The standalone overlay proof against the game HUD's demo project is
`screenshot_20261124_132835.png` in `build/source/scripting/test_game` from
`--ui-demo --capture-frames 6`.

The Windows IME prototype and what it could not verify without an installed input method are in
`docs/editor_ui_ime_report.md`; its result is that M1 owns the composition UI and the caret rect.

### 1.8 M1: text, and the first control (in progress)

- HarfBuzz is vendored under `source/third_party/harfbuzz` (upstream `src/` subset, built from the
  amalgam `src/harfbuzz.cc`, FreeType as the only backend) and shaping runs in `ui_overlay`:
  `shape_runs`/`shape_segment` split a run at every code point the primary face does not cover, so a
  mixed Latin/CJK line shapes per font while clusters stay UTF-8 byte offsets into the whole run. The
  font atlas rasterises at the device density and the fallback face is sized per face
  (`font_pixel_size[2]`), which is what made the CJK fallback render at all.
- `editor_ui_api` gained `shape_text` and `set_ime_handler`; `source/editor/ui/ui_text_shape.cs`
  wraps shaping for the managed side, and `ui_text_edit.cs` is the text field: grapheme boundaries
  via `StringInfo`, caret/selection, `insert`/`insert_run`/`backspace`/`delete_forward`, undo and
  redo with one entry per edit, copy/cut/paste through the Win32 clipboard (`ui_clipboard.cs`), and
  a composition preview that never edits the text until it commits.
- The Dope Sheet header now owns the clip name in a real field: click to focus, type (including
  IME), Enter applies to the staged name, Escape restores it. Input events carry `modifiers` and
  `repeated`, so Ctrl+A/C/X/V/Z/Y and word-wise Ctrl+Left/Right work; the event struct grew to 44
  bytes and `native_input_event` mirrors it.
- The window's composition notifications reach the field: `run.cpp`'s IME sink forwards
  start/update/commit/end through `ui_scripting::dispatch_ime`, and the frontend registers its
  handler through `editor_ui_api::set_ime_handler`. With no handler the sink keeps its old path of
  typing each committed scalar as a character event, so the game HUD is unaffected.
- The candidate window follows the field's caret: the frontend publishes the caret box in logical
  pixels through `editor_ui_api::set_ime_caret_rect`, `run.cpp` converts it to client pixels with the
  editor's density and hands it to the window seam, which converts to screen pixels before
  `ImmSetCandidateWindow`. Losing the focus clears the box instead of leaving the list behind.
- Renaming a clip persists: `motion_document.try_write_clip_name` patches only the first
  animation's `name` string in the JSON text (byte-level splice with `Utf8JsonReader` token offsets),
  inserting the member when it is missing, so the rest of the file - loader keys, extras, formatting,
  buffer layout - stays byte for byte. `Save Motion` writes the buffer and then the name.
- The camera no longer fights the controls: the frontend publishes `editor_ui_api::set_pointer_claim`,
  and the window seam's camera gate withholds only the orbit copy of a pointer event while the pointer
  is over a control (or a key drag is in flight), leaving the preview area to the camera. See 1.10.
- `--editor-ui-selftest` now ends with seven more phases: shaping invariants (Latin, CJK, mixed-run
  clusters and widths), the text field (typing, undo, clipboard round trip with retries, one-step
  paste, combining mark, CJK caret advance, preview), the candidate anchor (box inside the field,
  advancing with the caret, moving with a composition, cleared on blur), the clip rename
  (whole-file text equality after a multibyte rename, plus the insert path), the pointer claim
  (controls claim, the preview area does not, a gesture in flight claims wherever it goes), a press
  pushed through the registered input callback (registration, filtering and hit test in one check),
  and a typed character pushed the same way (a press inserts, a character without the flag does not).
  Thirteen selftest lines, exit 0 at 100% and 150%.

M1 is complete except for the real Microsoft Pinyin acceptance run, which only a person at the machine can
do - the synthetic probe proves the plumbing, not the composition window. Everything else is built and
covered: the controls, the splitter, the virtualized list, the menus, the dialog layer (1.13 and 1.14) and
an atlas growth test (1.15).

### 1.9 Save-path bugs found in review (fixed)

Two defects in the key write-back were reported against the M0/M1 code and both were real:

- `motion_document.write_channel` advanced with the accessor element stride for every key and packed
  one key's floats as one contiguous run. That is only correct for LINEAR and STEP; a CUBICSPLINE
  key owns three elements (in-tangent, value, out-tangent), so the second key was written on top of
  the first key's tangent pair, and a strided buffer view was written as if packed. The writer now
  addresses `element = key * blocks + block` at `byte_offset + element * value_stride` and writes
  `components` floats inside the element. The demo clip is all LINEAR (52 channels, zero
  CUBICSPLINE), which is why nothing caught it.
- A drag released on top of another key of the same channel produced equal key times, which glTF
  forbids (sampler input must be strictly increasing). `motion_document.has_strict_times` now refuses
  such a save with a message naming the channel and both times, and the drag release rejects the move
  instead of pushing it: the keys are restored from the pre-gesture snapshot, the notice explains
  why, and the history gains nothing.

New coverage in `--editor-ui-selftest` (now eight lines, exit 0 at 100% and 150%): a generated
CUBICSPLINE fixture with a 16-byte stride is loaded, its two keys are swapped in memory, saved,
reloaded, and checked both as arrays and as bytes (including the stride padding staying zero); a
duplicate-time save is refused; and the release path that would create a duplicate is cancelled.

### 1.10 Pointer ownership between the editor UI and the camera

The window seam forwards every pointer event to the runtime's orbit camera, which is how the 3D
preview above the Dope Sheet rotates. It had no idea the editor's own UI exists, so a left-drag that
started on the name field orbited the model as well; the classic RmlUi path avoided that only through
its `action_input.set_event_filter` callback, which the new frontend does not install (a filter would
also have withheld the event from the managed side, which needs it).

The fix is a claim the frontend publishes and a gate the seam consults:

- `editor_ui_api` grew its seventh entry, `set_pointer_claim(uint8_t over_ui)`. `ui_scripting` keeps
  the byte and exposes `pointer_claimed()`.
- `source/app/glfw.cppm` gained `set_camera_gate(gate, context)`: unlike the filter it never withholds
  an event from the sink, it only decides whether the event is ALSO chained to the callbacks this seam
  replaced. Releases always chain, so the camera never believes a button is still down, and a drag the
  camera itself started keeps its moves even when the pointer wanders over a panel mid-gesture.
- The frontend publishes the claim after each submit: the deepest element under the pointer claims
  unless it is `root` (the whole window) or `viewport` (the preview itself), and an in-flight key
  gesture claims wherever the pointer goes, so dragging a key past the sheet edge keeps the camera
  still.
- `source/app/run.cpp` installs the gate when `startup_options::editor_ui == 1`; the classic path keeps
  its filter untouched.

`--editor-ui-selftest` covers the rule: the name field and a transport button
claim, the preview area does not, and a gesture in flight claims outside the sheet. The gate itself is
a three-condition chain in the seam, exercised by an interactive drag.

### 1.11 A press carried no position (clicks did nothing)

Hovering worked - the camera claim followed the pointer, rows highlighted - but pressing a row, a
transport button or the name field did nothing at all.

The window event is built in `source/app/glfw.cppm`. The cursor callback fills `x`/`y`, the scroll and
button callbacks did not: a `mouse_button` event carried `{type, button, down, modifiers}` and left the
position at its default zero. The managed frontend hit-tests a press with the press position, so every
click landed on the window origin, found no element there and was dropped. Moves were unaffected,
which is exactly why hovering felt alive and pressing felt dead.

The fix is one query: the button callback fills `x`/`y` from the cursor position the seam already
tracks (falling back to `glfwGetCursorPos` before any move has arrived), so a press agrees with the
moves that preceded it, and the scroll event carries the last known cursor position too, because a
wheel belongs wherever the pointer is.

Two diagnostics stay in the tree because a pointer that lands in the wrong place cannot be found any
other way once the window is gone:

- The first editor frame logs `editor ui: window WxH, frame WxH, density D, authoring scale A,
  pointer P`. It is one line per run; a window moved to a monitor with a different density changes the
  ratio afterwards, so the line describes the first frame only.
- `DEREN_UI_INPUT_PROBE=1` logs each managed input event with the key it hit-test to, up to 200 events:
  `Editor probe: mouse_button at 798.0,29.0 down True hit 'none'`.

The selftest gained a nineteenth phase that pushes the events through the registered callback instead
of calling the press handler directly - registration, filtering and the hit test in one check - and the
selftest run's frame budget went from 40 to 120 frames so all phases get their turn.

### 1.12 A typed character carried no press flag (typing inserted nothing)

With clicks working, the name field focused on a click and then ignored every keystroke. Hovering and
clicking were fine, the keyboard was silent.

A character event is built in two places in `source/app/glfw.cppm`, the GLFW character callback and
`glfw_action_input::type_character` (the path an IME commit takes). Both left `down` at its default
false, and the field - like any consumer that asks whether an event is a press rather than one of the
pairs of press and release it answers - only inserts on `down`. Composition text still worked because
it arrives through the IME handler, not through character events, which is why the mismatch hid until
plain typing was tried.

The seam now marks every character event as a press: a typed character has no release, so `down` is
always true and a consumer can filter on it without special cases.

The selftest gained a twentieth phase that types through the registered callback: a character event
flagged as a press appends its codepoint to the focused field, and one without the flag must not change
the text. Thirteen selftest lines.

### 1.13 Reusable controls: hover, buttons, a splitter and a virtualized list

M1 needs controls that any screen can use, and the first thing they need is knowing whether the pointer
is over them. `ui_context` had `hovered_key` and `active_key` declared but never set, so nothing could
ever look hot. The context now keeps the pointer in logical pixels (`set_pointer`), derives
`hovered_key` from the freshly arranged tree inside `submit` - before anything paints - and exposes
`is_hovered(key)`. A control therefore reads its hot state from the context while it paints and never
stores state across a frame, which is what the per-frame tree design wants.

Two files hold the controls:

- `source/editor/ui/ui_controls.cs`: `ui_controls.button` (filled, outlined or subtle, hover-aware) and
  `ui_controls.splitter` (a drag handle that lights up under the pointer and while it is dragged). The
  transport bar now builds its five buttons through the control instead of hand-rolling boxes.
- `source/editor/ui/ui_list_view.cs`: a list that builds only the rows it can show. It keeps the index of
  its first visible row, `visible_rows` from the list height, and a scrollbar whose thumb length and
  offset are the visible window. `build(key, height, item_count, row_for)` adds the visible rows only,
  so a clip with 59 channels produces about 14 row elements instead of 59 - the sheet no longer has a
  "+35 more channels" line, it scrolls.

The wheel follows the pointer now: it scrolls the rows only when the pointer is over them (`channel`,
`channels` or a lane key), so a wheel over the preview keeps zooming the camera instead of moving rows
under the pointer. And the sheet can be resized: the divider between the preview and the sheet starts a
`split` gesture, `update_sheet_split` turns the pointer's y into the sheet's share of the space below
the header, and both bounds are clamped so the sheet never falls below its ruler, three rows and the
transport, and the preview never below 80 logical pixels.

The selftest gained two phases: the list builds five rows for 100 items in a five-row window, moves by
whole rows, stops at its last page and refuses to scroll when everything fits; a wheel pushed through the
registered input callback over the preview must leave the rows alone while one over a row must scroll
them; and the real sheet builds fewer rows than the clip has channels. The second phase drags the divider
through the same callback, which must shrink the sheet, keep both areas above their minimums, and clamp
when dragged past the top. Fifteen selftest lines, exit 0 at 100% and 150%.

### 1.14 Menus and a modal dialog layer

A menu cannot be a child of the screen it drops over: the layout engine gives every child a slice of its
parent, so an absolutely positioned panel would still consume space and shrink the weighted preview. The
popups therefore live in a second layer of the context. `ui_context.add_overlay(element, x, y)` queues a
tree with a position of its own; `submit` measures and arranges the queue at those positions, paints it
after the screen, and `hit_test` walks it in reverse first. The queue is emptied by
`add_popup_overlays`, which the screen calls at the end of the build, so the popups of the previous frame
stay hittable until this one replaces them - the same one-frame-late contract the pointer handlers already
live by.

`source/editor/ui/ui_popup.cs` holds the pieces: `ui_command` (a key, a label, an enabled flag and an
optional action), `ui_menu`, `ui_dialog`, `menu_title` (the header tab, accent while its panel is open),
`build_menu`, `build_dialog`, `build_scrim` and `dialog_height`. `source/editor/ui/dope_sheet_popup.cs`
owns the editor's own three menus - File (save, reload from disk, back to the scene editor), Edit (undo,
redo, cancel the dragged key) and Help (keyboard shortcuts) - and the dialog that asks before a reload
throws away unsaved edits. Every entry decides its enabled flag per frame from the clip and the history,
so a disabled Undo is a fact of this frame rather than a widget that has to be kept in sync.

A dialog is modal through the scrim: it covers the whole frame, so a press anywhere that is not a dialog
button lands on the scrim and only closes the dialog - it can never reach the transport underneath.
Escape closes a dialog first, then a menu, and only then cancels a gesture; Space is ignored while a
popup is open.

Two things had to be fixed while getting this green. The overlay queue was first cleared inside `submit`,
which wiped the popups the builder had just queued, so a menu never dropped a panel; the builder now owns
the clear. And the scrim was authored at 150/255 black, which dims too little once the renderer blends in
linear space - measured 220 to 148 on the preview - so it is 200/255 and the screen behind visibly recedes.

`DEREN_UI_POPUP_DEMO=menu|edit|shortcuts|reload` opens one popup on the demo's first frame, because a
capture run cannot press a title; it changes nothing when unset. The selftest gained a sixteenth phase
that drives the File menu with real presses: the title opens the panel, Reload Clip from Disk asks first
because the clip is dirty, a press on the transport lands on the scrim and leaves the clip alone, and the
Discard button finally reloads from disk, clears the dirty flag and drops the history. Sixteen selftest
lines, exit 0 at 100% and 150%.

### 1.15 The atlas has a report, and a test that forces it to grow

The atlas growing was in place but untested: nothing in the suite could tell "grew correctly and kept
every glyph" from "silently dropped the second screenful". Text that disappears because the atlas is full
also looks exactly like text a font cannot spell, so a frontend had no way to tell the two apart either.

`editor_ui_api` gained an eighth entry, `query_stats(editor_ui_stats*)`, carrying the atlas side in
pixels, the glyphs cached in it and a count of glyphs dropped because even the largest atlas was full
(`ui_overlay::atlas_stats`, `glyphs_skipped` incremented where `grow_atlas` refuses). The table grew to
eight pointers; `managed_abi_version` stays 47 because the table's shape is versioned by its size, which
the managed side checks exactly.

The selftest gained two phases. The first asks for the report, then submits a frame of 700 distinct CJK
code points at 44 logical pixels - more coverage than the original 1024x1024 sheet holds, so the packer
must double it. The second reads the report one frame later, because glyphs are rasterised during
`prepare` and not during `submit`: the side must have grown, nothing may have been skipped, and at least
600 more glyphs must be cached. Growing rescales every recorded `u/v` by the ratio of the two sides, so
the earlier text keeps pointing at its own pixels in the larger sheet - the phase checks the outcome, and
the captures of the sheet after the probe show the labels still legible. Seventeen selftest lines, exit 0
at 100% and 150%.

### 1.16 M2 begins: the scene editor on the own UI library

M1 left the library able to draw a whole editor: layout, text, controls, lists, splitters, popups and a
menu bar. M2 is the other direction - the scene editor itself, rebuilt on that library instead of RmlUi.
The first screen now exists in `source/editor/ui/scene_screen.cs`: menus, the hierarchy, a details
inspector, the project list and the footer, laid out around a slot that paints nothing so the renderer's
scene shows through where the 3D view belongs.

The panels do not own data either, exactly as the RmlUi ones did not. Every frame rebuilds the tree from
`scene_document`, `node_selection` and the asset list, so an undo, a reload or a selection change is
visible on the next frame without an invalidation call, and the outliner rows are the flattened document
in outliner order with their depth as an indent. What is still missing is honest: the outliner has no
folding, the details panel is read-only, and the two panel edges are plain separators rather than
draggable splitters. Those are the next three increments, and the port is only finished - and the RmlUi
editor path only removed - once they are in.

`--editor-ui=next` now opens this screen, `--editor-ui=animation` keeps the Dope Sheet, and
`--editor-ui=classic` still runs the RmlUi shell; the classic path being intact is what makes the two
frontends comparable while the port is in flight. `DEREN_UI_SCENE_DEMO=1` preselects the first placement
so a capture can show a filled inspector, since a capture run cannot click.

The selftest phase (`--editor-ui-scene-selftest`) drives the real input path - a press goes through the
registered callback and the frame's event queue - and asserts that the outliner lists every node, that a
press selects the node under the pointer, that the details panel shows that node's name on the next frame,
that the pointer claim covers the panels but leaves the 3D viewport to the camera, that the wheel over the
viewport does not scroll a list, that the File menu drops its panel, that one press on Help switches
menus, that its Shortcuts entry opens a dialog that Escape closes, and that a press on a project tile
picks that asset. In the demo project that is five nodes and seven assets.

Two layout facts worth keeping: the panel columns are `ui_theme.panel` (opaque) rather than
`ui_theme.window`, which is 226/255 and let the 3D scene bleed through the hierarchy; and a row's children
carry the row's key as a prefix (`scene_row_3_name`), because a press arrives with the key of the deepest
child, so the row index has to be read from a prefix rather than from the exact key.

### 1.17 Folding, multi-selection and an inspector that edits in place

The outliner now folds. `scene_outline_row` carries `has_children` and `closed`, the flattening walk stops
below a collapsed node, and every row reserves a fixed 14 px slot for the fold mark so names stay aligned
whether or not the branch has children. The mark is its own hit target: a press on `..._fold` toggles the
branch and leaves the selection alone, which is what the classic outliner did.

Selection grew the two modifiers the classic path had: Ctrl toggles one node, Shift replaces the selection
with the range between the current primary and the pressed row. One lesson came out of the selftest: Shift
must anchor at the CURRENT primary, because Ctrl-toggle moves the primary to the node it just added; the
range therefore re-anchors before extending.

The details panel stopped being read-only. Pressing a value row turns that half of the row into a text
field seeded with what the row showed, and the field is the same `ui_text_edit` the Dope Sheet uses, so
caret motion, grapheme-aware selection, clipboard, and one undo step per edit all come for free. Enter
applies, Escape cancels, and a press anywhere else commits first, so a click cannot silently throw typing
away. Name edits go through `apply_detail_change(detail_field.name, ...)` and inherit the 256 byte UTF-8
limit the document already enforces. A transform row takes three numbers separated by commas or spaces and
writes them as ONE undo step: the three lanes share a single `node_snapshot`, so Ctrl+Z returns the whole
vector rather than one component at a time. A malformed vector is refused with a notice and the node keeps
its values.

Reuse mattered here: the Dope Sheet's key mapping was extracted into
`handle_text_field_key(ui_text_edit, input_event, Action apply, Action cancel)`, so the scene screen routes
Enter, Escape and Ctrl+Z through exactly the code the animation screen has been exercising for weeks.

Two bugs the new phases caught: `begin_scene_edit` cleared the frame's event queue, which threw
`Collection was modified` because the press that opened the field was still being enumerated, and
`undo_stack` had no way to ask how deep the history was, which the test needs to prove a gesture pushed
exactly one step; `internal int depth => this.undo_entries.Count;` was added for that.

The scene selftest is now three lines and still exits 0 at 100% and 150%:
the screen lists and selects, the outliner folds and the modifiers extend, and the details panel renames a
node and sets a transform as one undo step. `DEREN_UI_SCENE_EDIT=name|position|rotation|scale` opens one
lane in the same capture the preselect uses, so a screenshot can show the inline field.

### 1.18 Three dividers that remember their sizes

The scene screen's panel edges became real drag handles: `ui_controls.vertical_splitter` is the column twin
of the horizontal `splitter` the Dope Sheet already had, six logical pixels wide, accent while hot or while
being dragged, with a 2x28 grip. The outliner's right edge, the details panel's left edge and the project
panel's top edge are all handles now, so the thin 1 px separators are gone.

Sizes live in `workspace_layout`, the record the classic shell already writes to
`<project>/.deren/editor-layout.json`. Three optional fields were added with the values the screen used as
constants (`scene_outliner_width = 250`, `scene_details_width = 300`, `scene_browser_height = 170`) and the
loader clamps them to 160..600, 200..600 and 90..500. Because both frontends deserialise the same record,
the file stays compatible in both directions: a classic-only file simply takes the defaults, and a file
written by the new screen carries keys the classic shell ignores.

The clamps are the interesting part. A divider can never take the last of the 3D view: the outliner and
the details panel are both limited against `scene_viewport_min` (120 px), so one panel cannot grow so far
that the preview disappears, and the browser is limited the same way vertically (500 px or the space minus
the preview, whichever is smaller). Every drag step writes `scene_layout` with a `with` expression, so a
drag that the clamp stops still leaves the last accepted size.

The release is what persists: `save_scene_layout()` runs only when a divider drag ends, not on every move,
so the file is written once per gesture. In the selftest the path is a scratch file, and a selftest run
skips loading the real layout entirely so a machine with a stale window size cannot change the assertions.

The scene selftest is four lines and exits 0 at 100% and 150%: the screen lists and selects, folding and
the modifiers work, the details editor renames and sets a transform as one undo step, and the dividers
resize, clamp and persist.

### 1.19 The outliner's other tools, and the keys that drive them

The outliner caught up with what the classic panel could do. Every row carries a visibility mark of its own
(`scene_row_N_vis`, `o` or `-`), so hiding a node never changes the selection; the caption carries a search
field that filters as you type; and the browser's tile adds a node on a second press within the classic
500 ms window.

The filter has one rule worth writing down: it keeps a match and the ancestors that lead to it, and it
ignores folding while it runs. Folding applies again the moment the field empties, because a filter that
respects a collapsed branch would report fewer results than it matched. The ancestors are inserted at the
position where their first matching descendant was appended, so the flattened list keeps the tree order the
indent implies.

Keyboard: Delete removes the selection through `delete_selected()`, Ctrl+D copies it through
`duplicate_selection()`, F2 opens the name lane with its text selected, and Escape clears the filter before
it closes a menu. All four reuse the host methods the classic shell already proved, including their undo
steps, which is why the selftest can assert that Ctrl+D is exactly one history entry.

`key_code` had no function-key constants, so the twelve F-keys were added next to the existing names
(`f1 = 290` through `f12 = 301`); the class documents that any other code still arrives as its raw GLFW
value.

The filter is a `ui_text_edit` of its own rather than the details lane's shared field. Sharing it would
have made the two focus paths fight: pressing the filter while a details value is open would have had to
commit the value first, and the two would each need their own undo pairing. Two small fields are cheaper
than one clever one.

The scene selftest is five lines now and green at 100% and 150%: the outliner hides a node without moving
the selection, the filter narrows the rows and clears back, Delete and Ctrl+Z restore, Ctrl+D copies as one
step, and a double press on a project tile adds a node.

### 1.20 The components, the scrolling inspector, and Play

Three slices landed on top of the first screen, all of them on the same pattern: the screen reads the
document, calls the host command the classic panel called, and stores nothing of its own.

**The inspector** now covers the components the classic RML rows did: Actor (visible, solid box, moving
solid, trigger volume, trigger target, flags), Pipeline and Behaviour read-only, Camera (vertical field of
view, main camera), Point Light (colour, intensity, range), Material (tint, metallic, roughness, emissive),
Audio Source (clip, volume, loop) and Animation (initial clip, auto play). A numeric or vector value opens a
text field in place; the transaction behind it snapshots once, so a vector edit is one undo step, and the
boolean rows go through the classic toggles (`toggle_volume`, `camera_command`, ...) so the semantics stay in
one place. Component data is not part of `node_snapshot`, so those lanes carry their own before/after
closure and record their own undo labels.

**The inspector scrolls.** Its content is taller than the panel whenever a node carries several components,
and the own UI library has no negative-offset concept, so `scene_details_body` builds only the rows inside
the window: a row above the window is skipped, a row below it is not added at all, and the content height
accumulates as if every row had been built. The wheel walks the column by whole rows and the clamp is
`content - window`. Nothing below the clip exists in the frame, so nothing below it can be hovered or
pressed. A different selection starts at the top; the same node keeps the reader's place.

**Play** works from the new frontend. The header button starts the same out-of-process session the classic
button starts, and the scene screen polls it because the shared update hook returns straight into
`run_scene_ui_frame` and never reaches the classic poll. The session and its thumbnail cache are created for
this frontend too, which the demo branch of `initialize` had skipped.

The scene selftest is eight lines and exits 0 at 100% and 150%: the screen lists and selects, folding and
the modifiers work, the details editor renames and sets a transform as one undo step, the dividers resize
and persist, the outliner hides, filters and drives the keys, the component lanes write through one undo
step, the inspector scrolls and clamps, and the header starts and stops Play.

### 1.21 Picking in the preview, saving, and the default frontend

The preview answers clicks again. The press is remembered only when it lands inside the viewport rectangle
and no panel claims it, and the release picks when the pointer never left a five-pixel slop; a press that
moved belongs to the camera and leaves the selection alone, which is the same split the classic shell used
(`viewport_press` plus `click_slop`). The pick itself is the classic one: the projection turns the click into
a ray, the live tree intersects it, `document.find_runtime` maps the hit to a node, and the instance root is
what gets selected so an actor's mesh does not select the mesh. A miss clears the selection with "Nothing
under the pointer", and `reveal_scene_row` scrolls the outliner to the node, so a pick and the hierarchy
cannot disagree. That scroll reuses `scene_outliner_window_height`, the same arithmetic the wheel uses.

Save/reopen is now a check rather than a claim: the phase renames a root, calls the File menu's
`save_document(absolute)` into `dynamic/scene-save-selftest/saved.scene.toml`, reads the file back through
`scene_instances.try_load` with the project-relative path, and requires the node count and the edited name to
survive. The scratch file and its directory are removed and `document.path` is restored, so the document the
other phases see is untouched. It also proves the writer and the loader are the same contract the classic
shell saved through, without a second format.

The default frontend is the scene screen now. `--editor` opens it; `--editor-ui=classic` still opens RmlUi,
and `--editor-ui=animation` still opens the Dope Sheet. The choice is resolved after the argument loop
(`requested_editor_screen_default` plus `requested_classic_ui`) instead of inside the `--editor` case, so
`--editor --editor-ui=classic` and `--editor-ui=classic --editor` mean the same thing. `--editor-ui=classic`
now also clears the own-UI flags, which is what makes the earlier argument win nothing.

The scene selftest is ten lines: the two new ones cover the preview pick (a click picks, a drag does not
move the selection) and the save/reopen round trip.

### 1.22 The browser search and dragging rows to reparent

The project list searches like the hierarchy does, with its own `ui_text_edit` so one field cannot disturb
the other. `scene_asset_matches` holds the indices that survive the filter, so a row still carries its
ABSOLUTE asset index in its key (`scene_asset_3`), which is what the press handling parses back; filtering
must not renumber the rows, or a double-click would add the wrong asset. The caption shows `matches / total`
next to the field, so a narrowed list cannot be mistaken for a short project, and the wheel scrolls the
filtered count, not the whole library. Both search fields share one painter, `paint_scene_field`, and one
placeholder rule: the hint is drawn only while the field is empty and unfocused.

Rows drag to reparent. The press that selects a row also arms a drag, and only a move past the five-pixel
slop starts it, so the same gesture still selects on a click and the outliner cannot fight the camera's
slop. The drop target is the row under the pointer, or the scene root when the pointer is over empty
outliner space, which is how the classic shell let a drag promote a node out of its parent. The release
hands the node to the classic `reparent_selection`, so the world transform is preserved, the runtime follows
and exactly one undo step records it; the drag never reimplements what already worked. The hovered target is
painted with `ui_theme.selection` instead of the row's own fill, and a drop on the dragged row itself is
ignored.

One bug came out of this slice, and it was a real one: every flexible filler in the outliner and the project
rows was `ui_node.spacer()`, which hard-coded the key `spacer`. The hit test reports the deepest element, so
a click on the blank right-hand part of a hierarchy row arrived as `spacer` and selected nothing - the row
looked clickable only where its text was. `ui_node.spacer` now takes a key, and a row that reacts to presses
passes `key + "_filler"`. That is the same lesson as the prefixed child keys in section 1.16, now enforced by the
builder itself instead of by every caller.

The scene selftest is twelve lines: the two new ones cover the browser search (press focuses, typing
narrows, the rebuilt frame shows the matches and the caption count) and the row drag (the press arms
without starting, the move settles on the target, the release reparents and one undo restores).

### 1.23 Folders and pages in the project browser

The classic browser grouped the library by folder and sliced it into pages of 48 tiles; the new frontend did
neither, so a project with a real asset tree was one flat list. Both came back with the same shape the old
panel had, but the data is rebuilt per frame instead of owned by a panel.

`scene_browser_entry` is a tile that is either an asset (`asset_index >= 0`) or a folder
(`asset_index < 0`, with `directory` true). `scene_browser_catalogue` is the whole library, built once from
`scene_authoring.asset_directory` relative to `library_root()`; `scene_asset_matches` is only the current
folder's slice. A folder tile is listed before the assets of its own directory, sorted with
`StringComparer.OrdinalIgnoreCase`, and a filter searches the subtree of the folder you are standing in,
not the whole library - that is what the classic browser did, and it is what makes "narrow this folder" mean
something.

Two indices had to be kept apart: `scene_browser_first` is a position in the page and the row key keeps the
asset's absolute index (`scene_asset_3`), because the press parses that key back into `assets[]`. Pages are
`page_size = 48`, the caption reads `Project / a / b` on the left and `first+1-first+shown of total` on the
right, and the `..`, `<` and `>` buttons only exist when they can do something. A missing folder (the asset
moved or the library changed under us) walks the current folder up instead of showing an empty page, and a
stale page is clamped back into range.

The library handle is now recorded at startup: `scene_library_ready = asset_authoring.try_open(out
scene_authoring)`. `asset_authoring` is a struct wrapping a native table, so calling `library_root()` before
the table is opened would call through a null pointer; every browser path checks the flag first.

The scene selftest is thirteen lines: the new one shortens the page size to two, walks the project with the
`<` and `>` buttons, checks that the first row of page two really is the entry at that offset and that the
caption starts with the first index on screen, then restores the page size. It also pins both folder
helpers, because a wrong split shows up as an empty browser and nothing else.

### 1.24 The status line and the output tab

Two pieces of the classic shell were still missing from the new frontend, and both were about the same
thing: where the editor talks back.

The footer used to show ONE of two things - the notice if there was one, otherwise the file path - so a
picked asset hid the selection and the dirty flag. It now prints the classic status line:
`path *dirty* | nodes: N | selected: <name> | <notice>`. The dirty flag is read from the document rather than
guessed (`document.dirty` is recomputed from the saved content, so restoring an edit really does clear it),
and the selftest makes an unsaved edit and puts it back through the document's own comparison to prove the
flag moves in both directions.

The bottom dock gained a second tab. `output_log_panel` stayed the owner of the queue (it caps at 200 lines,
which is what the classic panel did), but it now exposes `count`, `text_at`, `error_at` and `node_at` so a
frontend can render it without owning it. The scene screen's `Output` tab builds one row per line, newest
first - hence the row key carries the line id and the list index runs backwards over `count - 1 - index` -
and colours an error line with the new `ui_theme.error`; a line the runtime tagged with a node selects that
node when pressed, which is how a play failure walks you to the actor that raised it. `Clear` empties the
queue and the tab says `Nothing has been logged yet.` instead of showing an empty list. The wheel is flipped
for this tab so scrolling down reads newer lines.

The scene selftest is fifteen lines: the status line (path, dirty flag both ways, node count, selection, the
notice appended rather than replacing the line) and the output tab (the caption count, the newest row, a
press that follows the line to its node, and Clear leaving the tab empty).

### 1.25 The environment rows and their list

The classic shell edited the environment through dropdowns: the scene-wide map, one row per custom lane the
document carries, and a per-object row in the inspector. All three are now rows of the new inspector, and the
dropdown became a popup - the same overlay layer the menus use, hung under the row that opened it.

That split keeps the responsibilities where they were. A row only says which lane it edits; the list is
built from the catalogue the classic panel offered (the `.hdr` files under `<library>/environments`, in name
order) plus the entry that means "no map" and the one that opens the platform file picker. Picking an entry
goes through the host's existing commands - `apply_environment_selection` for a lane and
`apply_node_environment_selection` for an object - so lane allocation, map sharing between objects, the
"all 15 lanes are in use" refusal and the single undo step that writes a lane and points a node at it are all
still in one place. The lane rows only exist while the document carries a lane, which is what keeps fifteen
empty rows from burying the inspector; the scene map row and, with a selection, the object map row are always
there.

Two behaviours are worth naming. A press on anything that is not an entry closes the list and is spent, the
way a dropdown behaves in the classic shell, and Escape closes it before it clears the searches. And a capture
run cannot press a row, so `DEREN_UI_SCENE_COMBO=scene|node` opens the list for the image; it waits for the
first arranged frame, because the list has to hang under the row's own rectangle rather than under a guess.

The scene selftest is sixteen lines: the new one presses the scene map row, requires the list with the
procedural sky first and its first entry in the rebuilt frame, closes it with a second press, writes a lane
through the host's own command and requires the row to appear and to vanish again after one undo, then opens
the object row's list and closes it with Escape. It restores the lanes and the undo depth itself, so a
failure here cannot leak into the phases after it.

### 1.26 The behaviour rows

The classic inspector built its script rows in `script_inspector` and looked the field up again on every
message, because that panel owned the selected node. This screen rebuilds its rows every frame, so a row
carries the parameter index and the apply path resolves the field from the definition the node's binding
names. Two pieces of the classic panel are therefore not reused: the `fields` dictionary and the markup. The
transactions are: `set_script_settings` for a binding and a parameter of an ordinary node, and
`set_instance_override` for a placement's override, which re-derives every expanded node from the source
snapshot. Both were already there, so a script change and an override stay one undo step each.

The rows follow the distinction the classic panel made between kinds. A binding row opens the catalogue -
"No script" first, then one entry per definition, labelled with the type name after its last dot. A boolean,
node or asset parameter is a row that opens a list of the values its kind allows: `true`/`false`, the scene's
nodes flattened with their path, or the project's assets. The remaining kinds - an integer, a number, a
vector - edit in place, seeded with the value the row shows, exactly like a transform lane. An expanded
instance node shows its source scene and binding as read-only rows and gains a `From source` button under
every parameter it overrides.

One thing this slice needed besides the rows: the catalogue itself. The classic shell creates it while it
opens and the new frontend returns before that happens, so the first frame of the scene screen now creates
`script_catalog` over `<project>/scripts` and reloads it; without that every behaviour read as missing.

A second finding is worth recording. The outliner phase's "double press on a project tile" step used to add
the first tile in the list, which in this project is the imported `bistro` scene, and that import aborted the
process with `std::bad_alloc`. The step now adds the asset the first outliner row already uses, so it tests
the double press rather than the glTF importer, but the bistro import failure is a real defect outside this
milestone and is left open.

The scene selftest is seventeen lines: the new one presses the binding row, requires the catalogue with "No
script" first and one entry per definition, picks "No script" and requires one undo to bind the script again;
presses a boolean parameter, picks the other value and requires one undo to put it back; and presses a text
parameter, types `7`, presses Enter and requires the parameter and its undo.

## 2. Decisions taken

1. The animation editor must not share the scene editor's layout. Scene-only entry points (import,
   collision, trigger, audio, pipeline, arbitrary asset import) must not exist while animating
   (user, 2026-11-24). The chosen shape is a separate `animation_editor.rml` shell.
2. The animation editor must be able to create animation, not only open existing motion (user,
   2026-11-24). Creating means writing a new `motion.gltf` from the asset's skeleton instead of
   requiring a pre-existing motion asset.
3. The editor UI must be its own immediate-mode UI layer in C#, rendered by the engine, with RmlUi
   reserved for the runtime game HUD (user, 2026-11-24). Rationale recorded below.

None of these three is implemented yet. The current animation mode is still a dock tab inside
`editor.rml`, toggled by `display` styles and guarded by `animation_mode` checks in C#. That is the
pattern the user rejected and is scheduled for removal.

### Why the editor owns its UI

Every comparable editor owns its UI layer: UE uses Slate for the editor and UMG for the game,
Unity uses UIElements with its own renderer, Blender uses its own C widget layer, Maya uses its
own widget layer over Qt. Third parties may supply layout or text shaping, never the editor
framework.

The concrete reference notes for the UI layer itself live in `docs/ui_library_reference.md`: what
Blender 5.2.5's rewritten `ui::Block`/`ui::Button` layer and UE 5.8.2 Slate each do, the ten
patterns deren copies, the seven it deliberately does not, and the M0 scope for the new layer. The
overall phased plan (architecture, decisions, P0-P4 scope and estimates, risks, verification) is in
`docs/editor_ui_plan.md`, and the library design itself (retained declarative element tree, flexbox,
stylesheet subset, HarfBuzz text and IME, SDF rendering, damage tracking, ABI, S0-S5 milestones) is
`docs/editor_ui_library.md`.

RmlUi reached its limit here because the managed side only sees a ten-entry function table
(`source/scripting/rml_ui_api.cppm`). Every new interaction costs an ABI change: keyboard events
do not exist at all, so the editor has no shortcuts; the timeline cannot be self-drawn without a
new texture sentinel; trees, drag and drop, context menus and scrollbars are all missing. Patching
around that with `display` toggles and mode guards in a shared markup file does not scale (the dock
tab list already grew to three entries with a mode check, and the animation strip, timeline body
and mode button all swap their text or visibility from C#).

## 3. Blender reference notes

Blender 5.2.5 source is at `D:\blender\blender-5.2.5`. The animation editor's UI should mirror its
Dope Sheet, not an invented layout.

### 3.1 Regions

`SpaceDopeSheet` has five regions: HEADER (top, `RGN_TYPE_HEADER`), FOOTER (playback controls,
`RGN_TYPE_FOOTER`), CHANNELS (left channel list, `RGN_ALIGN_LEFT`), UI (right sidebar, `RGN_ALIGN_RIGHT`),
and WINDOW (the keyframe grid). See `source/blender/editors/space_action/space_action.cc`.

### 3.2 Header

`DOPESHEET_HT_header` (`scripts/startup/bl_ui/space_dopesheet.py`) draws the editor-type icon and the
mode selector, then the menus View / Select / Marker / Channel / Key / Action. The Action mode
header adds an action selector, then one icon row of filters (Only Show Selected, Show Hidden,
Only Show Errors), a filter popover (Search, Summary, Filter by Type), a snapping toggle with a
popover, proportional editing with falloff, and an overlay selector. Markers, groups, shape keys,
NLA and the layered-action selector have no counterpart in deren's clip model and must be omitted,
not invented.

### 3.3 Rows and keys

`ANIM_UI_get_channel_height() = 0.8 * widget_unit`, skip `0.1 * widget_unit`, name column
`10 * widget_unit`, expand widget and icon `ICON_WIDTH` each
(`source/blender/editors/animation/anim_channels_defines.cc:5148-5190`). Keyframes are diamonds
whose size depends on the keyframe type: 1.0 normal, 1.2 extreme, 0.925 move-hold, 0.85 breakdown,
0.8 jitter, 0.75 generated (`source/blender/editors/animation/keyframes_draw.cc:45-175`). deren clips
have no keyframe types, so only the normal size and the selected/unselected colors apply.

### 3.4 Transport

`scripts/startup/bl_ui/space_time.py`, `playback_controls`, in order: playback settings popover,
keyframing settings popover, auto-key toggle and its popover, jump to start, previous keyframe,
play reverse, play (or pause), next keyframe, jump to end, previous frame, next frame, jump popover,
snap playhead toggle and popover, current frame field, preview range toggle with its start/end.

### 3.5 Sidebar

In Blender 5.2.5 `source/blender/editors/space_action/action_buttons.cc` is disabled entirely, so
the Dope Sheet sidebar has no Active Keyframe or Active F-Curve panel; numeric key editing belongs
to the Graph Editor. A keyframe inspector in the animation editor would be an invention.

## 4. Next milestones

The authoritative design, scope and milestone order are in `docs/editor_ui_library.md` (revised by
the author after the first draft). In short:

- M0: the library foundation plus one real screen on it, the Dope Sheet workflow - open a motion,
  channel rows, ruler, visible-row virtualization, key selection and time drag, undo, and a minimal
  motion save/reopen when the write-back lands in this milestone. **Status: done**, see section 1.7.
- M1: the text pipeline (HarfBuzz + FreeType, atlas, grapheme/cluster editing, IME prototyped
  alongside) and the reusable control set. The theme is typed in M0; the stylesheet subset comes
  later.
- M2: scene editor parity, then removal of the editor's RmlUi path.

`docs/editor_ui_plan.md` keeps the repository-level rationale, the reuse notes, the ABI sketch and
the risk list; its P0-P4 phase list is background only, superseded by the M0/M1/M2 order above.

## 5. Open items

- `source/scripting/rml_ui_api.cppm` carries an uncommitted `reopen` tail entry that was added for
  the abandoned "second RmlUi document" plan. The managed mirror now accepts any whole-entry prefix
  of the table (mandatory entries first), so the tail can stay without breaking the classic path;
  removing it later is a plain ABI change, not a fix.
- The animation editor still needs: its own screen on the new layer, creating a motion clip from
  the asset skeleton (clone the asset's existing `motion.gltf` nodes and `extras.deren_motion`,
  empty `animations`; assets without a motion asset need the skeleton hash computed from the glb,
  deferred), keyframe insert/move/value editing with a per-motion undo stack, and glTF write-back
  that preserves `extras.deren_motion` and the buffer layout and survives the loader's fingerprint
  checks.
- `deren.exe run . --editor-animation-toggle N` is now a diagnostic flag used to reproduce the
  buffer-recycling case; it can be dropped once the animation editor screen replaces the mode
  toggling.

## 6. Verification

```powershell
cmake --build build
ctest --test-dir build -j 4
cd D:\deren\build\editor-animation-demo
D:\deren\build\deren.exe run . --editor-selftest-animation-mode
D:\deren\build\deren.exe run . --editor-ui-selftest
D:\deren\build\deren.exe run . --editor-ui-selftest --editor-ui-scale 150
D:\deren\build\deren.exe run . --editor-animation-toggle 10
D:\deren\build\deren.exe run . --editor
D:\deren\build\deren.exe run . --editor-ui=next --capture-frames 30
cd D:\deren
build\deren.exe run build\source\scripting\test_game --ui-demo --capture-frames 6
```

The IME probe needs a real window, so it is a windowed capture and reads the log rather than stdout:

```powershell
cd D:\deren\build\editor-animation-demo
$env:DEREN_IMEPROBE='1'
..\deren.exe run . --editor-ui=next --capture-frames 12 --capture-windowed
Remove-Item Env:\DEREN_IMEPROBE
Select-String -Path deren-debug.log -Pattern 'ime: ' | Select-Object -Last 10
```

The editor selftest matrix (all expected to exit 0): `--editor-selftest`, `--editor-selftest-collision`,
`--editor-selftest-audio-source`, `--editor-selftest-triggers`, `--editor-selftest-input-actions`,
`--editor-selftest-project`, `--editor-selftest-authoring`, `--editor-selftest-instances`,
`--editor-selftest-lights`, `--editor-selftest-browser`, `--editor-selftest-animation`,
`--editor-selftest-validation`, `--editor-selftest-animation-mode`.

### 6.1 Real IME acceptance (manual, user-run)

The synthetic probe cannot prove what a real IME does, so the acceptance run is manual. Start the
editor windowed, click the clip name field in the Dope Sheet header, type Pinyin and check:

1. the composition appears in the field with an underline and the caret stays after it;
2. the candidate list sits at the caret, not at the window corner, and follows the caret while
   space/backspace changes the composition;
3. Enter commits: the committed text lands in the field as one undo step (Ctrl+Z removes the whole
   committed run, not one character);
4. Ctrl+Z after a commit reaches the text before the composition;
5. Escape during a composition cancels it without touching the name;
6. Enter on the field applies the name, then `Save Motion` writes it: reopen the motion and the
   header shows the renamed clip.

```powershell
cd D:\deren\build\editor-animation-demo
..\deren.exe run . --editor-ui=next
```

