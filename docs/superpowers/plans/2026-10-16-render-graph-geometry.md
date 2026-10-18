# Geometry attachment graph implementation plan

Goal: extend the opted-in main graph to own the scene attachment producer, then lighting and post in the same execution. Continue inline in the authorized checkout without committing unrelated work.

Design: add a pure geometry appender declaring discard writes for three surfaces, velocity, HDR and depth. The existing scene pass keeps its secondary recording strategy, with a checked content entry point. Select and freeze main graph eligibility before scene recording; supported frames defer scene recording to the composed graph, importing attachments with undefined contents. Unsupported frames keep the existing producer and tail. Preserve external host/bindless/material/mesh/deformation/cluster/shadow protocols; this does not claim a complete scene resource dependency graph. No new mesh/task stages are needed for attachment writes.

（2026-10-18 核对：“Unsupported frames keep the existing producer and tail”已不成立：legacy scene tail 与其启动开关被删除后，资格判定只决定“图能否录制本帧”，不能录制的帧不再回退到旧 tail（缺 G-buffer 管线时清屏呈现、缺 post/overlay 时判失败）。证据：source/engine/runtime/runtime.frames.cppm:3146-3184；docs/reviews/2026-10-17-render-graph-gate.md:997-1019。本切片自身的结论（附件生产者、六条 discard 写、视口/附件顺序）不受影响。）

- [x] RED: CPU composition from undefined attachments requires geometry before lighting; assert six producer accesses and producer-to-lighting barriers. Runtime probe must show no early geometry recording on supported frames, and no scene recording on graph preparation failure.
- [x] Implement geometry appender and checked scene content, preserving legacy wrapper and secondary scheduling. Reject incomplete targets/frames; close rendering and propagate secondary failures.
- [x] Freeze eligibility before geometry; append and bind geometry ahead of lighting, retain velocity and attachment generations, remove duplicate manual attachment transitions on the opted-in path. Keep timestamp order and legacy flags accurate.
- [x] Build -j12; focused/full Release and ASan tests; independent review; actual GPU equality, fallback, timings and resize validation. Document scope and evidence.

Review focus: empty scenes still clear all attachments; preparation failure must not leave pre-recorded geometry; callback/secondary failure blocks submission; rim-only fallback must retain legacy geometry; viewport, attachment order and secondary inheritance must match the unchanged scene declaration.

Acceptance: Release 44/44, ASan 17/17; five GPU legacy/main pairs byte-identical with GPU timings enabled; twenty rapid window resizes clean under synchronization validation; expanded backend boundary gate zero. Independent read-only review found no actionable P1/P2 issue. Detailed evidence: docs/reviews/2026-10-16-render-graph-geometry.md.
