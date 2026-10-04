# VERIFY buffer face — independent verification of phase 1 (T2 + T3)

> **This is the authoritative copy** (2026-10-03 21:05): it lives under `docs/rhi/`, a path no
> build step owns. The original was written to
> `build-release-clang64/deren-ab/rhi/VERIFY_buffer_face.md`, which was **transiently unreadable
> at 21:04:54** while the surrounding build tree was being churned by something outside the team.
> See §0.1 for the measured evidence and for why the certified hashes must be treated as a
> rebuild target rather than as a live file path.

Verifier: `verify-boundary` (wrote no product file; read-only on the product tree).
Task: `task-4`. Lead: `lead`. Writers: `kit-runtime` (task-2, `vulkan/runtime/**`),
`kit-resources` (task-3, `vulkan/ray_tracing|acceleration_structure|primitive|readback|pipelines|core/filter`).
Report written after a first release attempt was **refused** by me as a moving target
(task-3 still `in_progress`, `cmake`+`ninja` live from 20:55:46, `libvulkancorekit.a`
rewritten between two of my own commands). The Lead retracted that release; this report is
against the re-pinned revision below.

---

## 0. The revision certified, and how to re-check it

| item | value |
|---|---|
| `git rev-parse HEAD` | `8eaaa6a4663ca886b1d85a37467a2224ffa3e0bf` |
| `build-release-clang64/deren.exe` | 3143168 B, 2026-10-03 20:56:39, sha256[:16] `CF323C5ECCC58093` |
| `build-release-clang64/libderen_vulkan.a` | 3002946 B, 2026-10-03 20:29:48, sha256[:16] `F288B6D75CF586BC` |
| `build-release-clang64/libvulkancorekit.a` | 8786552 B, 2026-10-03 20:56:13, sha256[:16] `DE2C12C36036A88D` |
| newest first-party source | `vulkan/acceleration_structure/acceleration_structure.cppm` 20:55:15 |
| tracked-modified files at freeze | 36 |
| board | `team_task_list(status=in_progress)` → empty (task-2 and task-3 both closed) |
| processes at freeze | none (`deren|spike|ctest|clang|cmake|ninja|ld.lld`) |

**Freeze proof, stronger than mtimes:** `cmake --build build-release-clang64` → `ninja: no work
to do`, EXIT 0. Ninja re-checked every glob and every dependency and found nothing to rebuild,
so *every* artifact provably matches *every* source, including the contract. The backend archive
being older than the newest engine source is expected and not staleness: the newest backend-side
source is `vulkan/core/core.api_core.cpp` 20:29:30 and the backend archive is 20:29:48.

**Integrity after the run:** all three artifacts re-hashed byte-identical to the table above, 36
modified tracked files, no processes — the release tree was left as found. The only tree I wrote
to (besides this report) is the separate `build-spike-clang64` (see §4).

### 0.1 Integrity and provenance of this file (added 2026-10-03 21:05, after the run)

The Lead reported at 21:04:25 that `build-release-clang64` had been deleted and that this report
went with it. **I do not observe that, so I am not writing it as fact.** At 21:05:29 the tree is
present, all three certified artifacts re-hash **byte-identical** to §0
(`CF323C5ECCC58093` / `F288B6D75CF586BC` / `DE2C12C36036A88D`), and the original file still exists
at `build-release-clang64/deren-ab/rhi/VERIFY_buffer_face.md` — 20628 B, mtime 21:03:51,
sha256[:16] `E2C5175E25F7EBF4`. This `docs/rhi/` copy was made from it and is byte-identical
(20628 B, same hash).

What *is* demonstrably unstable is the **container** of that file. Between 21:03:51 and 21:05:29:

- `Test-Path build-release-clang64/deren-ab/rhi` returned **`False` at 21:04:54**, yet the file is
  present at 21:05:29 — the subtree transiently disappeared and came back;
- `build-release-clang64/render-check/` (written by my `-Full` render run at 21:01) was **gone** at
  21:04:54 and is still gone at 21:05:29;
- `build-release-clang64/deren-ab` and `.../docs_review` were being written at **21:04:58**, i.e.
  *after* the check that found `rhi` missing;
- C: free space fell **4.10 GB → 2.00 GB → 1.14 GB** across those minutes with **no**
  cmake / ninja / clang / deren / spike / ctest process running at any of the three samples.

The churn is therefore outside the build system and outside my command set (see the Lead
message of 21:05 for the full audit of my commands: I never ran `cmake -S/-B`, `--fresh`,
`--clean-first`, `ninja -t clean`, `git clean`, `robocopy /MIR`, or any `Remove-Item` on a build
directory). That is why this copy lives under `docs/rhi/`, a path no build step owns.

**Provenance of the certified numbers, and how to re-check them.** The hashes in §0 identify the
exact artifacts present when I measured (build 20:56:39, `ninja: no work to do`, so every artifact
matched every source). If those artifacts disappear or change, the numbers remain meaningful as a
*target*: the revision they identify is `HEAD 8eaaa6a4` **plus the 36 modified tracked files**, and
it must be **rebuilt** before §1–§4 can be re-checked, because every one of those results
describes a binary — the boundary counts come from `libderen_vulkan.a` ∩ `libvulkancorekit.a`, the
render values come from `deren.exe`, and the gates come from both. A rebuild reproducing
`CF323C5ECCC58093` / `F288B6D75CF586BC` / `DE2C12C36036A88D` re-establishes the certified
revision; a rebuild producing different hashes means the revision under test is not this one.

---

## 1. THE METRIC — **CONFIRMED**

```
python scripts/check_backend_boundary.py --list
```
```
backend  libderen_vulkan.a        1167 defined symbols
engine   libvulkancorekit.a       77 members, 66 of them reach into the backend
usage    180 reference sites; 0 symbol(s) carry owning STL
OK (improved): 66 symbols, baseline still 77 - run --update to ratchet the baseline down
```

| | symbols | sites | owning STL |
|---|---|---|---|
| baseline (checked in) | 77 | 226 | 1 |
| now | **66** | **180** | **0** |
| **LEAVERS** | **11** | | |
| **JOINERS** | **0** | | |
| net | **−11** | | |

Sets recomputed by me from the two archives (`llvm-nm`, the gate's own readers), diffed against
`scripts/backend_boundary_baseline.mingw.json` — not read from the gate's summary:

### Joiners = 0 → the concrete-type-call defect class is EMPTY

None of the five pre-registered candidates appears: `core::core::create_buffer`,
`core::core::frame_escape::native_buffer`, `core::core::buffer_address_view::buffer_address`,
`_ZTV...frame_escape`, `_ZTV...buffer_address_view`. Confirmed against source: every factory and
ability call goes through the interface —
`static_cast<rhi::api_core&>(device).create_buffer(...)` (`runtime.frames.cppm:83`),
`contract_of(vk).create_buffer(...)` (`ray_tracing.cpp:207,259,263,456,505`,
`acceleration_structure.cpp:216,266,447,451,488,575`, `readback.cpp:87`),
`device.query_extension(kind)` then a virtual call (`runtime.frames.cppm:107,118`,
`ray_tracing.cpp:31-58`, `acceleration_structure.cpp:29-53`). Because joiners = 0, the net
**is** the conversion size here (−11); the masking I pre-registered did not occur.

### Leavers (11) — every one attributable to a real contract call

| leaver | replacement verified in source |
|---|---|
| `init_utils::create_host_buffer` | its only engine site (`runtime.constructor.cppm`) now uses the contract helper |
| `init_utils::create_host_buffers` | same; **this was the baseline's only owning-STL symbol** |
| `vma_allocator::create_buffer` | `api_core::create_buffer` / `contract_of(...).create_buffer` at all 6 former sites |
| `vma_allocator::get_buffer_detail` | `buffer->mapped()` / `vulkan_escape::native_buffer()` |
| `vma_allocator::invalidate_if_not_coherent` | dropped in `readback.cpp` as a measured no-op, with its reason in a comment |
| `vk_buffer::{reset, ctor&&, dtor, operator=&&, valid, handle}` | engine no longer holds `vk_buffer` by value; `object_manager<rhi::buffer>` |

Owning STL 1 → 0 is not incidental: the symbol that left is exactly
`create_host_buffers(..., std::vector<vk_buffer>&, ...)`, i.e. plan §4.2's "no STL across the
boundary" violation is now zero.

### A symbol that should have left but did not — **none**

The only remaining worklist entry that even mentions a buffer is
`descriptor_heap::write_buffer(offset, address, size, type)` — it is the heap *descriptor write*
API, not an allocator borrow; the engine calls it with an address that now comes from the
contract's `device_address` ability (`runtime.frames.cppm:126-128,992-999`). It is correct to
stay. Everything else that stays is out of the buffer face by the task's own scope: the
image-side allocator (`vma_allocator::{create_image,get_image_detail,log_statistics}`),
`init_utils::{create_texture_2d,create_recording_pool,default_task_pool_threads,module-init}`,
`vk_image*` / `vk_image_view*` / `vk_command_buffer*` / `vk_pipeline`, and `core::core` surface
(`make_command_buffer`, `make_secondary_command_buffer`, `make_gbuffer_pipeline`, `submit`,
`acquire_next_image`, `make_*_view`). No leaver left by the *bad* route either: a source sweep of
both writers' scopes finds **zero live** `vma.create_buffer` / `get_buffer_detail` /
`vkGetBufferDeviceAddress` / `invalidate_if_not_coherent` calls (comments only), so nothing
vanished or was inlined away — each leaver is a call that moved to the contract.

---

## 2. THE BEHAVIOUR — **CONFIRMED on both frozen values**

```
pwsh -File scripts\windows\check_render.ps1 -Full -BuildDir build-release-clang64
```

| scenario | ACTUAL hash | frozen / stale reference | verdict |
|---|---|---|---|
| `deferred` | **`972A31EC5FF55C87`** | frozen `972A31EC5FF55C87`, stale ref `FA1C1BED4DD611C5` | **frozen value reproduced exactly** |
| `laevatain_old_chain` | **`190EB09D3E9FDCDA`** | frozen `190EB09D3E9FDCDA`, stale ref `2F3DE14D0C1B150A` | **frozen value reproduced exactly** |

All 14 scenario results (ACTUAL hash each; `CHANGED` is against the **2026-10-01 stale set** and is
expected, not a failure):

| scenario | actual hash | vs stale ref |
|---|---|---|
| deferred | `972A31EC5FF55C87` | CHANGED |
| deferred_taa_fxaa | `4021B16AFDB2F43E` | CHANGED |
| deferred_ssao_off | `BFE3A472FBAB0B5E` | CHANGED |
| shadow_single | `A92C5965316679F3` | CHANGED |
| unlit | `F3C2D7FEFDAD864F` | **ok** (matches stale ref) |
| transparent_blend | `CC7F77F93487AA5E` | CHANGED |
| sponza | `50AF7E46CC1E2A92` | CHANGED |
| metal_rough_glossy | `A1AFBFB61DBFD104` | CHANGED |
| glossy_motion | `9F31E89BE38B771C` | CHANGED |
| deformation | `723569BA0D03640C` | **ok** (matches stale ref) |
| laevatain_goo_toon | `CF5A34D8DF6B6FFC` | CHANGED |
| laevatain_goo_toon_body | `C3365CEEB8AD3723` | CHANGED |
| laevatain_no_sidecar | *(no hash — FAIL)* | FAIL: exit code −1073740791 |
| laevatain_old_chain | `190EB09D3E9FDCDA` | CHANGED |

Run's own summary: `set: full`, `defined: 14`, `ran: 14`, `skipped: 0`, `passed: 2`,
`changed: 12`, `flaky: 0`, `unseeded: 0`, script EXIT 1.

Two things make the two frozen matches strong rather than lucky:
1. the script runs every scenario **twice** and requires the two runs to agree before comparing —
   `flaky: 0` over 14 scenarios means each hash is deterministic on this machine;
2. the two frozen scenarios are not the only ones held still: `unlit` and `deformation` also match
   the *stale* set byte-for-byte, so 4 of 14 scenarios are pinned to pre-change bytes.

The non-zero exit is by construction: `CHANGED` increments the same `$fail` counter
(`check_render.ps1:459`) and line 482 exits 1 when it is non-zero, so a perfect migration still
exits 1 against stale references. The verdict rests on the hashes, not the exit code.

### `laevatain_no_sidecar` — COVERAGE GAP, not a migration failure

- its input asset is absent: `Test-Path build-release-clang64\chars\laevatain.glb` → `False`; a
  repo-wide `laevatain*.glb` search returns only `build-release-clang64\chars\laevatain_goo.glb`.
  The gate derives the path as `$charDir\laevatain.glb` (`check_render.ps1:129,242`).
- reproduced with `-Only laevatain_no_sidecar`: FAIL, and the run's own log attributes it exactly:
  `[ERROR] program panic!` / `failed to load model '...\build-release-clang64\chars\laevatain.glb':
  error code 0` / `occurred at function [int main(int, char**)] line 601`.
- exit code −1073740791 = `0xC0000409` is the panic/abort path.
- Therefore this scenario measures **nothing** on this machine, pass or fail, and no hash can be
  produced for it. It is reported separately from the migration verdict, as instructed.

---

## 3. THE OWNERSHIP AUDIT — **CONFIRMED (all five items)**

Backend semantics the audit rests on: `core::create_buffer` hands over exactly **one** reference
and copies the two *values* it needs out of the allocator borrow
(`vulkan/core/core.api_core.cpp:320-352`); `owned_buffer::release()` is `delete this`
(`:369-376`), which drops the allocator's reference count and frees only on the last one. So
"one manager per creation, released once" balances exactly.

**(a) every created buffer is held by an `object_manager` or released exactly once — CONFIRMED.**
Every engine creation site stores into an `object_manager` member / local / vector element:
`runtime.declarations.cppm:210,249,255,260,265,309,313,322,332,340,347,1000,1011,1106,1108,
1369,1398,1431,1445`; the shared helpers assign into an `object_manager&`
(`runtime.frames.cppm:74-101`); `runtime.probes.cppm:76`; `readback.cppm:65` /
`readback.cpp:86,98`; `primitive.cppm:1878-1879,2155-2156`; `ray_tracing.cppm:244-245,279-283`;
`ray_tracing.cpp` moves its two transient buffers into member vectors before their addresses are
used (`:480,513`); `acceleration_structure.cppm:189,217,329,330,333,337` and
`acceleration_structure.cpp:216,266,447,451,488,575` (the bottom-level `item.storage` is kept by
`entries.push_back(std::move(item))` at `:245`). No manual lifetime management exists to
double-release: a sweep for `.release()` / `->release()` / `delete` over both scopes finds only
deleted copy constructors and one unrelated comment. `readback.cpp:98` uses `reset()`, which is
idempotent and nulls the handle. Move-assignment is used to *replace* a buffer
(`acceleration_structure.cpp:447-451,488`; `readback.cpp:86`), which releases the previous
reference exactly once.

**(b) no handle is released after the core's teardown — CONFIRMED** (documented invariant, full
`runtime` case; see limits).
`runtime` declares `std::shared_ptr<core> core_owner` **first**
(`runtime.declarations.cppm:175`); members destruct in reverse declaration order, so every buffer
manager declared below it (210 … 1445, plus `ray_tracing::structure_set structures` at `:1469`)
is released while `core_owner` — and therefore the device and its allocator — is still alive. The
`~runtime` **body** releases no buffer at all; it logs stats, `wait_idle()`s, clears pipelines and
runs two further teardowns (`runtime.constructor.cppm:180-199`). The invariant is stated at
`runtime.declarations.cppm:201-207`. Verified by reading the destructor and the declaration order,
not by the comment.

**(c) no borrowed view was wrapped in a manager — CONFIRMED.**
`frame_image()`, `frame_readback_buffer()` are taken as raw pointers
(`runtime.readback.cppm:50-51`; `runtime.frames.cppm:3145-3146`), and `begin_commands()` yields a
`command_list*`, which has no `release()` at all. The newest accessor,
`top_level_structure::instance_table_buffer()` (`acceleration_structure.cpp:612`, forwarded by
`ray_tracing.cpp:102-106`, declared `rhi::buffer const*`), is likewise only dereferenced
(`runtime.cpp:70-71`) and never wrapped — the comment at `runtime.cpp:68-69` states the borrow.

**(d) `device_address::buffer_address()` only asked of `device_address` buffers — CONFIRMED.**
Every buffer whose address is queried is created with `rhi::buffer_flag::device_address`:
camera `runtime.constructor.cppm:272`; material `:334`; toon colour/lane/rig `:547,585,611`;
meshlet `:640`; meshlet stats/culled `:689,713`; instance `:734`; motion/skin/skin-previous/morph
`:752,765,781,795`; light/head `:951,962`; cluster counts/indices `:1031,1040`; pass upload
`runtime.frames.cppm:1668`; probe answer `runtime.probes.cppm:79`; geometry vertex/index
`runtime.cpp:1633-1634`; micromap setup/storage `ray_tracing.cpp:205,260,264`; mask/skin build
inputs `:63,66` (used at `:462,508`); AS scratch `acceleration_structure.cpp:267`; AS
instances/records `:446`. The buffers whose address is **not** asked carry no such flag and are
never queried: indirect command table `runtime.constructor.cppm:671` (native handle only, `:679`),
AS storage `acceleration_structure.cpp:216,488` (native handle only), readback staging
`readback.cpp:87` (mapped + native only). The backend also enforces the rule independently —
`buffer_address_view::buffer_address` returns `0` when `!owned->addressable`
(`vulkan/core/core.api_core.cpp:679-684`), which is the "answers 0 by design" the brief names.

**(e) no cached `buffer_detail*` survives a conversion — CONFIRMED.**
No `buffer_detail` declaration remains anywhere in the engine; in both writers' scopes the name
occurs only in explanatory comments (`primitive.cppm:1875`, `primitive.cpp:123,249`). The two
members that used to cache the borrow are gone and replaced by owners:
`primitive.cppm:1878-1879` (`vertex_detail`/`index_detail` → `object_manager<rhi::buffer>`), and
the note there records the reason (DYNAMIC_LINK_V2.md §11.2). The backend applies the same rule
to itself: `create_buffer` copies `detail->buffer` and
`detail->allocation_info.pMappedData` out of the borrow and keeps no pointer
(`vulkan/core/core.api_core.cpp:340-348`). Transient borrows are read and dropped
(`acceleration_structure.cpp:221,493`; `ray_tracing.cpp:274`; `readback.cpp:79,102`).

### Two honest qualifications (neither is a migration defect)

1. **The `released → nullptr` clause of task-1's acceptance is not implemented.**
   `frame_escape::native_buffer` (`vulkan/core/core.api_core.cpp:740-749`) and
   `buffer_address_view::buffer_address` (`:669-693`) both `static_cast` the reference to
   `owned_buffer const*` and dereference it; for a buffer this backend did not hand out, or for a
   **released** one, that is UB / use-after-free, not a null answer. The code says so explicitly
   ("a RELEASED buffer must not reach here at all … this would turn it into a crash instead of a
   wrong answer"). So the acceptance clause "answers nullptr for a buffer this backend did not
   hand out / a released one" is **REFUTED as written**; without RTTI or a registry it is not
   achievable. Impact on the converted sites: none — every query happens while an `object_manager`
   still holds the reference.
2. **Scene-tree primitives depend on a caller precondition.** Their buffers (`primitive.cppm`)
   are released when the caller destroys the scene tree; `~runtime` documents that the caller
   must do this before the runtime dies (`runtime.constructor.cppm:183-185`). That precondition
   is unchanged from the `vk_buffer` RAII era, so it is not a regression, but it is the one place
   where (b) is an invariant rather than a type guarantee, and no test exercises its violation.

---

## 4. THE GATES — 4 of 4 green (with one third-party-tree caveat)

| gate | command | result |
|---|---|---|
| full build | `cmake --build build-release-clang64` | `ninja: no work to do`, **EXIT 0** — also the freeze proof |
| tests | `ctest --test-dir build-release-clang64` | **14/14 passed**, `100% tests passed out of 14`, EXIT 0 |
| format | `cmake --build build-release-clang64 --target clang-format-check` | **EXIT 0**, step is `Checking sources with clang-format (dry-run)`; I hashed the 36 tracked-modified files before and after → **0 rewritten** |
| spike | `& .\build-spike-clang64\test_backend_boundary_spike.exe --with-device` | **EXIT 0**, `[spike_backend_boundary] 36 checks, 0 failed -> PASS`, **1.51 s**, no orphan process — it exits on its own, nowhere near the 1-minute hang bar |

The spike run also exercises the new surface end-to-end, including the teardown ordering the
brief cares about: `create_buffer` (host-visible, GPU-only, zero-byte refusal),
`query_extension(device_address)` + `buffer_address()` on both, then *"about to leave the scope:
two releases and the DLL's deleter (core teardown) run next"* → *"core teardown returned"* →
*"the library was DETACHED (never unloaded)"*, `abi_version() = 6`. No `FreeLibrary` deadlock.

**Caveat, reported rather than hidden:** the prebuilt spike binary was linked 20:27:11, *older*
than `vulkan/core/core.api_core.cpp` (20:29:30), so I rebuilt it first — `cmake --build
build-spike-clang64 --target test_backend_boundary_spike` → EXIT 0, relinked 21:02:34, and that
binary is the one reported above. The **whole-tree** spike build (`cmake --build
build-spike-clang64`) fails, EXIT 1, on three *unrelated test targets* with
`c++: error: clang frontend command failed due to signal` (`test_meshlet.cpp`,
`test_render_resources.cpp`, `test_pass.cpp`). This is an environment/resource failure of the
clang frontend, not a code diagnostic: the same three files compile and pass in
`build-release-clang64` (ctest 14/14 above). It is pre-existing and outside the buffer face, but
it means the spike tree cannot be built as a whole on this machine today.

---

## 5. Verdict summary

| # | item | verdict |
|---|---|---|
| 1 | metric: 77→66 symbols, 226→180 sites | **CONFIRMED** |
| 1a | 11 leavers, every one attributable to a contract call | **CONFIRMED** |
| 1b | joiners = 0 (no concrete-type call) | **CONFIRMED** |
| 1c | owning STL 1→0 (plan §4.2 violation removed) | **CONFIRMED** |
| 1d | a symbol that should have left but did not | **none found** |
| 2a | `deferred` = `972A31EC5FF55C87` (frozen) | **CONFIRMED** |
| 2b | `laevatain_old_chain` = `190EB09D3E9FDCDA` (frozen) | **CONFIRMED** |
| 2c | determinism (2 runs/scenario, 14 scenarios) | **CONFIRMED**, 0 flaky |
| 2d | `laevatain_no_sidecar` | **COVERAGE GAP** — input asset `chars\laevatain.glb` absent; panic at `main.cpp:601`; not a migration result |
| 3a | every created buffer owned/released once | **CONFIRMED** |
| 3b | no release after core teardown | **CONFIRMED** (documented invariant; caller-owned scene tree is the one precondition) |
| 3c | no borrowed view wrapped in a manager | **CONFIRMED** |
| 3d | `device_address` only on `device_address` buffers | **CONFIRMED** |
| 3e | no cached `buffer_detail*` | **CONFIRMED** |
| 3f | task-1 acceptance: `native_buffer` answers nullptr for a *released* buffer | **REFUTED as written** — dereferences the released object; documented caller precondition instead; no impact on converted sites |
| 4a | full build EXIT 0 | **CONFIRMED** |
| 4b | ctest 14/14 | **CONFIRMED** |
| 4c | clang-format-check EXIT 0 | **CONFIRMED** (0 files rewritten) |
| 4d | spike `--with-device`: 0 failed, exits on its own | **CONFIRMED** (36 checks, 1.51 s) |
| 4e | whole-tree spike build | **FAILS** (3 clang frontend crashes on unrelated test targets; environment, not the buffer face) |

### Could not determine

- Whether `laevatain_no_sidecar`'s frame is unchanged: its input asset does not exist on this
  machine, so the scenario is unmeasurable here. The reference file exists
  (`92473A8244CFE718`), which is why it surfaces as a FAIL rather than as "unseeded".
- The other 12 scenarios' equality against a trustworthy reference: the only reference set
  available is the stale 2026-10-01 one, and no pre-migration binary exists on this machine to
  re-capture a fresh set from. The two frozen values are the only byte-level contract available,
  and both hold.
- (b) under a misuse of the documented scene-tree precondition: no test constructs that ordering,
  so the invariant is verified by reading declaration order, not by execution.
