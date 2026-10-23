# Software Routing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans inline. Use one final fresh-context review, no implementer agents.

**Goal:** Reduce hybrid rendering overhead in the user-approved order.
**Architecture:** Reuse the stable hardware-prefix/software-suffix split to generate direct work. Then improve triangle-scale selection and finally move survivor routing to GPU.
**Tech Stack:** C++23 modules, Slang, Vulkan/RHI, Python capture scripts.
**Spec:** docs/superpowers/specs/2026-10-23-software-routing-design.md

## Global constraints

Preserve user changes in the existing authorized dirty checkout. Do not commit/push. Software stays default-off; triangle path remains env opt-in. Whole-frame hardware fallback on refusal, no partial list. Preserve ID/depth/coverage semantics. Build with 14 workers. Pause and checkpoint at <=5% five-hour quota remaining.

## Review focus

Offscreen suffix IDs must retain gaps; capacity and malformed order must not partially publish; both frame slots share exact existing stride; CPU screenshot oracle must remain independent; legacy/HZB recovery must keep complete candidates.

### Task 1: CPU direct list
Files: visibility_software.cppm, runtime.cpp, test_visibility_software.cpp.
Produces: append_triangle_dispatch(split, order, projections, entries, budget), write_triangle_dispatch(lane, metadata).
- [x] Write tests against deduplicated CSR for on/off-screen mixed suffixes, invalid order/count, capacity, packed IDs and lane sentinel preservation. Observe RED.
- [x] Append entries from parallel stable split; serialize header/used entries without normal CSR; diagnostic-only CSR oracle. Observe GREEN.
- [x] Build -j14, full CTest, both-slot raw image and Bistro validation/correctness comparisons.
- [x] Three-round same-settings measurement; document actual CPU/GPU cost and limits.

### Task 2: Triangle-scale routing
- [x] Refine immutable metadata layout from geometry/source capability and stage-1 results; tests first for edge bounds, invalid/near-plane and long/thin cases.
- [x] Implement opt-in triangle metric, preserve old metric A/B and IDs/fallback.
- [x] Validate and measure thresholds; record policy and acceptance without claiming universal gains.

### Task 3: GPU survivor routing
- [ ] Refine bounded buffers/arguments and graph ordering with recovery/overflow/ID contracts.
- [ ] Add failing tests in test_dynamic_link.cpp for validate_dispatch_indirect_range (12 bytes, 4-byte alignment, subtraction-safe range); dispatch_indirect on command_buffer/mergeable_command_list; ABI48 pins. Implement conservative default unsupported, Vulkan frame forwarding/native ownership/indirect-flag validation, enabled device_capabilities.compute_indirect query.
- [ ] Prepare GPU route metadata/argument shared buffers per fenced frame slot in software_visibility_pass, explicit graph imports. Metadata header: threshold bits, near plane, projection scale, projection kind, bounded budget, reserved padding to8 words; then one outward-rounded object-edge float per original meshlet candidate. Arguments seed0/1/1. New heap families follow atomic scratch run.
- [ ] Integrate GPU classification/reservation/emission into existing hzb_cull.slang primary/history-invalid/recovery compaction (mode bit2; bit1 remains recovery). Temporary eligibility flag bit4 normalized to final0/1/3. Bounded CAS reservation falls open per item to hardware. Atomically maintain indirect X/Y (flatten stride65535).
- [ ] GPU runtime mode: retain original candidate order and skip CPU copy/project/split/CSR; publish metadata only for owned eligible records, infinity for unsupported policy. Software kernel consumes GPU count via dispatch_indirect. If GPU prep/capabilities/HZB unavailable use valid CPU routing.
- [ ] Extend HZB graph parameters with optional camera/route metadata/list/args; software graph parameters with indirect args read. Move needed work-list imports before HZB. Prepare resources before graph assembly; host candidate publication still occurs while recording primary visibility, before queue submission.
- [ ] Capture-only: publish results/survivors/list shader writes to host, validate visible-candidate partition against HZB flags and reconstruct CPU reference from actual GPU software IDs + immutable candidate geometry. Validate bothslots, recovery, disabled-history, forced budget exhaustion. No production count readback.
- [ ] Implement GPU routing after final visibility decisions without CPU readback.
- [ ] Validate coverage, fallback, both slots and measured full-frame benefit.

### Task 4: Closeout
- [ ] Fresh read-only review, fix Important findings with regressions, full verification, docs/checkpoint. No commit/push.
