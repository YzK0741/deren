# Shared camera resource migration

Continuation of the accepted passchain lifetime-group design. Scope: the shared camera input of scene,
shadow, deferred and post passes. Preserve shader slots, ABI32, projection/jitter/history calculations,
frame completion and descriptor publication ordering.（2026-10-18 核对：这里记的当时值是 ABI32，RHI ABI 现为 33，证据 source/promise/rhi/rhi.contract.cppm:300；本批的“不改 ABI”约束本身仍成立。）
> 历史规划文档：本文写作时的 RHI ABI 为 33 / GUI ABI 为 3；当前值为 RHI ABI 59 / GUI ABI 5（见 `rhi.contract.cppm` 与 `gui.cppm`）。

- [x] Add behavioral regression for camera ownership, heap binding, slot isolation, reuse, chain handover/retirement and invalid admission before acquire.
- [x] Extend scene_constants_pass with the camera request and optional selection. Replace runtime camera ownership with a borrowed snapshot; initialize new generations, upload via checked frame-buffer writes and publish existing heap slots.
- [x] Release build, portable/native tests, frozen-baseline scene captures and resize. Review independently and update inventory/management documentation.

Ruling: retain constructor output-only preparation but include camera there, because setup-time scene
heap publication needs a usable camera buffer. Shadow, cluster, light/head stay lazy. Do not migrate
deformation/AS/Gaussian storage in this batch: their initialization and addressed subranges require a
separate lifecycle contract. No automatic barrier or new resource kind is needed for camera.

Baseline: 97883342; frozen runtime under build-release-dyn-clang64/camera-resources-check/runtime.before.

Progress: ownership regression RED1/2 before implementation, GREEN55/55 after migration and added
scope/failure recovery cases. Existing constants60/60, Release build successful, CTest24/24 in19.77s,
native Vulkan1164/1164, 13 frozen-baseline captures pixel-identical and sync-clean, ML/RT/TAA
maximize/restore2 resizes clean. Independent final review found no important/critical defects;
inventory, management documentation and evidence report complete. No scope/ruling changes beyond
the constructor camera exception above. Working changes remain uncommitted.
