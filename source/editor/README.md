# Scene editor

A managed project, not a native mode. The editor compiles to `Game.dll` and is launched exactly like
any other project, so the native executable stays a renderer that only executes what C# asks for:

```powershell
cmake --build build
build/deren.exe run build/source/editor
```

Everything the editor does lives in C#: the document model, the TOML reader/writer, the scene sync,
undo, selection, the panels and the shell layout. The managed ABI gained only what the editor could
not express otherwise: `scene_api.set_node_name` (ABI 40), `scene_api.set_scene_aspect` (ABI 41) for a
projection that follows the viewport shape, and the rml pointer events, `get_element_rect` and
`set_element_style` entries of ABI 42 for the splitters and dock tabs (`docs/scripting_module_abi.md`).

## What P1 ships

- One scene document with an outliner, a details panel, a toolbar and a status bar drawn by RmlUi.
- `New Node`, `Delete`, `Save`, `Undo`, `Redo`, `Rename` and row clicks in the outliner.
- New Scene prompts Save / Discard / Cancel when the document is dirty. Save failure keeps the old
  document and the prompt open. Recreated subtrees restore transform, visibility, flags, pipeline and name.
- The details panel edits the selected node in place: name, the nine transform lanes, visibility and
  the application flags. Each accepted row pushes the value onto the runtime node immediately and
  records one snapshot undo command, the way a Unreal details row applies a property edit.
- Save/load of `scenes/<name>.scene.toml` under the project directory, undo/redo through a 64-entry
  stack, and a dirty flag.
- `--editor-selftest`: save, reload, compare every uid/name/transform, rename the root through the
  button path, delete it, undo it, then edit a transform lane through the details path, check the
  runtime node received it and undo it again. A failure ends the run with
  `script_status.script_failure`.
- `--scene <path>` opens an existing document instead of the default single-node one.

## Viewport (P2)

The centre of the document is one `<img id="viewport_image" src="deren://scene"/>` inside `#viewport`.
It shows the live scene, not a capture: the engine's frame image is sampled into the element, so a
transform pushed from C# or a moving camera is visible on the next frame.

`deren://scene` is a pseudo-URI of the managed document, not a file. `LoadTexture` answers it without
touching the filesystem, returning a sentinel handle to mark the draw, and `RenderGeometry` sets
`texture_view.scene_color` on it (see `source/ui/rml_document.cpp`); the overlay then binds the
render chain's image for that draw instead of a texture the document owns. The intrinsic size the
loader reports (1920x1080) is only a placeholder: the element is sized in CSS. One viewport element is
authored, and one image is handed to the overlay per frame.

The image is the frame's display-encoded LDR target, and it exists only on frames a resolve pass
finishes: FXAA at the default `render_scale = 1.0`, or the upscale pass below it. When neither runs,
`post_composite` writes the swapchain itself, `runtime.current_scene_color()` reports no image, and
the overlay skips the scene draws, which leaves the viewport empty rather than stale. That is why
`main.toml` must keep `[render] fxaa = true`: at the editor's 1.0 render scale it is the resolve, and
without it the viewport never receives an image. `fxaa` and `upscale` are alternatives, not a stack:
a chain below `render_scale = 1.0` resolves through the upscale pass instead, and FXAA is switched
off there.

The element fills its viewport column (`width: 100%; height: 100%` in `ui/editor.rml`). That column's
shape changes with the window and with the splitters, so `editor_host` reads the element rectangle
(`try_get_element_rect("viewport")`) every frame and reports its shape to the renderer through
`camera_projection.try_set_aspect(width / height)`. The render still covers the whole window, so the
image is stretched into the column exactly the way its projection was squeezed - geometry stays
correct at any window size, with no letterboxing or cropping. Both numbers are dp and an aspect is
unitless, so a render scale other than 1 does not change the result. The 1920x1080 the loader reports
stays a placeholder.

P2 ships the viewport as a display surface inside an editor shell modelled on the Unreal Level Editor:
a menu bar, the main toolbar, the viewport with its own small toolbar over a bottom dock, and a right
column with the World Outliner above the Details panel. The three splitters (`split_right`,
`split_outliner`, `split_bottom`) are draggable: the pointer events of ABI 42 carry dp coordinates and
the id of the nearest element with one, and `editor_host` resizes the neighbouring panel by writing
`width`/`height` in dp through `try_set_element_style`. The bottom dock switches between the Content
Browser and Output Log tabs by toggling the visibility of both bodies; there is no tab tearing, dock
dragging or floating window. Picking (a click resolving to a node) and gizmos are not implemented: the
outliner still selects and the details panel still transforms.

## Editor shell additions

- **Menu bar.** `File` (New Scene, Save Scene), `Edit` (Undo, Redo, Delete Node, Rename Node),
  `Window` (World Outliner, Details, Content Browser) and `Help` (About). The four dropdowns are
  authored once in `ui/editor.rml` and toggled by `visibility`; a click on any non-menu element closes
  them first. The Window entries toggle a panel with `display: none`/`block` (the details and browser
  bodies are `flex`), so hiding a panel does not leave its column reserved.
- **Content browser.** `asset_manager.list()` is read once at startup and rendered into the 24 authored
  tiles (`asset_0` .. `asset_23`); `#browser_search` filters the id list (`OrdinalIgnoreCase`) and
  `#browser_count` reports "shown / total". A tile click moves a `#4a9eff` border to that tile and
  writes `asset: <id>` into the status bar. Tiles use placeholder glyphs instead of thumbnails;
  double-click adds the asset as a root node at the origin, selects it, and records an undoable edit.
- **World Outliner.** `#outliner_search` keeps a node when its own name or any descendant name contains
  the filter, so typing a child still reveals its parent chain; the selected row is still marked with
  a `"> "` prefix. Each row has a `#row_N_vis` glyph that toggles the runtime node's visibility (and
  records one `Show Node`/`Hide Node` undo command).
- **Viewport toolbar.** `Select`/`Move`/`Rotate`/`Scale` highlight the active tool button and report
  `tool: <name>` in the status bar; they do not change the gizmo yet because there is no gizmo.
- **Status bar.** The notice suffix (`tool:`, `asset:`, the About line) is set by these interactions and
  cleared by the next click, on top of the existing `dirty`, node count and selection fields.

`--editor-selftest` exercises the same handlers in-process (the shell is not interactive yet when it
runs): outliner filtering and the visibility glyph, asset selection from a tile and from its label, the
viewport tool highlight, opening/closing a menu and both dock tabs.

## Layout

| Path | Responsibility |
| --- | --- |
| `editor_host.cs` | The only `game_entry`; owns the document, the tree, the RmlUi document and the events. |
| `document/scene_document.cs`, `document_node.cs` | The editable tree: uid, name, asset, pipeline, transform, visible, flags, children, runtime node id. |
| `document/document_io.cs` | The strict TOML reader/writer for `*.scene.toml`. |
| `document/scene_sync.cs` | One `rebuild` at startup, then per-node `create_one`/`destroy_one`/`push` into the activated tree. |
| `editing/undo_stack.cs`, `undo_command.cs`, `selection.cs` | The undoable edits, the 64-entry history and the selection set. |
| `panels/outliner_panel.cs`, `details_panel.cs`, `status_bar.cs`, `content_browser_panel.cs` | Text into the fixed RmlUi ids (`row_0..row_63`, `detail_*`, `asset_0..asset_23`, `status`). |
| `ui/editor.rml` | The document markup and its inline style: menu bar, toolbar, viewport column, bottom dock, outliner column and the three splitters. |
| `main.toml` | The entry scene the manifest points at: window size and title, and the `[render] fxaa` switch the viewport needs. |
| `deren.toml.in` | The project manifest template CMake configures into the build tree. |

The RmlUi managed document API exposes `set_text`, `set_value`, `set_visible`, `try_poll_event`,
`try_get_element_rect`, `try_set_element_style` and `close` only - it cannot create elements - so the
outliner is 64 pre-built rows (`row_0` .. `row_63`), the content browser is 24 pre-built tiles
(`asset_0` .. `asset_23`) and the selected outliner row is marked with a `"> "` text prefix. The details
panel uses the same trick: its rows are authored once in `ui/editor.rml` and `details_panel` maps their
`change` events back to properties. It rewrites a row only when the selection changes or an edit
commits, because rewriting every frame would overwrite text the user is still typing. Visibility is a
button rather than a checkbox because a checkbox `change` value is not round-tripped unambiguously by
this bridge.

## Document format

`document_io` owns a strict subset of TOML. Reading rejects an unknown or duplicated key, a duplicate
uid, a missing or unknown parent, a version other than 1, an unparsable number and an unterminated
string, and reports the file, line and reason. A file starts with `[document]`, then one `[[node]]`
block per node in depth-first order, where a parent always appears before its children:

```toml
[document]
    version = 1
    name = "untitled"

[[node]]
    uid = "0f8fad5b-d9cb-469f-a165-70867728950e"
    name = "root"
    visible = true
    flags = 0
    position = [0, 0, 0]
    rotation = [0, 0, 0]
    scale = [1, 1, 1]
```

`uid` and `name` are required; `parent` (a parent uid), `asset`, and `pipeline` are optional. Floats
are written so they round-trip exactly.

## Limits

One open document, 64 visible outliner rows, 24 asset tiles, a fixed set of details rows (no
add/remove property, no asset picker), no viewport picking or gizmo, no Play/Stop, and the Output Log
body is still a placeholder (no engine log feed). The content browser supports double-click placement but has no
rendered thumbnail, asset rename, file import or drag-to-scene. The shell is a fixed five-region layout with draggable
splitters, not a dock system: panels cannot be undocked, torn out, tabbed into each other or floated,
and the Window menu only hides and shows the three authored panels. Those are the next milestones; the
document model, `scene_sync` and the snapshot undo commands are the seams they will attach to.

## Visual style

The editor uses the neutral Slate/Starship hierarchy from the local UE reference:
`StyleColors.cpp` (background, recessed, panel, header, input and primary roles) and
`StarshipStyle.cpp` (toolbar, row and tab hierarchy). RmlUi implements the appearance
without loading Unreal assets or copying its widget implementation.

Toolbar groups, blue selection states, scrollable actor/asset lists, compact RGB XYZ
fields and asset placeholders share the same palette. The card glyphs represent library
assets; they are not rendered thumbnails. Existing element ids and editing actions are retained.

Double-click placement uses the asset's default pipeline and an identity transform at the scene root.
A single click only selects the card. Undo removes the instance, redo reloads it, and the asset id is
saved in the scene document. Failed loads preserve the document, selection and undo history.
`--editor-selftest` also exercises placement after startup, renders a frame, then checks undo, redo,
failed-load rollback and save/load of the placed asset. The lightweight fixture is `damaged-helmet`.

Placed assets record their declared default pipeline in the document (Laevatain: `game_toon`).
Selecting this pipeline enables the character/game-toon stages even if the editor started empty.
Editor imports pause bundled animation before the next frame, including reload/redo; they remain
at the selected clip's initial frame rather than starting playback. Game projects retain their
existing playback behavior. Use `--editor-selftest-asset laevatain` to exercise this path.
