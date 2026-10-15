# TLAS and hit-shading instance records — 2026-10-14

Each completed TLAS frame slot now owns a resource-only passchain subchain declaring its top-level
structure and coherent shader instance table. It borrows both handles and retains a snapshot; capacity
growth prepares/adopts the pair atomically. Normal begin/add reuse performs no registry reconciliation,
resource allocation or idle wait. RHI ABI32, build inputs, record layout and heap update paths remain unchanged.

Two existing growth defects are fixed. The previous code replaced the instance buffer before checking
creation success, losing the old table on refusal. It also did not copy records already written during
the current frame when capacity doubled past256. Admission now validates the complete candidate and
copies the used prefix while the old snapshot still retains its source; failure keeps old buffers,
capacity, count, pending instances and public snapshot. Retry preserves the first256 records.

The managed registry has a typed acceleration_structure request/instance path, currently restricted
to top-level descriptors without borrowed geometry pointers. BLAS descriptions will need owned geometry
and dependent resource retention in the next batch. rt_instance_table is appended as an engine id;
existing ids/heap values are unchanged. Subchains have one allocation pair for their completed slot;
the parent still addresses logical TLAS instances by frame slot.

Production structure_set receives core_owner and forwards it to TLAS. Slot chains drain before releasing
objects; retained snapshots hold the device. The RT phase waits before destroying micromaps, ensuring
that its existing teardown cannot free them before submitted work finishes. Reference-only callers keep
their core alive. The portable AS regression is now included in CTest; its stale ABI31 assertion is32.

Validation:

- RED4/265: missing ownership, failed growth losing old buffer, lost growth prefix, and stale ABI assertion.
  Expanded GREEN279/279 covers AS/buffer creation failures, growth retry/prefix, reuse, slot isolation,
  snapshot/device retention and managed object identities.
- Actual Vulkan AS/build/refit/micromap/traversal instrument362/362; runtime1164/1164.
- Release build successful, CTest25/25 in26.09s. The added default AS test uses doubles and opens no device.
- RT smoke, scaled MegaLights+RT+TAA and fixed-time animated skin/refit:40frames/.5-degree sweep each,
  old/new RGBA identical and clean synchronization validation.
- Scaled MegaLights+RT+TAA maximize/restore2/2 clean; final1280×720 client/output match.
- Independent read-only review found no new actionable issue.

Evidence: [captures, configs, logs, scripts and SHA256](2026-10-14-tlas-resources/).
Frozen runtime.before includes all Gaussian migrations. PNG compression changes only lossless IDAT encoding.

BLAS, micromaps and mask/skin output buffers remain; RT migration and the whole-pass goal are incomplete.
Shared textures/samplers and the final ownership audit also remain. Changes are uncommitted.
