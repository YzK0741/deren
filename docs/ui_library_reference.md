# Editor UI library reference

What the two mature editor codebases in this workspace do, what deren should copy from them,
and what deren should deliberately not copy. Read this before touching the editor UI layer.

Sources: Unreal Engine 5.8.2 at `D:\UnrealEngine-5.8.2-release` (Slate and SlateCore) and
Blender 5.2.5 at `D:\blender\blender-5.2.5` (interface module and blenfont). Line numbers are
from those trees and will drift; the file names stay useful.

This document is written for the plan in `docs/editor_progress.md`: deren gives its editor its own
immediate-mode UI layer drawn through `deren_gui_vulkan`, and retires the RmlUi editor shell.

## 1. Blender 5.2.5 interface module

Blender 5.2.5 rewrote the old C `uiBlock`/`uiBut` layer as C++ `ui::Block`/`ui::Button`.

- `ui::Button` (`source/blender/editors/interface/interface_intern.hh:195`) holds `Layout *layout`,
  `flag`, `drawflag`, `type`, `str`/`drawstr`, a block-relative `rctf rect` (`:236`), `poin`,
  `rnapoin`/`rnaprop`/`rnaindex`, `func`/`apply_func`, `tip`, `emboss`, `icon`, and
  `HandleButtonData *active` (`:198-363`).
- Variant payloads are derived types, not a `void*`: `ButtonText`, `ButtonNumber`,
  `ButtonNumberSlider`, `ButtonColor`, `ButtonTab`, `ButtonSearch`, `ButtonDecorator`,
  `ButtonProgress`, `ButtonSeparatorLine` (`:382-495`).
- `ui::Block` (`:674`) owns `Vector<std::unique_ptr<Button>> buttons_ptrs` and
  `ListBaseT<LayoutRoot> layouts`, plus `oldblock`, `endblock`, `bounds_type` (`:677-757`).

The rebuild model is the important part: every redraw allocates a fresh block, builds all buttons
and layouts, then `block_end_ex` (`interface.cc:3939`) migrates interaction state from the old
block (`block_update_from_old`, `:1980`), resolves layouts, aligns, computes bounds and sets
`endblock = true` (`:2097`, `:2134-2192`). Layouts are scratch and freed after resolve
(`interface_layout.cc:5846`); only buttons survive, through migration. Buttons therefore cannot
know their pixels while being created.

Layout is a row/column solver with explicit weights:

- `layout_add_but` (`interface_layout.cc:5721`) only appends an item; it marks a button fixed-size
  when width <= `2 * UI_UNIT_X` and the text is empty.
- `LayoutRow::resolve_impl` (`:3812`) charges inter-item `space_`, repeatedly fits or clamps each
  item to its minimum or fixed size, distributes leftover pixels and repeats until stable; columns
  are the same at `:4037`. Expansion is `LayoutAlign::Expand` against Right/Center.
- `UI_UNIT_X == UI_UNIT_Y == U.widget_unit` (`editors/include/UI_interface_c.hh:2346`), default
  `buttonspacex = 8` scaled by `UI_SCALE_FAC` (`interface_style.cc:109`, `:445`).

Drawing is centralized, not per-button: `draw_button` (`interface_widgets.cc:5511`) picks a
`WidgetType` from theme and button type; the type is a small vtable of `wcol_theme`, `wcol_state`,
`state`, `draw`, `custom`, `text` (`:262-286`, `:5230`, `:5808`). Theme is pure data
(`uiWidgetColors` and `ThemeUI`'s ~20 sets, `makesdna/DNA_theme_types.h:184-225`).

Painting is hybrid: `immBegin/immEnd` for simple shapes, instanced rounded boxes through a shader
batch (`interface_widgets.cc:1070-1143`, `interface_draw.cc:75`), and one glyph batch per block
(`interface.cc:2305`). Hidden, scrolled-out, and oversized buttons are skipped per draw
(`:2315-2341`). Adjacent aligned buttons share rounded corners through corner flags
(`widget_roundbox_set`, `interface_widgets.cc:5421`).

Interaction state lives outside the widget: `HandleButtonData` (`interface_handlers.cc:417`)
holds the value, the original value, drag deltas, text-edit state, timers and cancel/applied flags.
Hit testing is a linear reverse scan (`interface_query.cc:269`, `:331`).

Text is FreeType plus a hand-rolled glyph cache, with no HarfBuzz anywhere in `blenfont`: font
caches are keyed by size/weight/spacing (`blf_glyph.cc:85`), glyphs by charcode and subpixel
(`:172`), one GPU atlas per cache appended row by row (`blf_font.cc:268`), font sizes quantized to
1/64 px (`:2331`). Blender's editor UI text is unshaped by design.

Lists virtualize inside the template system: `uilist_prepare` computes a visible row window from
`list_scroll` and only those rows get buttons
(`editors/interface/templates/interface_template_list.cc:526`, `:724`); the scrollbar is a button
bound to the same integer (`:799`). Panels (`panels_end`, `interface_panel.cc:1982`) then offset
and align their block.

Commit and undo are split on purpose: `button_value_set` writes the value immediately
(`interface.cc:2840`), while `apply_but_undo` (`interface_handlers.cc:1040`) pushes undo only for
buttons flagged `BUT_UNDO`, with a label derived at the call site and executed through an
`AfterFunc` (`:1142-1246`), so one user gesture yields one undo step.

## 2. Unreal Engine 5.8.2 Slate

Slate is retained-mode widgets with an explicit retained draw-element cache, and the editor UI is
itself drawn as a stream of typed elements.

- Every widget derives from `SWidget` and implements `ComputeDesiredSize` and
  `OnArrangeChildren`; layout is measure then arrange with a cache (`CacheDesiredSize`,
  `ArrangeChildren`, `SlateCore/Public/Widgets/SWidget.h:694`, `:759`, `:774`, `:880`, `:1780`).
  `ComputeDesiredSize` is documented as a developer aide, not a robust measurement source.
- Box layout exposes slot sizing as `AutoWidth`, `FillWidth(coefficient)`, `FillContent(...)`
  (`SlateCore/Public/Widgets/SBoxPanel.h:181`, `:192`, `:206`), so weights are authored on the
  slot rather than computed in the child.
- Paint produces draw elements typed by `EElementType`: box, text, shaped text, line, spline,
  gradient, viewport, border, rounded box, custom, custom verts, post-process pass, non-mapped
  (`SlateCore/Public/Rendering/DrawElementCoreTypes.h:34-57`). Slate 5.8 added element caching
  (`FSlateCachedElementData`, `FSlateCachedElementsHandle`, `FDeferredPaint` in
  `SlateCore/Public/Rendering/DrawElements.h:28-139`).
- Batching is a key comparison: `FSlateRenderBatchParams::IsBatchableWith` compares `Layer`,
  `ShaderParams`, `Resource`, `PrimitiveType`, `ShaderType`, `DrawEffects`, `DrawFlags`,
  `SceneIndex`, `ClippingState` (`SlateCore/Public/Rendering/SlateRenderBatch.h:30-41`); each batch
  is a contiguous run of vertices and indices inside shared arrays (`:44-149`).
- Text does use HarfBuzz: `hb_shape` at `SlateCore/Private/Fonts/SlateTextShaper.cpp:775`, with
  glyph atlases in `SlateFontRenderer`/`SlateFontCache`.
- Input is routed events over a focus path: `FSlateApplication::ProcessKeyDownEvent`
  (`Runtime/Slate/Private/Framework/Application/SlateApplication.cpp:4961`) tunnels
  `OnPreviewKeyDown` (`:5025`), then bubbles `OnKeyDown` (`:5047`), then falls back to an
  unhandled handler (`:5069`). Pointer events use the same router; the route loop stops at the
  first reply that claims `IsEventHandled` (`:457`).
- `FReply` is a value-type command object, not a bool: `CaptureMouse`, `SetUserFocus`,
  `ReleaseMouse`, `BeginDragDrop` (`SlateCore/Public/Input/Reply.h:28-137`), applied centrally in
  `ProcessReply` (`SlateApplication.cpp:3387`) so widgets never touch the OS.
- Shortcuts have no central dispatch: each widget calls `FUICommandList::ProcessCommandBindings`
  from its own `OnKeyDown` (e.g. `Editor/ContentBrowser/Private/SAssetView.cpp:2934`);
  `FUICommandList` maps actions, chains parents and gates on `CanExecute`.
- Menus are real windows (`FMenuStack::CreateNewWindow`, `MenuStack.cpp:494`), tooltips are
  spawned windows too (`SpawnToolTip`, `SlateApplication.cpp:3747`).
- Numeric drag is capture plus a threshold plus a deferred commit: mouse down captures, mouse move
  accumulates deltas only after `GetDragTriggerDistance`, mouse up commits through
  `NotifyValueCommitted`, and a short drag falls through to text entry
  (`Slate/Private/Widgets/Input/SSpinBox.cpp:330`, `:465`, `:367`, `:380`).

## 3. What deren copies

1. One rebuild per frame with a declarative tree, and interaction state kept outside the widgets,
   keyed by a stable id. This is Blender's block model without the old-block migration: deren's ids
   are hashes/strings the caller supplies, so no per-widget identity matching is needed.
2. Blender's row/column solver with fixed, auto and weighted sizes and iterative clamping. It is
   about 150 lines and predictable, and it is what makes dialogs and toolbars behave.
3. A widget style table keyed by (widget kind, state) plus a data theme, like `WidgetType` and
   `ThemeUI`, instead of per-call color arguments.
4. A single draw list with typed commands and a sort key, batched like `FSlateRenderBatchParams`
   (layer, clip, texture, blend). Adjacent commands that share a key become one draw call.
5. Blender's central draw dispatch and its culling rules: skip commands that are hidden, clipped
   or off-screen, and clip children to the parent rect.
6. UE's reply model in simplified form: an input event produces a result struct with
   `handled`, `capture`, `focus`, `cursor` fields that the context applies once, after dispatch.
7. UE's drag rule: capture on press, accumulate only after a drag threshold, commit once on
   release; a short press becomes a click or text edit.
8. Blender's deferred undo pattern: the widget reports the commit, the caller pushes one undo step
   with a label.
9. Blender's virtualization window for long lists (channel rows, asset grids): only visible rows
   are submitted.
10. Text as FreeType glyphs in one atlas per font/size, like Blender, with the shaping boundary
    left open (see below).

## 4. What deren does not copy

- Blender's old-block state migration: with caller-supplied ids it is unnecessary, and it is the
  part of Blender's UI that is hardest to follow.
- Blender's linear hit test over all buttons: deren keeps a per-region list and a small grid index
  for dense areas (timeline keys), because the key count can reach thousands.
- Blender's append-only glyph atlas with full clears: deren's atlas gets a free-list and repacks.
- UE's "every widget dispatches commands itself": shortcut precedence should be resolved by the
  context from one table (focus path first, then global).
- UE's full attribute/invalidation machinery: with a rebuild-per-frame model, invalidation only
  needs to mark the frame dirty.
- Blender's per-region temporary `ARegion` plumbing for popups: whose context owns a popup stack.
- Text without shaping when CJK input matters. deren ships Latin-only labels first; the CJK text
  field needs HarfBuzz (as UE does) and IME before it can be called done.

## 5. deren layer shape

`ui_context` (C#, `source/editor/ui/`):

- frame lifecycle: `begin_frame(input)`, widgets, `end_frame()`; one draw list per frame.
- id stack: `push_id(string)`, and every widget takes an `id` that is unique within its parent.
- layout: `row/column/box` scopes with `fixed`, `auto`, `weight`; `padding`, `gap`, `splitter`,
  `scroll_area`; solved at `end_frame` from the children's desired sizes. Same semantics as
  Blender's solver.
- draw list commands: `rect`, `rounded_rect`, `line`, `triangle`, `text`, `clip_push/pop`,
  `image(texture_id, uv_rect)`, `custom_indexed` for timeline grids and curves.
- input: one `ui_event` struct (type, position, button, modifiers, wheel, plus a text and key
  payload) and a `ui_reply` struct applied by the context.
- retained only for: focused id, active (captured) id, drag state, text edit state, scroll offsets,
  popup stack, and per-id cached measurement for text.

Rendering (`deren_gui_vulkan`): one vertex format (pos, uv, color), two pipelines (solid, text
sampled from the atlas), scissor-based clipping, vertex upload through a single transient buffer,
and an ABI call `submit_ui_vertices(vertices, indices, draw_ranges)` that the C++ host records into
the frame. Text atlas upload is one ABI call `upload_ui_font_atlas(pixels, width, height)`.

## 6. M0 scope from this reference

M0 is deliberately small and uses every idea above once:

- `ui_context` with id stack, row/column solver, draw list, clip stack, and the reply model.
- controls: label, button, toggle, text field (Latin, no IME), tree row, scroll area, splitter.
- one font atlas from `LatoLatin-Regular.ttf` (already shipped for RmlUi).
- the Vulkan submission path and one test window showing a splitter, a scrolling list of 2000 rows,
  a text field and a button row, so the solver, atlas, batching and virtualization are proven
  before any editor screen moves onto the layer.
