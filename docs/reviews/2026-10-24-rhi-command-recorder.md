# RHI command recorder consolidation

## Result

RHI ABI 49 introduces `command_recorder`, shared by the sibling owners `command_buffer`
and `mergeable_command_buffer`. Only `command_buffer` exposes individual submission and
`execute(batch)`. Direct owner recording calls remain unchanged.

Heap bind/push now carry one recorder pointer; scoped synchronization, radix sorting and
mesh dispatch accept the same recording interface. Runtime heap helpers and pipeline
binding no longer branch between duplicate recording vocabularies. Vulkan uses one
live-registry resolver before implementation casts. Reserved texture-group push bytes
are checked for both primary and mergeable targets.

Resource ownership, heap/shader layout, pending retention and submission completion are
unchanged. The internal mergeable adapter still delegates to its retained secondary;
removing that composition is not needed for sharing the public recording interface.

## Verification

- Red: `test_rhi_ownership` failed to compile because the common base did not exist.
- Green: compile-time tests prove both owners derive from the recorder, remain siblings,
  and only the ordinary buffer can receive execute or be submitted individually.
- Full Release build with 14 jobs; 68/68 headless CTest checks passed.
- Real-device recorder tests exercise forged concrete tags, mergeable heap bind/push,
  scoped buffer barriers and ended-state refusal. Optional empty radix-sort recorder
  checks are included but skipped on this machine because GPU sorting is unavailable;
  they do not cover an actual mergeable GPU sort.
- Gaussian flowers and PBR DamagedHelmet each rendered 8 stationary-camera frames and
  saved a final windowed screenshot. Both exited successfully, the GUI was present
  (at least 2883 / 2894 bright pixels in its text region across two runs), and no validation error was logged.
  This is a smoke check, not a long-duration flicker measurement.
- Independent review found no actionable issue in the scoped changes.

## Existing device-test failures, checked against ABI 48

The pre-change source snapshot at `a777f7d5b80b666c2eddbaff7244161eacd44701`
was rebuilt separately in `tmp/repro-application-build`. Its RHI implementation is
unchanged by the application repro's optional GUI workaround. These failures reproduce
in both versions:

| Suite | ABI 48 | ABI 49 | Failure |
| --- | --- | --- | --- |
| Backend boundary spike | 1 | 1 | Headless present returns `unsupported`; old assertion expects `not_ready` |
| Texture group | 1 | 1 | Texture-group pipeline creation fails before sampling |
| Runtime device | 1034 | 1034 | Texture-group creation/allocation checks fail |

The texture-group table's fixed slot 17664 is inside the current scene grid
16384..18431. Grid reservation already advances the heap cursor beyond that fixed
base; the later texture-group reservation cannot return the required base. Both the
layout constants and reservation code predate this change. Repair requires a separate
heap/shader layout decision and is deliberately outside this interface consolidation.

Consequently the new mergeable reserved-push-range regression is present in
`test_texture_group`, but its device execution remains blocked by the existing pipeline
creation failure. Do not count that check as passed. The full device suites are not green.

Local raw evidence: `tmp/rhi-recorder-verification/`, `tmp/rhi-recorder-ctest.log` and
`tmp/rhi-recorder-build-final.log`. No commit or publication was made for this change.
