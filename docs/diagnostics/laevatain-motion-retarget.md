# Laevatain standalone motion: first playback and pose investigation

## Implemented path

The supplied Bibideba VMD spans frames 0..1288 at 30 fps (42.933 seconds),
with 51 bone tracks and no morph/camera keys. Offline conversion produces a
target-specific motion glTF under `asset/motion/bibideba_laevatain`, containing
52 animation channels and approximately 1 MB of binary keys. It contains no mesh
or material data. The input and generated motion remain local and ignored.

Native loading recomputes both skeleton fingerprints, validates animation data,
and maps motion node IDs to character node IDs before appending clips. A Laevatain
motion is accepted by Laevatain and rejected against the current Tangtang rig.
Hash compatibility is not a proof of motion quality.

## Reported calf distortion at 8 seconds

The first four sparse captures at 0, 8, 18, and 30 seconds established playback,
not faithful retargeting. The user correctly requested a closer pose review.

For the original direct-rest conversion at frame 240:

- The target thigh length remains 0.4546 and calf length 0.4147 on both sides.
  Only the designated root receives a translation channel.
- The right foot sole/toe-tip direction pitches downward approximately 76.5
  degrees, visually extending the shin silhouette. Right knee and ankle axial
  twist are approximately +7.1 and -7.5 degrees respectively.
- Source right-knee rotation magnitude changes from 6.4 degrees at frame 239 to
  26.3, 62.4, 82.0, and 103.0 degrees at frames 240..243. The largest one-frame
  step in that track is approximately 36.7 degrees, at 240..241. Exact integer
  frames use the original source keys, not interpolated estimates.
- Independent NumPy skinning baked to a static mesh produces the same rendered
  silhouette as runtime GPU skinning when camera/import shifts are aligned.
  This rules out an additional runtime skinning deformation for this reproduced
  pose, but does not validate the source-to-target calibration.
- Knee surface triangles show local linear-blend stretching despite fixed
  skeleton segment lengths. Stable bone lengths alone do not establish good skin
  deformation or anatomical correctness.

Private diagnostic captures/scripts live under `tmp/motion-preview`,
`tmp/motion-cpu`, `tmp/motion-side`, and `tmp/motion-aligned`; they are not build
or test dependencies.

## Reference-pose correction

Review of Endfield Poser's retargeting stages showed that the initial converter
was missing source-reference direction alignment. The converter now has explicit
canonical A/T reference choices, preserves the native foot frame, and transports
independent finger keys through the native parent bind axes. No source key is
filtered, no foot rotation is arbitrarily clamped, and bind nodes remain unchanged.
See `asset/motion/README.md` for the runtime contract and source-reference limits.

The chosen canonical A pose is an approximation. Exact pose agreement requires
the motion's actual source rest skeleton or a synchronized visual reference.
There is no claim that source capture discontinuities, planted-foot contacts,
clothing dynamics, or all character-specific deformation have been solved.

## Validation

- Native loader tests exercise node-ID remapping, incompatible rest matrices,
  malformed targets, hash tampering, atomic rejection, and matrix-only animated
  targets whose decoded TRS would lose the base pose.
- Converter tests cover handedness, shortest-arc interpolation, VMD truncation,
  preserved offsets, source T-pose alignment, native foot-frame preservation,
  and the interpolation block's physics-flag slots.
- Playback was captured on the actual Laevatain model, including `--motion=path`
  as the final command-line argument. The CPU-only verifier does not create Vulkan.
