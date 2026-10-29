# Environment lighting implementation plan

Execute inline using writing-plans, executing-plans, test-driven-development and final independent
review. The user approved starting the four-part environment workflow after the visual discussion.
Current tracked workspace is clean at b7138fc8; preserve unrelated untracked files. Work in the
current checkout to reuse its validated GPU build/assets. Do not resume performance optimization.

- [x] Preserve baseline binary/config and acquire pinned reference HDR/tone-map source/licensing.
- [x] Add failing tests for environment controls and config parsing; implement shared sanitization.
- [x] Append LightUBO environment settings, unify diffuse/specular/background transforms, add runtime
  controls, config and GUI bindings. Keep default behavior unchanged.
- [x] Add selectable PBR Neutral tone mapping with proper attribution and post-layout coverage.
- [x] Add asynchronous HDR selection/presets, transactional publication and retained previous state
  on errors. Document controls and provide offline-usable asset fetch instructions.
- [x] Build/test, compare default and representative HDR captures, inspect images, independently
  review and deliver settings/results with stated limits. No commit/push requested.

Acceptance: `docs/reviews/2026-10-29-environment-lighting.md`. Release/shader build passed,
75/75 CTest passed, 12 successful captures, exact default/first-frame exposure regressions,
background interior tolerance <=1 RGB code value. Independent review fixes verified. Manual GUI
interaction and cross-platform execution were not tested. GPU publication retains wait-idle.
