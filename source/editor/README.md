# Scene editor

A managed project, not a native mode. The editor compiles to `Game.dll` and is launched exactly like
any other project, so the native executable stays a renderer that only executes what C# asks for:

```powershell
cmake --build build
build/deren.exe run build/source/editor
```

Everything the editor does lives in C#: the document model, the TOML reader/writer, the scene sync,
undo, selection and the panels. The managed ABI only gained the one entry the document could not
express before, `scene_api.set_node_name` (ABI 40).

## What P1 ships

- One scene document with an outliner, a details panel, a toolbar and a status bar drawn by RmlUi.
- `New Node`, `Delete`, `Save`, `Undo`, `Redo`, `Rename` and row clicks in the outliner.
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

The element is fixed at 800dp x 450dp (16:9) in `ui/editor.rml` and centred in the middle column. The
engine renders at the window's framebuffer extent and the element samples that image across its own
area, so the scene is scaled into 800x450, not laid out at the engine's resolution. On a window whose
aspect is not 16:9 the image is stretched: RmlUi has no `object-fit` and this document does not
letterbox.

P2 ships the viewport as a display surface. Picking (a click resolving to a node) and gizmos are not
implemented: the outliner still selects and the details panel still transforms. The layout is also
fixed: one viewport, no `object-fit`, no docking, and the three columns cannot be moved or resized.

## Layout

| Path | Responsibility |
| --- | --- |
| `editor_host.cs` | The only `game_entry`; owns the document, the tree, the RmlUi document and the events. |
| `document/scene_document.cs`, `document_node.cs` | The editable tree: uid, name, asset, pipeline, transform, visible, flags, children, runtime node id. |
| `document/document_io.cs` | The strict TOML reader/writer for `*.scene.toml`. |
| `document/scene_sync.cs` | One `rebuild` at startup, then per-node `create_one`/`destroy_one`/`push` into the activated tree. |
| `editing/undo_stack.cs`, `undo_command.cs`, `selection.cs` | The undoable edits, the 64-entry history and the selection set. |
| `panels/outliner_panel.cs`, `details_panel.cs`, `status_bar.cs` | Text into the fixed RmlUi ids (`row_0..row_63`, `detail_*`, `status`). |
| `ui/editor.rml` | The document markup and its inline style, including the 800x450 `deren://scene` viewport. |
| `main.toml` | The entry scene the manifest points at: window size and title, and the `[render] fxaa` switch the viewport needs. |
| `deren.toml.in` | The project manifest template CMake configures into the build tree. |

The RmlUi managed document API exposes `set_text`, `set_value`, `set_visible`, `try_poll_event` and
`close` only - it cannot create elements - so the outliner is 64 pre-built rows (`row_0` .. `row_63`)
and the selected row is marked with a `"> "` text prefix. The details panel uses the same trick: its
rows are authored once in `ui/editor.rml` and `details_panel` maps their `change` events back to
properties. It rewrites a row only when the selection changes or an edit commits, because rewriting
every frame would overwrite text the user is still typing. Visibility is a button rather than a
checkbox because a checkbox `change` value is not round-tripped unambiguously by this bridge.

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

## Limits of P1

One open document, 64 visible rows, a fixed set of details rows (no add/remove property, no asset
picker), no viewport picking or gizmo, no content browser and no Play/Stop. Those are the next
milestones; the document model, `scene_sync` and the snapshot undo commands are the seams they will
attach to.
