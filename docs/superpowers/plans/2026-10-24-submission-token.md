# Submission token implementation plan

> **For agentic workers:** Use superpowers:executing-plans for inline execution.

**Goal:** Expose copyable, non-owning submission completion values for independent graphics work.

**Architecture:** A new optional `command_submission` extension returns `expected<submission_token,error>` from typed primary/batch submits and provides poll/wait. Tokens contain process-unique device ID and successful submission sequence. Vulkan lazily creates a completion timeline and signals it only for tracked submissions, retaining existing pending-resource bookkeeping. Completed values survive pending-record reclamation. Existing frame submit/present and untracked submit remain unchanged.

**Tech Stack:** C++ modules, Vulkan timeline semaphores, existing RHI ownership, CMake/Ninja.

**Spec:** Value-token proposal approved in this conversation on 2026-10-24.

## Constraints and review focus

- Core and backend must outlive use of tokens; tokens do not own or cancel work.
- Failed submissions return no token and consume no published sequence.
- Poll: ok/not_ready; wait: ok/timeout. Invalid, cross-device and future tokens return invalid_argument. Device loss is an error, not completion.
- Wait does not hold submission/queue locks while blocking or wait for later work.
- Completion is not resource ownership or a replacement for recorded memory barriers.
- No new vtable slot or layout change to an existing interface: ABI remains 49; append a new ability bit.
- Preserve workspace changes; no commit requested; build with 14 jobs.

### Task 1: Contract and failing test

- [x] Define token/extension and capability documentation after observing missing-contract test failure.
- [x] Add a standalone device probe covering actual copies, blocked queue timeout, reusable completion values, batch submissions, invalid/foreign/future tokens and failed submissions.

### Task 2: Vulkan and consumer

- [x] Factor internal primary/batch submits to accept optional output token; preserve old entry points.
- [x] Signal one per-core completion timeline, use it for tracked pending records and implement poll/timed wait.
- [x] Allocate device identities from shared utility so device recreation/module boundaries cannot recycle an ID.
- [x] Migrate an independent readback probe with fallback for backends lacking the extension.

### Task 3: Verification

- [x] Build, run headless CTest and standalone GPU completion probe with validation enabled.
- [x] Smoke-test windowed rendering and startup; retain documented baseline device-test failures.
- [x] Fresh independent review, fix findings, document limits and results.

## Execution record

- Full build and 68/68 CTest passed. Device completion probe: 63 checks, validation clean.
- Gaussian GPU sort diagnostic token consumer: 48 cases passed; Gaussian and PBR windowed captures succeeded with zero validation errors. Normal interactive startup rendered repeatedly and closed normally.
- Fresh independent review and final test/docs review found no actionable issue. Native failure/device loss not injected; limits recorded in docs/reviews/2026-10-24-submission-token.md.
- Ruling: optional extension preserves existing submit ABI; a lazily-created per-core timeline avoids per-token native fences and unbounded completed-token records. Existing untracked/final frame submits keep their paths.
- Ruling: completion domain IDs come from shared utility rather than core addresses, preventing aliasing across core recreation; token still does not own the core.
- Ruling: migrate the isolated GPU sort readback diagnostic first; frame screenshots/resize/destruction retain their existing waits.
