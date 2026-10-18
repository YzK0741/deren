# GBuffer handoff and lighting graph implementation plan

> **For agentic workers:** Use superpowers:executing-plans inline, with failing tests before implementation and one independent review at completion.

Goal: migrate the first main-chain boundary: existing geometry produces GBuffer/HDR, then one composed graph owns GBuffer sampling, deferred LOAD lighting and post processing.

The approved composition mechanism stays unchanged. Geometry, shadow/cluster preambles, immutable material/IBL inputs, host buffer publication and submission remain external protocols. This is an incremental boundary migration, not a claim of complete scene access modeling. No task/mesh or AS stages are invented for these external operations.

Startup main_render_graph implies post_render_graph, defaults false. Select per-frame before recording graph commands. TAA/debug/RT/MegaLights/Gaussian/transparent/character branches use the existing tail and optional post graph; no fallback after graph recording starts. GBuffer imports describe the actual current-frame attachment producer, avoiding a new cross-frame cache.

（2026-10-18 核对：本段描述的是该切片当时的选择协议，已不成立——`main_render_graph`/`post_render_graph`/`bloom_render_graph` 三个配置键与三个启动 setter 均已删除，legacy tail 也已删除，图是唯一录制路径；TAA(10/11)、GBuffer debug(12)、RT 阴影(18)、Gaussian(17)、透明(13)、角色(14) 现在都是图内 lane，只有 MegaLights 曾长期留在图外、随后被整链移除。证据：docs/reviews/2026-10-17-render-graph-gate.md:997-1014、source/application_configuration/application_configuration.cppm:255、source/engine/runtime/runtime.post_graph.cpp:36-40、source/engine/runtime/runtime.frames.cppm:3181-3184。同理，下方任务 3 里“add main_render_graph config/startup setter … safe legacy-tail fallback”的产物已被后续清理撤销。）

- [x] Add CPU test_render_graph_lighting: three GBuffer sampled inputs, depth read, HDR preserve write; reject aliases/undefined HDR; compose with post and test cross-boundary barriers/culling. Observe missing API RED.
- [x] Implement lighting.cppm append with typed parameters; add checked deferred_pass::record_content without legacy barriers. Keep legacy record wrapper.
- [x] Add main_render_graph config/startup setter, per-frame eligibility and safe legacy-tail fallback; extend record_post_graph to import GBuffer generations and append/bind lighting before post, retain stable payload and correct timing marks.
- [x] Extend tests for config and content errors; build Release -j12 and run full suite.
- [x] Validate actual GPU legacy/post/main image equality, FXAA/upscale, unsupported-branch fallback, sync validation and window resize; ASan/boundary gates, independent review, docs/evidence.

Ruling: Work in the existing authorized checkout, preserving unrelated uncommitted work. No commit/push. User's continue authorizes inline execution of the next main-chain slice.

Ruling: GBuffer drawing remains an explicit external producer for this slice. Its bindless geometry/material and secondary command behavior cannot be certified by an attachment-only graph declaration. Moving the consumer boundary first gives a fully testable lighting-to-post composition without misrepresenting those accesses.

Acceptance: Release build with -j12 passed; full CTest 44/44 and focused ASan suite 16/16 passed. Expanded backend boundary scan found zero dependencies. Six legacy/main GPU pairs (including debug and TAA fallback) were byte-identical, three window resizes passed synchronization validation, and an additional GPU-timings-enabled pair matched without ordering warnings. After adding an explicit standard header, rebuild and focused post/lighting/docs tests passed 3/3.

Independent review's rim-only fallback finding was reproduced by two failing tests and fixed by checking both contour features directly. Runtime probes also cover the real empty-scene attachment producer, callback failure and preparation failure. Detailed scope and evidence: docs/reviews/2026-10-16-render-graph-lighting.md.
