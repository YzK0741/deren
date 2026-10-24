# api_core subviews — 2026-10-24

## Result

RHI ABI 50 splits the core into borrowed `resources`, `commands` and `frames` public members.
Their pointer-sized `context_view<T>` wrappers expose arrow/get/bool, with private binding and
no assignment/reset. Member names are ordinary names without prefixes/suffixes. Derived constructor
bodies bind once after their interface bases or context members are constructed. No const_cast,
mutable binding, additional ownership or per-view allocation is used.

Vulkan implements all interfaces on the same core allocation; a member-owned probe verifies the
alternative construction shape. Root factory/frame/submit slots were removed and production/test
callers migrated. The root keeps release, ABI, capabilities/extensions, native context and idle.
Resource-cluster ownership, recorder ownership and completion tokens keep their existing protocols.

`commands->submit(buffer/batch)` submits independent work. The borrowed current-frame primary is
rejected with `invalid_argument`; `frames->submit()` submits the acquired frame without a buffer
argument. Tests verify that rejection does not prevent the subsequent frame submission.

## Verification

- Red compile-time ownership tests initially rejected the missing subview contract.
- Full Release build with 14 jobs passed (`tmp/subviews-build-final.log`).
- Full CTest: 68/68 passed (`tmp/subviews-ctest-final.log`).
- Standalone completion device test: 63 checks, zero failures.
- Render-graph frame device test: 12 checks, zero failures, including frame/independent rejection.
- Both device runs logged zero VUID/validation/error messages.
- Windowed Gaussian and PBR: eight frames each, exit 0, capture produced, zero validation errors.
- Gaussian GPU sort completion consumer: PASS 48 cases.
- Normal interactive startup rendered repeatedly and closed gracefully with exit 0.
- Independent review found no actionable correctness issues; its test/documentation suggestions
  were incorporated and verified. Follow-up review also found no actionable issues and confirmed
  those gaps were addressed.
- `git diff --check` passed (existing line-ending notices only).

Local device evidence: `tmp/subviews-device-final.log`, `tmp/subviews-test_submission_completion.log`,
`tmp/subviews-test_render_graph_frame.log`, their separate `-validation.log` files,
`tmp/subviews-smoke/results.json` and per-scene/startup logs and captures.
The only source changes after the final build/device runs were comment reference corrections.

## Limits

This does not add a new backend or redesign native queues, texture-group layout, frame completion,
or multi-queue scheduling. The pre-existing device-suite failures documented in
`2026-10-24-rhi-command-recorder.md` are not claimed fixed or passing. Startup smoke does not diagnose
the earlier intermittent manual startup report. All views borrow the core; using them before binding
or after core destruction violates their lifetime precondition. No commit or publication was requested.
