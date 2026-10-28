# Shared deformation resource migration

Continuation of the approved lifetime-group design; keep the previous uncommitted camera migration.
Scope: motion transforms, skin matrices, previous skin matrices and morph data, shared by scene,
shadow and traced deformation jobs. Preserve shader layout, heap slots and ABI32.（2026-10-18 核对：RHI ABI 现为 33，证据 source/promise/rhi/rhi.contract.cppm:300；ABI32 是当时的基线值，不是当前值。）
> 历史规划文档：本文写作时的 RHI ABI 为 33 / GUI ABI 为 3；当前值为 RHI ABI 59 / GUI ABI 5（见 `rhi.contract.cppm` 与 `gui.cppm`）。

- [x] Behavioral RED regression for ownership, setup data preservation, per-slot partial updates, history, provider handover/retirement and invalid admission.
- [x] Add deformation resource producer, append the missing previous-skin resource id, and adopt borrowed generations in runtime. Prepare at construction for setup writes; preserve old slot contents after completion before replacement.
- [x] Run Release/CTest/native/history regressions, frozen captures and resize; review and document.

Ruling: preserve mapped bytes during generation replacement rather than rebuild from partial CPU
skin history, because morph deltas and instanced motion data are written at setup and have no complete
CPU mirror. This copy happens only after an idle drain, never on normal frame reuse. Its cost is a
one-time full-buffer copy on provider handover (morph is 48 MiB per slot); wrong ordering could lose
setup state or race the GPU, so handover preservation and retirement need explicit coverage.

Raw morph scratch remains a borrowed view for immediate setup/frame writes; callers must obtain it
again after resource reconciliation. The animation controller already obtains it for each operation.
Ordinary frames must preserve all buffers (no zero_on_frame).

Frozen runtime: build-release-dyn-clang64/deformation-resources-check/runtime.before, includes the camera migration.

Ruling: fix the preexisting debug HDR write-after-write barrier found by the animated fixture gate.
Both frozen baseline and initial candidate reproduce it: G-buffer stores scene color into HDR when
TAA is inactive, but the debug stage transitions from undefined and loses the source write mask.
Use color_attachment as the source use before the debug clear. This is a concrete ordering correction,
not automatic-barrier redesign; validate the same native capture RED→GREEN.

Completed: RED11/56→GREEN99/99 deformation regression, skin history35/35, managed frames46/46,
schema605/605, Release build successful, final CTest24/24 in21.30s, native1164/1164.
Fifteen logical scenes sync-clean in candidate. Original13 static scenes and both animation scenes
with fixed0.025s/frame match baseline pixels. Debug baseline intentionally retains its original
WRITE_AFTER_WRITE; fixed candidate is clean. Resize2/2 clean. Independent review found no concrete
defect and confirmed the HDR correction. Evidence/report complete; no uncommitted code path is partial.

Legacy managed_frames test was updated from a16-byte scratch using motion_vectors id to the real
motion capacity/lifetime/flags and preservation semantics. Growth, failure/retry and retirement
remain covered; registry tests still cover scratch clearing. Previous-skin appends schema family40.
No deferred minor findings. Changes remain uncommitted alongside camera migration.
