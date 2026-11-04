# Standalone motion assets

Each motion lives in its own directory containing a skeleton-only `motion.gltf`
and `motion.bin`. Geometry, materials, and textures stay in the character asset.
The current player binds one external motion asset to the single-model scene.

From the repository root, with the build runtime dependencies on PATH:

```powershell
python scripts/assets/convert_mmd_motion.py --model asset/laevatain/laevatain_game_toon_preview.glb --vmd "PATH/TO/Bibideba.zip" --output asset/motion/bibideba_laevatain --deren build/deren.exe --source-pose poser-a
build/deren.exe --verify-motion asset/laevatain/laevatain_game_toon_preview.glb asset/motion/bibideba_laevatain/motion.gltf
build/deren.exe --motion asset/motion/bibideba_laevatain/motion.gltf
pwsh -File scripts/assets/preview_motion.ps1
```

Use `--config` to select a working character preview configuration. The converter
accepts a VMD or a ZIP containing exactly one VMD. `--mmd-unit` explicitly controls
model units per MMD unit (default 0.08); the source VMD does not contain a reference
skeleton from which an exact scale could be recovered. The current offline profile
supports the Bip001 body and finger chains; it does not simulate cloth or hair,
provide expressions, or solve planted-foot contacts. Unknown source tracks are
reported. Animation keys are sampled at the source's 30 fps.

`--source-pose mmd-a` (default) aligns body/limb directions to a canonical upright
45-degree A pose before applying the motion. `mmd-t` selects horizontal arms;
`native` reproduces direct application to the unchanged target rest pose. These
are explicit approximations because the source VMD carries no source skeleton.
Foot frames remain native to avoid introducing a constant high-heel tilt, and
finger deltas use native parent bind axes while inheriting the aligned hand.
The stored binding nodes and their fingerprint remain the original target rig.
Reference-pose alignment changes animation keys, not bone lengths or bind data.

`poser-a` adds the standard MMD leg reference used by Poser and derives root
translation scale from the source and target leg lengths. `--mmd-unit` overrides
the computed scale.

Calibration stages were checked against the open-source
[Endfield Poser retargeter](https://github.com/OedoSoldier/Endfield-Poser/blob/master/src/math/mmd_retarget.h).
Its source-reference selection, foot-frame handling, and native finger-parent
convention helped identify missing calibration in the initial converter. This
tool does not embed that plugin or use its game hooks, character data, or PMX loader.

## Binding contract

`extras.deren_motion` contains `binding: "bip001-rest-v1"`, `skeleton_hash`, and
the `joints` array of motion-file node IDs. The nodes include the union of the
character's skin joints and their ancestors, with original rest transforms. No
glTF skin is required because this document does not contain skinned geometry.

The native verifier computes XXH3-128 over the following canonical UTF-8 text:

- Prefix `deren-motion-rest-matrix-v1` followed by a newline.
- Entries sorted by root-relative, slash-separated node path. Names must be
  nonempty and cannot contain slashes; duplicate paths are rejected.
- For each entry: byte length of the path, `:`, path, `:`, joint membership
  (`0` or `1`), then sixteen `:`-separated signed decimal matrix components and
  a newline. Components are column-major local rest-matrix values multiplied by
  1,000,000 and rounded to the nearest integer, with halves away from zero.

Matrix form canonicalizes equivalent quaternion signs and matrix/TRS storage;
rounding canonicalizes negative zero and insignificant float noise. Bone lengths,
parent paths, and rest orientation are part of compatibility; file node ordering,
skin palette ordering, mesh data, and scene placements are not. Digest text uses
the existing `data_block<16>::to_hex_string()` convention. Both the motion and
character skeleton hashes must match the declaration. Compatibility does not
replace animation data validation: channels, targets, key times, and finite values
are checked before any clips are appended. Mismatches never silently retarget.

The downloaded VMD and converted dance data remain local. This README and the
conversion/verification code can be shared independently of those data files.

Matrix-form binding nodes can participate in fingerprinting, but animated target
nodes must expose a decoded TRS rest pose matching their local matrix. The player
rejects matrix-only animated targets rather than dropping their translation/scale.
