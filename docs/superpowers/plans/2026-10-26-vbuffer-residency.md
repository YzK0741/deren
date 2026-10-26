# Same-frame texture residency implementation plan

> **For agentic workers:** Use superpowers:executing-plans inline; final independent review.

**Goal:** Load whole scene textures selected by current-frame visibility before shading that frame.
**Architecture:** A synchronous frame checkpoint separates graph visibility/feedback from resolve;
engine residency owns immutable shared sources and publishes only required lazy images.
**Tech Stack:** C++23 modules, Vulkan, Slang, existing lazy resources and render graph.
**Spec:** docs/superpowers/specs/2026-10-26-vbuffer-residency-design.md (approved).

## Global constraints

- Default off; static ordinary PBR eligible profile, geometry resident, MASK alpha sources pinned.
- Same-frame correctness, no placeholder shading for a required image; no whole-device wait_idle.
- Preserve stable shader indices and frame camera/items. Shared uint8_t source lifetime is explicit.
- Preserve user's unrelated files; existing checkout, no commit/push without request; build -j14.
- Execute continuously in this conversation; no repeated implementation approval between tasks.

## Review focus

- Descriptor writes race prior-frame readers: checkpoint must finish prior graphics work.
- Hybrid software winner absent from hardware ID: feedback shares resolve candidate selection.
- Completed recordings retain lazy handles: fallback publication and unload retries are separate.
- Prefix submit failure/acquire consumption: no premature presentation or unsafe reset.
- Borrowed source data and MASK/shared textures: owning bytes, dedup and conservative pinning.

## Tasks

### 1. Frame checkpoint contract and Vulkan implementation

Files: rhi.extension.cppm, rhi.contract.cppm, Vulkan core declarations/API/frame submission,
test_render_graph_frame.cpp, ABI fixture tests and RHI standard.
Interface: optional frame_checkpoint extension (new ability bit14), error checkpoint_and_wait(). ABI58.
- [x] Add failing offscreen frame prefix/suffix buffer readback and invalid-scope tests.
- [x] Implement submit/wait/reset/reopen while preserving acquire epoch/ring; complete pending leases;
      reject active rendering before mutation, preserve timing queries, handle failed submit/wait safely.
- [x] Validate repeated checkpoints, prior independent submission, unchanged default frame protocol.

### 2. Engine texture residency ownership/policy

Files: new engine texture_residency module, runtime declarations/constructor, test_texture_residency.
Interface: stable texture_record IDs, shared const vessel vector<uint8_t> source, material dependency
sets, pinned/wanted/published/physical readiness. Runtime registers streamable textures lazily and
publishes fallback into nonresident slots; only required lazy images are bound.
- [x] RED/GREEN tests for union/dedup/pinning, deferred unload and generation rejection.
- [x] Add owning CPU sources and conservative supported-scene/config predicate; eager fallback.
- [x] Add default-off visibility_texture_streaming setting and separate residency diagnostics.

### 3. Visibility material feedback

Files: new feedback Slang shader, engine pass/render graph declarations, CMake shader build/tests.
Interface: current material bitset (16384 bits), status header, bounded current-frame draw-item lookup.
- [x] RED/GREEN CPU decode tests and GPU known IDs/empty/tiny/hybrid candidate readback.
- [x] Implement full-resolution scan, atomicOr, clear/copy/host-read barriers; reuse candidate helper.

### 4. Graph split and same-frame integration

Files: runtime.post_graph.cpp, runtime.frames.cppm, graph compilation/execution helpers as needed.
Interface: two compiled graph segments, explicit cross-segment states/retention; checkpoint host phase
collects feedback, replaces undesired slots, releases views, attempts unload, loads desired bindings.
- [x] RED/GREEN split handoff and same-frame newly revealed texture tests.
- [x] Freeze camera/items/generation, update histories once, rebind all suffix logical state.
- [x] Implement eligible/fallback frame flow and safe resize/config transition behavior.

### 5. Acceptance, documentation and final review

- [x] Full Release build/CTest and device lane; validation-enabled targeted probes.
- [x] Eager/selective MASK-scene and sweep captures; separate window probe covers both slots and resize.
- [x] Bistro near/far/path smoke measurements: elapsed frame intervals, CPU wall time and residency/update time,
      actual residency/pinned/deferred bytes and counts; distinguish allocator blocks from payload.
- [x] Independent final review, fix important findings with regression, write evidence/docs.

## Ledger

BASE6de686cd. User approved the written design and execution; use inline implementation without a new
plan-approval round. Existing unrelated untracked files retained. Initial design builder check passed.


Ruling: use one compiled schedule partitioned before the named resolve pass rather than independently
recompiling two graphs -- preserves the exact compiler-produced handoff barriers and retained owners.
Ruling: coherent mapped storage publishes feedback directly to host after checkpoint, eliminating an
extra GPU copy; host visibility is an explicit recorder epilogue. No async or multi-queue promise.
Ruling: pin additional consumer materials conservatively; supported fallback frames restore full demand.

Final independent review found disable-restoration and overlay-pinning defects; both fixed. Integration
captures caught mip0-only views; complete-mip publication restored exact far/near/sweep RGB. Bounds,
empty feedback and window checkpoint/resize probes added. CTest71/71; device8/8; dedicated validation
probe153/0. Benchmark and limitations recorded in docs/reviews/2026-10-26-vbuffer-residency.md.
No commit/push; no full renderer resize/camera-reversal or device-loss stress claim.
