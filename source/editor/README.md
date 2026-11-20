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

## Layout

| Path | Responsibility |
| --- | --- |
| `editor_host.cs` | The only `game_entry`; owns the document, the tree, the RmlUi document and the events. |
| `document/scene_document.cs`, `document_node.cs` | The editable tree: uid, name, asset, pipeline, transform, visible, flags, children, runtime node id. |
| `document/document_io.cs` | The strict TOML reader/writer for `*.scene.toml`. |
| `document/scene_sync.cs` | One `rebuild` at startup, then per-node `create_one`/`destroy_one`/`push` into the activated tree. |
| `editing/undo_stack.cs`, `undo_command.cs`, `selection.cs` | The undoable edits, the 64-entry history and the selection set. |
| `panels/outliner_panel.cs`, `details_panel.cs`, `status_bar.cs` | Text into the fixed RmlUi ids (`row_0..row_63`, `detail_*`, `status`). |
| `ui/editor.rml` | The document markup and its inline style. |
| `main.toml` | The entry scene the manifest points at: window size and title only. |
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
