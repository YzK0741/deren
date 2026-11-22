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

## Editor projects and standalone games

Create a project after building the editor:

```powershell
python scripts/tools/new_editor_project.py my_game
.\build\deren.exe run .\my_game --editor
.\build\deren.exe run .\my_game
```

The project owns `deren.toml`, `render.toml`, `scenes/`, `asset/` and `scripts/`.
`[project] main_scene` points at the renderer settings, `[editor] scene` names the scene
opened for editing, and `[game] scene` names the saved scene run by default. Projects
without `[game]` keep their existing launch behavior. `--scene scenes/other.scene.toml`
overrides the startup scene for that launch; with `--editor` it opens the file for editing.
Scene paths in the game are relative to the project directory.

Edit, Play and standalone share the asset library, renderer settings and input action
bindings. Play runs an unsaved copy of the scene and uses the editor camera; standalone
reads the saved file and uses the configured camera. Both use the same scene loader and
behaviour lifecycle. A behaviour can call `try_change_scene("scenes/other.scene.toml")`
in `on_update` to queue a scene change for the next frame. The first accepted request
wins, and a load failure logs the reason and leaves the current scene active.
The editor UI stays in the build output. A project's layout and preview caches live in
`.deren/`. Use `--asset-library` when the project shares an existing asset library.

To try both launch modes with a small game:

```powershell
python scripts/tools/new_editor_project.py build/helmet-game --demo
.\build\deren.exe run .\build\helmet-game
.\build\deren.exe run .\build\helmet-game --editor
```

The demo uses the repository's helmet assets, builds its C# behaviour into `scripts/`
and generates a short WAV. Move with A/D, play the sound with Space, and switch
between Workshop and Hangar with Enter. The behaviour source is in `script-source/`.

## Windows game export

Export a saved scene project with Python 3.12+ after building `deren`. The
destination must be a new directory outside the source project.

```powershell
python scripts/tools/export_windows_game.py build/helmet-standalone-v2 dist/helmet-game
```

Double-click `deren.exe` in the export, or move the whole folder and open it
there. The executable starts `project/deren.toml` by default. The package contains
the Windows engine binaries, shaders, a private .NET runtime, and the game files.
The target machine needs a compatible Vulkan graphics driver.

The export includes saved scenes, compiled behaviour DLLs in `scripts/`, audio,
UI files, and renderer inputs. It copies a project-local asset library in full.
For a shared library it includes the assets named in saved scenes. Repeat
`--asset ID` for assets only referenced in code or script parameters, and
`--data PATH` for other project-relative data the game reads. External asset
entries are copied with their asset folders and license files. Referenced glTF
buffers, textures and motion files within the asset library are included too.
Renderer inputs must use project-relative paths.

`--build` selects a different build output and `--dotnet-root` selects the
.NET installation used to assemble the private runtime.

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

The image is the frame's display-encoded LDR target. At full render scale FXAA resolves it when
enabled. If the document samples the scene without FXAA, a same-size copy keeps the viewport
available. Below full render scale the upscale pass resolves the scene.

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
dragging or floating window. A viewport click picks the nearest visible asset bounds or clears the
selection on a miss. The outliner and Details panel share that selection. Use the Details panel to
edit the selected node's transform.

## Editor shell additions

- **Menu bar.** `File` (New Scene, Save Scene), `Edit` (Undo, Redo, Delete Node, Rename Node),
  `Window` (World Outliner, Details, Content Browser) and `Help` (About). The four dropdowns are
  authored once in `ui/editor.rml` and toggled by `visibility`; a click on any non-menu element closes
  them first. The Window entries toggle a panel with its authored display value (`flex` for the
  outliner and the content browser column, `block` for the details body) and hide the splitter that
  only mattered while both neighbours were visible, so hiding a panel does not leave its space
  reserved and the remaining handle cannot resize an invisible column.
- **Borderless caption.** With `[render] borderless_window = true` in the project's `main.toml` the
  editor window is created without the system frame and draws its own caption: `#win_minimize`,
  `#win_maximize` and `#win_close` at the right of the menu bar, a press-and-drag on the empty menu
  bar strip (or the brand/project label) moves the window through the system move loop, and a second
  press inside the double-click window maximises or restores it. All of it goes through
  `deren.window_control` (ABI 43), not through faked window state: the buttons forward to the real
  window, and the close button runs the same unsaved-changes prompt as New Scene before it asks the
  window to close. The window is resizable from its edges because the caption installs a thin
  hit-test band; maximising honours the monitor work area. The default is off, so other projects keep
  their decorated window and the dark `dark_window_frame` frame.
- **Content browser.** The asset catalogue is rendered into the 24 authored
  tiles (`asset_0` .. `asset_23`); `#browser_search` filters the id list (`OrdinalIgnoreCase`) and
  `#browser_count` reports "shown / total". A tile click moves a `#4a9eff` border to that tile and
  writes `asset: <id>` into the status bar. **Build Thumbnail** renders the selected asset offscreen
  and caches the resulting image in the editor output's `ui/thumbnails` folder. An asset without a
  cached image shows a placeholder glyph.
  double-click adds the asset as a root node at the origin, selects it, and records an undoable edit.
- **World Outliner.** Expand or collapse a group from its disclosure control. Ctrl-click toggles
  individual nodes and Shift-click selects a range of visible rows. Search reveals matching nodes and
  their ancestors, including matches inside collapsed groups. Edit > Duplicate Selection copies each
  selected subtree with fresh node IDs. Drag a row onto another node to parent the selection under it,
  or onto Scene Root to move it to the top level. Reparenting preserves the world transform and skips
  the edit if the required local transform contains shear. Duplicate and reparent each record one undo.
- **Scene files.** File > Open Scene and Save Scene As accept an absolute path or a path relative to
  the project's scenes folder. Opening a scene with unsaved edits prompts before replacing it.
  Undo and redo update the dirty indicator relative to the most recent save.
- **Asset placement.** Drag an asset tile into the viewport to place it on the construction plane.
  Click Pipeline in Details to change between the selected asset's supported pipelines.
- **Play / Stop.** Play starts a separate renderer process from a temporary copy of the scene and
  the editor camera. It runs bundled animation and optional node scripts. Stop closes the play
  process and returns to the editor with the open document and undo history intact.
- **Viewport toolbar.** `Move`, `Rotate` and `Scale` expose X/Y/Z handles at the selected node's
  pivot. Drag along the handle to move or scale, or across the handle to rotate. `World`/`Local`
  switches the move and rotate axes. Scale uses the node's local axes. Each drag records one undo
  entry. Esc or loss of pointer focus cancels the drag.
- **Status bar.** The notice suffix (`tool:`, `asset:`, the About line) is set by these interactions and
  cleared by the next click, on top of the existing `dirty`, node count and selection fields.

`--editor-selftest` exercises the same handlers in-process (the shell is not interactive yet when it
runs): outliner filtering and the visibility glyph, asset selection from a tile and from its label, the
viewport tool highlight, opening/closing a menu and both dock tabs, and the Window-menu panel toggles
(the layout shots check that a hidden panel really releases its column and that showing it again
restores the same boxes). The caption buttons need a real window and are verified by hand.

## Layout

| Path | Responsibility |
| --- | --- |
| `editor_host.cs` | The only `game_entry`; owns the document, the tree, the RmlUi document and the events. |
| `document/scene_document.cs`, `document_node.cs` | The editable tree: uid, name, asset, pipeline, transform, visible, flags, children, runtime node id. |
| `document/document_io.cs` | The strict TOML reader/writer for `*.scene.toml`. |
| `document/scene_sync.cs` | One `rebuild` at startup, then per-node `create_one`/`destroy_one`/`push` into the activated tree. |
| `editing/undo_stack.cs`, `undo_command.cs`, `selection.cs` | The undoable edits, the 64-entry history and the selection set. |
| `panels/outliner_panel.cs`, `details_panel.cs`, `status_bar.cs`, `content_browser_panel.cs` | Outliner rows keyed by node UID, plus details, asset tiles and status. |
| `ui/editor.rml` | The document markup and its inline style: menu bar, toolbar, viewport column, bottom dock, outliner column and the three splitters. |
| `main.toml` | The entry scene the manifest points at: window size and title, and the `[render] fxaa` switch the viewport needs. |
| `deren.toml.in` | The project manifest template CMake configures into the build tree. |

The Outliner generates its rows through `try_set_markup` whenever the visible node order changes.
Node names go through `set_text` so they are displayed as text. The Content Browser uses 24
authored tile slots and the Details panel maps its authored controls to node properties. Input values
are refreshed on selection changes and committed edits so a frame does not overwrite active typing.

## Project scenes and presentation settings

Use **File > Project Scenes** to open a saved scene or select **Set as Startup Scene**.
This updates the game's startup scene in the manifest. **Open Scene by Path** still opens
files outside the project scene list.

**File > Project Settings** edits exposure, sun direction and intensity, background, FXAA,
and TAA. **Preview** updates the viewport without writing the project. **Cancel** restores
the saved presentation. **Save** changes the affected keys in the renderer TOML, preserving
the other settings and comments. Background mode and color saved here override the referenced
lighting profile; other calibration in the profile stays intact. Play, standalone and
export all read the saved values.

## Environment maps

The **Environment** group under Details lists the environment maps in use. **Scene map** is the
project-wide HDR: it writes `[lighting] environment_hdr` in the renderer TOML, and *(procedural sky)*
clears it. **Choose a file...** opens the platform dialog in the asset library and stores the pick
relative to the project.

Select a node and use the **Environment** row of Details to give that object its own map: the
dropdown lists the same HDRs, *(scene map)* returns the object to the project-wide map, and picking
a file allocates a free environment lane automatically. Objects that ask for the same map share one
lane, so the sixteen engine slots are spent only on distinct maps; when every lane is already filled
and none is free of objects, a new map reuses the lane the selected object already owned, and the
status line says so because the other objects on that lane change with it. A lane row appears under
Environment as soon as a lane is used, and setting it to *(procedural sky)* empties it and returns
every object that sampled it to the scene map. The lane number, not the path, is what the scene node
stores, so a map shared by several objects is uploaded once. A node may only name a lane the document
fills: a hand-edited file with `environment = 3` and no `slot_3` is refused on load, because a node on
an empty lane would sample a descriptor nobody sets and render unlit. One dropdown pick is one undo
step, even though it changes both the lane and the node.

## Importing models

In Content Browser, choose **Import** and browse for a GLB or glTF 2.0 model, or paste its path.
The destination is the project's configured asset library. Use the suggested asset ID or enter a
different one. The importer creates a PBR asset folder, preserves the relative paths of external
buffers and textures inside the model's directory, and refuses missing files or duplicate IDs.
The new asset stays selected in Content Browser; double-click it or drag it into the viewport.
Use **Refresh** to rescan asset folders changed outside the editor. Existing scene instances keep
their loaded data. Imported files belong to the project, so undoing a placement only removes
the instance from the scene.

## Document format

`document_io` owns a strict subset of TOML. Reading rejects an unknown or duplicated key, a duplicate
uid, a missing or unknown parent, a version newer than 4 (an unsupported future file), an unparsable number and an unterminated
string, and reports the file, line and reason. A file starts with `[document]`, then one `[[node]]`
block per node in depth-first order, where a parent always appears before its children:

```toml
[document]
    version = 4
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

`uid` and `name` are required; `parent` (a parent uid), `asset`, `pipeline` and `environment` (the
lane the node samples, 0 for the scene map) are optional. An `[environment]` table between
`[document]` and the first node stores the scene lanes as `slot_1`..`slot_15`; an absent lane stays
absent, and an empty string means the procedural sky. Floats are written so they round-trip exactly.

To attach a C# behaviour, build a DLL against `Deren.Managed` and put it and its managed
dependencies in the **project's** `scripts/` directory. Choose the behaviour in Details. The type
is public, derives from `script_base` and has a public parameterless constructor. **Refresh**
rescans the DLLs without restarting the editor.

Mark a public writable instance field with `[editor_parameter]` to expose it in Details. Supported
types are `int`, `float`, `bool`, `vector_3`, `node_id` and `asset_reference`. Commit numbers and
vectors with **Apply**. Vector input uses comma-separated X, Y and Z. Node and asset references use pickers. A blank or **Default** value uses
the field initializer. The scene stores overrides and Play applies them before `on_create`. Scene
documents written by versions 1 and 2 still load.

Open **Output Log** to see live Play messages from `host_logger.write` and runtime errors. Clicking
a script callback error selects its node in the editor. Play continues to work from a temporary scene
copy, including unsaved edits.

## Limits

One open document, 24 asset tiles, a fixed set of details rows (no
add/remove property outside declared script parameters). The content browser supports file import
and double-click placement. Asset rename is not available. The Window menu moves the sidebar between left and right, puts the
browser and log dock above or below the viewport, and swaps the order of Outliner and Details.
Splitters keep working in each arrangement. Panel sizes and positions are saved in the editor
output's `editor-layout.json`. Reset Layout restores the default arrangement. The panels are docked
inside the editor window.

One animated asset can be placed only once per scene. The runtime owns a single animation player and
a single skin-matrix region, so a second placement of the same asset is not driven by the player and
renders with a mismatched skeleton. Use a single placement, or a different asset, until multi-instance
skinning lands.

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


The viewport supports right-drag to orbit, middle-drag to pan, and the scroll wheel to zoom.
Select a node and press F or click Focus to frame its visible assets, including child nodes.
A node without visible assets focuses its world-space pivot.
