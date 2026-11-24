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

## 5. Project validation and export

Add a project check that reports broken scene, asset, script, and audio
references with enough context to find the node or setting that owns each one.
Use the same checks before export. Detect missing content referenced from saved
data and keep explicit inclusion available for content selected dynamically by
scripts.

**Acceptance.** A broken reference produces a targeted error. After repairing
it, a clean Windows export launches from a different directory and completes
the sample interaction using the packaged content.

## Release checks

Verify new authoring features through edit, undo and redo, save and reopen,
Play, and standalone. Repeat the two-room demo from a fresh project and its
Windows export when changes affect scene loading, scripts, input, collision,
audio, or packaging. Use a schema version change when the saved format changes
and preserve loading of existing projects.
