# Editor UI layer: overall plan

The plan for replacing the editor's RmlUi shell with an editor-owned UI layer drawn by the engine,
and for the animation editor that will be its first real screen. Reference material for the two
codebases this plan borrows from is in `docs/ui_library_reference.md`; the current implementation
state is in `docs/editor_progress.md`.

The library design itself (retained declarative element tree, flexbox, stylesheet subset, HarfBuzz
text and IME, SDF rendering, damage tracking, ABI, milestones) is `docs/editor_ui_library.md`, which
the author revised after the first draft. That document is authoritative for the library design and
for the milestone order (M0 Dope Sheet workflow, M1 text and reusable controls, M2 scene parity); it
supersedes this plan's sections 3-6 where they differ. This plan keeps the repository-level
rationale, the reuse notes and the risk list.

Status: proposal, nothing implemented yet.

## 1. Goal and non-goals

Goal: the editor owns its UI, the same way Blender, UE, Unity and Maya own theirs. The editor can
therefore grow dense widgets (dope sheet, curve editor, outliner drag and drop, context menus,
keyboard shortcuts) without an ABI change for each one, and the animation editor is a screen, not
a set of `display` toggles inside the scene editor's markup.

Non-goals:

- Replacing the game's runtime HUD. `rml_overlay`, `rml_document` and `rml_scripting` stay for
  game UI; games keep their markup. Only the editor stops using them.
- A general purpose UI toolkit for third parties. This layer serves `source/editor`.
- Widget look-and-feel work beyond a plain dark theme. Theme data exists from day one, beautifying
  does not.

## 2. What already exists and gets reused

- `source/ui/rml_overlay.{cppm,cpp}` is a complete Vulkan overlay renderer for RmlUi: one pipeline,
  per-frame vertex buffer, a batch list of (texture, clip rect, first vertex, count), a sampler,
  and a special `deren://scene` texture that samples the frame's 3D image. The new layer reuses
  this architecture; it does not need a new renderer style.
- `source/app/run.cpp` installs the overlay generically:
  `runtime.set_output_overlay(&*game_ui, &deren::ui::rml_overlay::record)` (`:1663`), with
  `runtime.require_scene_color(...)` (`:2343`) and `set_output_overlay`. Any object exposing a
  record function can be an overlay, so a second overlay class can replace RmlUi for the editor.
- `deren.input_events` (`source/scripting/managed/Api/input/input_events.cs`) already delivers
  mouse move, mouse button, scroll, key down/up, character (Unicode codepoint) and focus-lost to
  managed code, with `key_code` constants. The editor already installs that callback
  (`source/editor/editor_host.cs:588`) for camera navigation. This is the input source for the new
  layer; no new input ABI is needed.
- `window_control` offers minimize/maximize/close/begin_drag for the title bar.
- The editor is already split into partial-class files under `source/editor/` (`editor_host.*.cs`,
  `panels/`, `editing/`), so screens can be migrated one at a time behind a flag.

What is missing and must be built: a UI context with layout/text/widgets/input handling, a text
atlas, and a managed-to-native submission ABI plus a `ui_overlay` that records it.

## 3. Target architecture

```
window events (GLFW via input_events)
        |
        v
ui_context            (C#, source/editor/ui/)
  begin_frame(input) -> screen builds widgets -> end_frame()
  layout solver, draw list, clip stack, id stack
  retained state only: focus, active, drag, text edit, scroll, popups, caches
        |
        v
ui_draw_list  (vertices, indices, draw commands: texture, clip, blend, layer)
        |
        v
ui_overlay    (C++, source/ui/ui_overlay.cppm, in deren_gui_vulkan)
  one vertex/index upload, one pipeline (solid + text atlas), scissor clips,
  optional scene texture for the 3D viewport element
        |
        v
frame recording (same hook as rml_overlay::record)
```

Screens on top of `ui_context`: `animation_editor` (new), then `scene_editor` screens migrated
from `panels/*`, then `dock`, `modal`, `menu`, `tooltip`.

## 4. Decisions and why

1. **Immediate-mode with caller ids, not retained widgets.** Blender rebuilds every widget each
   redraw and migrates interaction state from the old block. deren avoids the migration by taking
   an explicit id for every widget; retained state is a few dictionaries keyed by that id. This
   gives Blender's cheap rebuild without its identity matching.
2. **Layout: Blender's row/column solver.** Fixed, auto and weighted sizes, iterative clamping,
   padding and gap, plus splitter and scroll areas. Roughly 150 lines, predictable, and it is what
   makes Blender's dialogs behave. Rejected: a flexbox port (heavier, harder to reason about) and
   UE's measure/arrange cache (only pays off when layout is retained across frames).
3. **One draw list with a sort key.** Commands carry layer, clip rect, texture and blend; adjacent
   commands with equal keys merge into one draw call, mirroring `FSlateRenderBatchParams`. Rejected:
   per-widget draw calls (state churn), and a retained element cache (UE only needs it because its
   widget tree persists).
4. **Text: FreeType glyph atlas, no shaping at first.** This is what Blender's editor does today
   (no HarfBuzz in `blenfont`), so it is sufficient for an editor with English labels. The text API
   is width-agnostic (`measure`, `draw`, `caret` from a byte offset) so a shaping backend can be
   added for CJK later without touching call sites. UE's HarfBuzz path is the reference for that
   step.
5. **Input: one event struct, one reply object.** Events are dispatched top-down through the
   focus path; the handler returns `handled/capture/focus/drag` and the context applies it once,
   like `FReply` + `ProcessReply`. Rejected: per-widget side effects during dispatch.
6. **Popups, menus, tooltips owned by the context**, drawn last with a modal layer. Rejected:
   OS windows (UE does this; too heavy for whose needs) and separate markup files (the RmlUi
   pattern that caused the current guards).
7. **Shortcuts in one table, not per widget.** Ordered by focus path then a global table. UE's
   per-widget `ProcessCommandBindings` is explicitly not copied because it makes precedence
   implicit in the widget tree.
8. **Theme is data**: widget kind + state -> colors and metrics in one table, like Blender's
   `WidgetType`/`ThemeUI`. No per-call colors in screen code.
9. **DPI is an explicit value** passed to the context each frame, not a global like
   `U.widget_unit`. Layout is solved in dps; vertices are emitted in pixels.
10. **Undo is pushed once per gesture at commit**, with a caller-supplied label; widgets report
    the commit instead of writing history. This mirrors Blender's `BUT_UNDO` + deferred
    `ED_undo_push`.
11. **Long lists virtualize by visible window**, as Blender's `uilist_prepare`. The dope sheet has
    67k keys in the sample clip, so the key grid must never create per-key widgets.
12. **The animation editor and the scene editor are separate screens**, not dock tabs inside one
    markup. That was the user's objection: an animator must not be able to reach import, collision,
    trigger, audio or pipeline controls.
13. **Only one overlay is installed at a time.** During migration the editor picks its UI path at
    startup (`--editor-ui=classic|next`), so the two stacks never draw into the same frame. This
    avoids a composite overlay and keeps regressions attributable.

## 5. Rendering and ABI

Managed to native, added to `deren_gui_vulkan` and exposed as a small table like `rml_ui_api`:

- `ui_begin(width, height, scale, slot)` / `ui_end()` frame scope.
- `ui_upload_vertices` (transient buffer, one copy per frame).
- `ui_draw_commands` (texture id, clip rect, first index, index count, layer).
- `ui_upload_texture(id, pixels, width, height, format)` for the font atlas (and icons later).
- `ui_scene_texture()` reports the frame's 3D image handle for the viewport element, or the
  overlay learns it through the same `scene_texture` mechanism RmlUi uses.

`ui_overlay` keeps `rml_overlay`'s per-frame resources: vertex buffer, batch vector, sampler,
pipeline. A solid-colour fragment path uses a 1x1 white texture so there is one pipeline, as
`rml_overlay` already does for its markup.

## 6. Phases

Superseded by the milestone order in `docs/editor_ui_library.md` (M0 first Dope Sheet workflow, M1
text pipeline and reusable controls, M2 scene editor parity). The phase list below records the first
draft's order and the reasoning behind each step; read it as background, not as the current plan.

Estimates are working days for one developer, and are deliberately coarse.

**P0 - skeleton (3-5 days).** `ui_context` with id stack, frame lifecycle, layout solver
(row/column/box, fixed/auto/weight, padding, gap), draw list, clip stack, reply model; `ui_overlay`
with vertices + commands + white texture; one test screen with a splitter, a scrolling list of
2000 rows, a text field and a button row. Exit: the test screen runs at 60 fps in the editor window
with correct HiDPI scaling and no RmlUi involved.

**P1 - controls (4-6 days).** Button, toggle, text field (Latin, caret, selection, clipboard),
tree row with expand/collapse, scroll area and scrollbar, splitter, dropdown, popup menu, tooltip,
modal. Text atlas with a free list. Theme table. Exit: a control gallery screen and a selftest that
drives every control through synthetic events.

**P2 - animation editor on the new layer (8-12 days).** Blender-faithful Dope Sheet: header (menus
View/Select/Channel/Key/Motion, filter icons, search, snap), channel list with hierarchy, key grid
with diamonds and virtualized rows, footer transport in Blender's order, sidebar with the Action
panel, asset picker that only lists animatable assets, create-motion flow, key insert/delete/move
with undo, then the `motion.gltf` writer and the live reimport. Exit: an animator can open an
asset, create a motion, edit keys, undo, save, and see the preview update, all from the new layer.

**P3 - scene editor migration (10-15 days).** Replace `editor.rml` screens one at a time on the new
layer: menu bar, toolbar, outliner, details, content browser, viewport gizmos and tool buttons,
project panels, dialogs, output log. The scene data model and every command stay as they are; only
the widgets change. Exit: `--editor-ui=next` reaches feature parity on the selftest matrix.

**P4 - removal (2-3 days).** Delete the editor's RmlUi usage: `editor.rml`, `panels/*` markup
paths, `set_dock_tab`/`animation_strip` guards, the `animation_mode` UI toggles, and the editor's
`rml_document`. Keep `rml_overlay` for game HUD. Exit: the editor links no RmlUi document; the
`reopen` tail in `source/scripting/rml_ui_api.cppm` is either used by games or reverted.

**P5 - later.** Graph editor (curve editing) on the same layer, reusing the key grid and the
numeric drag rules from `docs/ui_library_reference.md`.

Total to the end of P4: roughly 27-41 working days, with P2 carrying most of the product risk
because it touches animation data and the glTF writer, not because of the UI layer.

## 7. Risks

- **CJK input.** Text fields without shaping and IME cannot edit Chinese. Mitigation: English
  labels first; the text API keeps a shaping boundary; schedule HarfBuzz + IME before any
  localisation work. This is the one risk with no cheap workaround.
- **Regression during migration.** Two UI stacks exist until P4. Mitigation: startup flag, one
  stack per run, the existing selftest matrix must stay green for the classic path until it is
  deleted, and a new screenshot check per migrated screen.
- **Text quality.** FreeType plus manual advance has no kerning and no shaping; some labels look
  slightly loose. Acceptable for an editor; if it is not, adding HarfBuzz early is a contained
  change inside the text module.
- **Performance of the dope sheet.** 67k keys. Mitigation: visible-window virtualization, a single
  indexed quad buffer for diamonds, and no per-key widgets.
- **Scope creep into a toolkit.** Mitigation: the layer lives in `source/editor` (not a separate
  library), has no public API guarantee, and only grows features a screen needs.
- **Losing the RmlUi safety net too early.** The animation editor is new, so it can go first; the
  scene editor only migrates in P3 after P1 and P2 prove the layer.

## 8. Verification

- `--editor-selftest-*` flags keep working: one per screen on the new layer, driven through the
  same synthetic-event entry the selftests already use.
- A screenshot check per migrated screen (the editor already captures frames headlessly).
- Counters per frame logged in the selftest: draw calls, vertices, ui build microseconds. Budget:
  under 1 ms UI build and under 10 draw calls for the animation editor at 1600x900.
- The full `ctest` suite and the existing editor selftest matrix must stay green after each phase.

## 9. Questions this plan does not answer

1. Does the animation editor ship on the new layer before the scene editor is migrated (this plan
   assumes yes, because it is the user's current priority), or should the scene editor migrate
   first so only one screen ever runs on two stacks?
2. When is CJK text entry needed? If soon, HarfBuzz and an IME bridge move into P1 instead of P5.
3. Are menus in the window's title bar (like the current menu row) required, or is a normal menu
   bar inside the client area acceptable? Platforms differ on native menu support.
