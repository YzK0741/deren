# Review fixes and subtilis follow-up

Execute the user's four fixes sequentially in the current workspace; preserve APIs and avoid unrelated refactors. Then refine the existing subtilis design against current visibility code. No subtilis product implementation or commit/push in this task.

- [x] Shared ownership: reproduce concurrent last-strong/last-weak release under ASan, replace dropping flag with an implicit weak reference held until object destruction ends; cover self-observing objects and weak lock.
- [x] RT inputs: add an adapter-level geometry-to-RT barrier test, observe incorrect destination stage, declare sampled inputs as ray_tracing.
- [x] GUI attachment dependency: test a typed overlay handoff through a probe; insert color-output write to read/write synchronization between the two rendering instances, with failure blocking submission.
- [x] Overlay inheritance lifetime: reproduce copy/move or disabled-NRVO failure; rebind internal pointers after construction/copy/move.
- [x] Full Release and relevant ASan checks, GUI device capture with sync validation, independent review.
- [x] Read the existing subtilis spec and actual meshlet/visibility interfaces; document a small staged implementation with conservative cut coverage, boundary preservation and overflow fallback. Explain design choices and remaining tradeoffs without implementing it yet.

Evidence and limitations: [review record](../../reviews/2026-10-19-render-graph-review-fixes.md). RHI ABI 46; GUI ABI 5. subtilis §11 remains a proposed route, with no geometry product implementation in this batch.
