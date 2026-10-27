# Screen-driven mip residency implementation plan

> **For agentic workers:** Use superpowers:executing-plans in the current feature checkout.

**Goal:** Reduce low-only time and unnecessary full-image uploads through screen-driven mip tiers.

**Architecture:** GPU resolve emits material extent demand to a separate completed-slot feedback
buffer. CPU policy budgets mip suffix images; bounded workers upload one finer tier at a time.

**Tech Stack:** C++23, existing Vulkan image factory, Slang, render graph.

**Spec:** ../specs/2026-10-27-screen-mip-residency-design.md

## Global constraints
- Preserve prior uncommitted progressive work, user files and baseline controls; default off.
- No per-frame checkpoint, frozen camera, CPU HZB, RHI ABI change or sparse image allocation.
- Keep pinned data and frame ownership safe; shutdown joins workers before device idle.

## Review focus
- Requests must use geometric footprint, not reduced resident image sizes.
- NPOT/single/partial-chain data must yield exact valid image byte ranges.
- Graph declarations and post-dispatch host publication must cover the new feedback writes.
- Replacement uploads and retired frame images must use actual tier sizes in accounting.
- Restore/re-enable, insufficient budget and failed upgrades must preserve valid sampling.

## Task 1: Policy and source mip ranges
- [x] Write failing tests in test_progressive_texture for suffix offsets, dimensions, tiers and budgets.
- [x] Implement mip range/filtering helpers and multi-level policy in progressive_texture.h.
- [x] Run pure tests, including missing mip generation and tight budget cases.

## Task 2: GPU demand and runtime tiers
- [x] Extend graph resolve inputs/parameters and pass recording with optional mip feedback.
- [x] Emit geometric desired extent in visibility_resolve.slang; allocate/read/reset per-slot buffers.
- [x] Add resident/upload mip metadata, exact byte accounting and one-tier asynchronous replacements.
- [x] Preserve two-tier/default modes, pinned sources, restore and shutdown behavior.
- [x] Build and run the complete CTest suite.

## Task 3: Validation and comparisons
- [x] Independent source review; fix material findings.
- [x] Device validation: fixed, moving, zero budget, restore and missing/partial mip source.
- [x] Serial two-round moving comparisons against two-tier control, plus full-preload references.
- [x] Record timings, quality and logical payload measurements with raw evidence and limitations.

## Ledger
- User explicitly said continue after proposing intermediate mip and screen-demand upgrades. Proceed
  inline under that authorization, with writing-plans and TDD; no additional approval gate.
- Existing branch codex/progressive-texture-residency is the authorized feature checkout.

- Complete: pure policy32checks, final73/73CTest including4measurement-evidence tests; sixacceptedVulkanvalidationcaptures.
- Review fixes: separate fourth push word preserves software flag; restore requires mip0 and no pending upload; failed memory sampling now fails the runner.
- Additional evidence for the earlier memory question: two independent WDDM runs,49/50steady samples,2.019->1.768GiB fixed-view dedicated usage.
- Measurements and limits are in docs/benchmarks/2026-10-27-screen-mip-residency.md. Preserve defaults off and leave work uncommitted for review.
