# Resource group implementation plan

> Use superpowers:executing-plans inline; one independent final review.

**Goal:** General immutable groups with texture_group as a member; no exposed descriptor container.
**Architecture:** Shared contract resource members, pipeline logical layout, private reusable Vulkan
slots, immediate load and existing command/submission retention. ABI 51.
**Spec:** docs/superpowers/specs/2026-10-24-resource-group-design.md
**Constraints:** Preserve pre-existing work, ordinary members, no lazy recording, build -j14, no commit.

- [x] Add failing contract/layout tests, verify red.
- [x] Add group types, factory/load verbs, layout validation, ABI 51.
- [x] Implement Vulkan slot allocation, typed validation, pipeline layout/recording and retention.
- [x] Resolve fixed texture-group reservation overlap, add mixed GPU shader/readback tests.
- [x] Full build/tests, device regressions, windowed smoke, independent review and docs.

## Review focus

Wrong-device and wrong-kind resources; immutable publication/failure rollback; host range overflow;
layout/push overlap; group lifetime across rerecord and pending secondary/frame/batch; descriptor reuse
only after completion; shader/native descriptor stride agreement; GUI workaround unchanged.

## Execution rulings and outcome

Executed inline in the existing checkout, preserving unrelated work. Builder review resolved lifetime
aliases and early fixed reservation. Independent code review corrected per-stage capability validation
and native uniform/storage constraints. Vertex-only depth support required optional fragment-module
creation in the existing native pipeline builder. Fixture corrections and all final checks are recorded
in docs/reviews/2026-10-24-resource-group.md. No lazy recording or pass migration added. No commit.
