# Software Routing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans inline. Use one final fresh-context review, no implementer agents.

**Status (closed 2026-10-23):** implementation + acceptance evidence complete; final fresh review done; M4 recorded as experimental/opt-in/default-off/NOT accepted (`docs/reviews/2026-10-23-m4-decision.md`). Task 3 boxes are ticked with evidence; Task 4 is the completed review. The Subtilis phase is closed per user decision: software raster is optional/default-off and not optimized further; see the [final closeout](../../reviews/2026-10-23-subtilis-final-closeout.md).

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
- [x] Refine bounded buffers/arguments and graph ordering with recovery/overflow/ID contracts. `software_route_metadata`/`software_route_arguments` are per-fenced-frame-slot heap families (metadata header: threshold bits, near plane, projection scale, kind, bounded budget + padding; one outward-rounded object edge per original candidate; argument seed 0/1/1), imported explicitly into the graph with the work list; recovery/overflow/ID rules are enforced in `software_route.slang` and `validate_gpu_partition`.
- [x] Add failing tests in test_dynamic_link.cpp for validate_dispatch_indirect_range (12 bytes, 4-byte alignment, subtraction-safe range); dispatch_indirect on command_buffer/mergeable_command_list; ABI48 pins. Conservative default unsupported, Vulkan forwarding/native ownership/range+alignment validation, enabled `device_capabilities.compute_indirect` query. Tests pass in the full suite.
- [x] Prepare GPU route metadata/argument shared buffers per fenced frame slot in software_visibility_pass, explicit graph imports; new heap families follow the atomic scratch run (`software_route_metadata`/`software_route_arguments` after the software images).
- [x] Integrate GPU classification/reservation/emission into hzb_cull.slang primary/history-invalid/recovery compaction (mode bit2; bit1 recovery; temporary eligibility bit4 normalized to 0/1/3; bounded CAS reservation falls open per item to hardware; indirect X/Y atomically maintained with the 65535 flatten stride).
- [x] GPU runtime mode retains the original candidate order and skips CPU copy/project/split/CSR; it publishes metadata only for owned eligible records, infinity for unsupported policy, and the software kernel consumes the GPU count via dispatch_indirect; CPU routing remains the valid fallback when GPU prep/capabilities/HZB aren't available.
- [x] Extend HZB graph parameters with camera/route metadata/list/args and software graph parameters with the indirect-arguments read; work-list imports are moved before HZB; resources are prepared before graph assembly and host candidate publication still happens while recording primary visibility.
- [x] Capture-only: results and survivors/list writes are published to host, the visible-candidate partition is validated against HZB flags/survivors/counts, and the CPU reference is reconstructed from the ACTUAL GPU software IDs plus immutable candidate geometry (both slots, recovery, closed history, forced budget exhaustion; capture manifests + the CPU-oracle gate were added). Recovery is accepted on the textured near frame (hardware recovery 314 / recovered visible 327, 30 covered pixels, 2 ULP); the far textured recovery frame validates its partition but its image comparison differed at 1 pixel, so it is not claimed as image evidence. No production count readback.
- [x] GPU routing after final visibility decisions without CPU readback: implemented (recovery compaction routes after its final decision; production only consumes indirect arguments). Evidence: capture logs/route reports above.
- [x] Validate coverage, fallback, both slots and measured full-frame benefit. Both slots verified byte-identically at 115,446 pixels/2 ULP; closed-history fallback verified; same-build 3-round matrix shows GPU routing removes the CPU routing overhead (cut wall -0.63/-0.47 ms far/near vs CPU routing) but NO stable net whole-frame benefit (deltas inside run-to-run spread; near CPU-active not improved) - negative result documented, software stays default-off and M4 not accepted.

### Task 4: Closeout
- [x] Fresh read-only review, fix Important findings with regressions, full verification, docs/checkpoint. Done: `docs/reviews/2026-10-23-software-routing-review.md` (0 Critical / 4 Important / 12 Minor); I1 stale 26x claim fixed, I2 inspected-only waiver + E1 HZB-only capture follow-up, I3 developer-only diagnostic documented, I4 explicitly waived; disposition in `docs/reviews/2026-10-23-m4-decision.md`. M4 remains NOT accepted.
