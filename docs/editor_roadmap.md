# Editor enhancement roadmap

## Goal

Use Deren's editor to build a small playable 3D project from a blank workspace.
The workflow covers importing assets, composing scenes, attaching C# behaviours,
testing in Play, and exporting a Windows game. Use the two-room helmet demo to
check that these pieces still work together as the editor grows.

## Baseline

- Scene editing includes an Outliner, Details, a Content Browser, a viewport
  camera and transform gizmos, undo and redo, save and load, and Play mode.
- Project workspaces support asset import, script inspection, project settings,
  environment maps, standalone launch, and Windows export.
- [PR #108](https://github.com/YzK0741/deren/pull/108) adds reusable scene
  instances, asset relocation, point lights, game cameras, instance materials,
  editable input actions, audio sources, box triggers, and static box collision.
- A freshly generated helmet demo was verified in editor Play and through the
  exported executable's default launch. The tested sequence was wall blocking,
  sliding along the wall, passing through the doorway, playing the audio cue,
  and switching from Workshop to Hangar.

See the [editor README](../source/editor/README.md) for controls and feature details.

## 1. Grounded character movement

Extend the existing C# movement API and static collision boxes into a basic
grounded controller. Start with ground detection and gravity. Expose enough
contact information for a behaviour to tell when the character lands or walks
off an edge. Keep character tuning in the sample behaviour until multiple
projects need the same controls.

**Acceptance.** In editor Play and the exported game, the character lands on
the floor, falls after walking off an edge, slides along a wall, and can still
pass through the demo doorway and trigger the room transition.

## 2. Moving solids and gameplay interactions

Give moving collision boxes explicit update semantics and exercise them with a
door or platform. Keep trigger enter and exit events usable from C# for doors,
checkpoints, and audio cues. Add visible scene geometry for the demo collision
volumes so players can tell where the passage is.

**Acceptance.** A closed door blocks the character. Once opened by an authored
interaction, the same opening becomes passable. The trigger emits one event
per overlap transition and the result survives saving, reopening, and exporting.

## 3. Reusable gameplay content

Build on linked scene instances. Start with per-instance script parameter
overrides so the same reusable room prop can point to different targets or
audio cues. Show which values come from the source scene and let the editor
restore the source value. Keep the source scene responsible for its own node
structure.

**Acceptance.** Two instances of a reusable door can have different targets.
A source scene edit reaches both instances and each instance retains its saved
overrides. Play and standalone resolve the same references.

## 4. Independent animated instances

Give each placement of an animated asset its own playback state and skin
matrices. Add editor controls for the initial clip and whether playback starts
automatically. Use a two-character scene to exercise independent playback and
cleanup.

**Acceptance.** Two placements of the same character play at different times
or use different clips. Editing or removing one leaves the other correctly
animated in Play and standalone.

**Status.** ABI 46 makes the animation player per placement: the scene API takes
the placement node, the host keeps one controller, clock and skin-matrix slice
per placement, and a removal drops only the players of the subtree that goes
away. Details gained an **Animation** group with **Initial Clip** and **Auto
Play**, stored as `animation_clip` and `animation_autoplay` from document
version 13. The `build/editor-animation-demo` project holds two `laevatain`
placements in `scenes/two-character.scene.toml`; one autoplays from t=0 while a
behaviour seeks the other to t=21 s, and `screenshot_20261124_061727.png`
(Play) shows the two poses in one frame. `deren.exe run <project>
--editor-selftest-animation` covers the rows, the version and placement gates,
undo and redo, per-placement select and seek, and deleting one placement while
the neighbour keeps its player.

## 5. Project validation and export

Add a project check that reports broken scene, asset, script, and audio
references with enough context to find the node or setting that owns each one.
Use the same checks before export. Detect missing content referenced from saved
data and keep explicit inclusion available for content selected dynamically by
scripts.

**Acceptance.** A broken reference produces a targeted error. After repairing
it, a clean Windows export launches from a different directory and completes
the sample interaction using the packaged content.

**Status.** The check is `source/editor/editing/project_validator.cs`, shared by
the editor and the exporter: `deren.exe run <project> --check-project` prints one
`file: node 'Name': message` line per problem plus a
`Project check passed: N scene(s), M reference(s).` summary and fails the
process, **Check Project** in the project scene panel runs the same code and
shows the first problem, and a project whose startup scene cannot be built still
opens for a command-line check so the problems can be reported. It covers the
manifest's startup scenes, every scene under `scenes/`, asset library ids, scene
instances (recursively, depth cap and loop detection), behaviour bindings,
audio files, environment maps, and the `[export] include` list that names
content loaded dynamically by scripts. `scripts/tools/export_windows_game.py`
runs the check before staging and refuses to write a package when it fails, and
it copies the `[export] include` entries into `project/`. Evidence:
`build/editor-validation-demo` (missing asset, behaviour, audio file and include
entry) reports all four problems and the exporter refuses it with the same
text, while `build/editor-animation-demo` checks clean, exports, and the moved
package `build/_export-moved` launches and shows the two animated characters
(`screenshot_20261124_092747.png`). `--editor-selftest-validation` covers the
clean pass and each problem class. The release check also ran from a fresh
project: `python scripts/tools/new_editor_project.py build/_release-check --demo`
passes `--check-project` and `--editor-selftest`, exports with
`scripts/tools/export_windows_game.py`, and the package moved to another
directory launches, enters and leaves the second room (`Helmet demo: entered
Workshop` / `left Workshop`) and renders the helmet and both doors
(`build/release-check-screenshot.png`).

## 6. Animation editor

Edit an asset's motion clips inside the editor instead of converting them
offline. The animation editor is isolated: switching into it puts the project
scene aside and opens an empty preview scene, and the author places the asset to
animate into it, so authoring motion never edits a saved scene and a scene's undo
history is never shared with a clip's. Back the timeline with real keyframe data, let the author
change key times and values, and write the result back to the asset's
`motion.gltf` so Play, the standalone game and the exporter all see it. C# owns
the authoring logic; the engine keeps sampling clips as it does today.

**Acceptance.** An author opens the animation editor on a character, changes a
key's time and value, and the preview shows the edit immediately. Saving updates
the asset's motion file, reopening the editor and starting Play show the edited
motion, and the unchanged project scene still opens and saves as before.

**Status.** The mode, the isolated preview scene and the transport are in.
`source/editor/editor_host.animation_mode.cs` swaps the project document for an
empty in-memory preview document (`name = "Animation Preview"`, no roots, no
path) and restores the project document, its selection and its path on exit. The
author fills it with **Open Asset** in the Timeline header, which creates one
placement of the asset the Content Browser highlights, frames the camera on it and
selects it, so entering the mode itself never depends on what was selected. The
empty preview uses a fixed pose; once the placement's player reports a clip (the
asset import trails node creation) the mode focuses the real mesh bounds and the
strip rewinds to 0.00 s. The **Animation** button
toggles the mode, the toolbar note reads `ANIMATION EDITOR` while it is active,
**Save** and **Play** refuse the preview, and the timeline strip under the
viewport shows the clip name, the time and a scrub bar with **Play**/**Pause**
and **Restart**; the strip drives the placement's own ABI 46 player, so it reads
back the real clock. Evidence: `deren.exe run <project> --editor-animation-toggle 1`
captures the empty preview (`build/editor-animation-demo/screenshot_20261124_114234.png`:
`nodes: 0`, the Timeline reads "Empty preview: open an asset to animate"), and
`--editor-animation` opens `laevatain` through **Open Asset** and captures
`build/editor-animation-demo/screenshot_20261124_114525.png` (strip at
`0.00 / 42.93 s` showing "Bibideba", Timeline listing the 52 channels), while
`--editor-selftest-animation-mode` covers the isolated document,
the preview player appearing after the asset import, seek, the strip's Play and
Restart buttons, the save guard, and restoring the project document unchanged.
Next slices: keyframe rows read from the clip (read-only), then key time/value
editing with undo, then the `motion.gltf` writer with a reload of the placement's
player.

The first of those slices is in: the bottom dock's **Timeline** tab reads the
preview asset's `motion.gltf` from C# (`source/editor/editing/motion_document.cs`
and `source/editor/editor_host.timeline.cs`) instead of growing the scripting
ABI, lists one row per animated channel with its node, path, key count and key
range, and seeks the preview to a channel's first key when its row is clicked.
The tab only exists while the animation editor is open and the clip is dropped
when it closes. `--editor-selftest-animation-mode` checks the loaded channel
count, the key counts and the row selection. Remaining: key time/value editing
with undo, then the writer that patches edited keys back into the glTF buffer
and reloads the placement's player.

Switching modes rebuilds the scene's placements, and each animated placement
holds a slice of the shared skin and morph buffers. The slice is now returned
when its placement is destroyed and reused by the next one, so switching back and
forth no longer fills the buffer (a tenth switch still allocates blocks 4, 440
and 876 for `laevatain`, and a placement that cannot get a slice falls back to
the rest pose instead of reading another character's joints).

## Release checks

Verify new authoring features through edit, undo and redo, save and reopen,
Play, and standalone. Repeat the two-room demo from a fresh project and its
Windows export when changes affect scene loading, scripts, input, collision,
audio, or packaging. Use a schema version change when the saved format changes
and preserve loading of existing projects.
