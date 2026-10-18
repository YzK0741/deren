# Complete production pass resource ownership

User requested a persistent goal: migrate and test incrementally until all production passes and their
shared GPU dependencies are managed. Continue the approved passchain lifetime-group design.
Keep uncommitted camera/deformation work; do not push automatically. Stop and checkpoint at3% remaining quota.

- [x] Camera and deformation shared inputs; build/CTest/native/pixel evidence recorded.
- [x] Shared scene tables: material/instance/toon/meshlet tables plus packed indirect/counter/cull storage.
- [x] Gaussian preparation/sorting/raster storage, including per-slot scratch and scene assets.
- [x] RT structure/build/mask/skin resources, with specialized owning types and completion retention.
- [x] Shared textures, IBL/furnace/LUTs and samplers; preserve uploads and initialization.
- [x] Final inventory audit and whole-goal verification: no remaining independent production pass allocation outside the declared/managed ownership model. Imported presentation/asset resources need explicit retained ownership rather than duplicate allocation.

Each batch: RED behavioral regression, ownership implementation, Release/CTest/native checks,
relevant frozen-baseline captures/resize, review and durable report. Preserve heap/layout/ABI semantics;
specialized descriptors can be added without turning lifetime labels into an automatic render graph.

## Scene-table batch

Prepare nine device-wide allocations before init_scene_resources initializes defaults/registers material0.
Preserve global allocation layout for indirect/stats/culled tables with internal frame slices. Append
engine ids for missing table types; correct material/instance schema scope to their actual single binding.
Runtime borrows handles/mappings and copies old mapped contents only after idle on generation replacement.
Validate all table descriptions before heap adoption/acquisition. Preserve toon neutral setup code verbatim.

Ruling: keep existing packed frame regions instead of splitting allocations into F instances — shaders and
indirect offsets already encode those regions; re-layout is unnecessary risk. Copy complete contents on
provider handover to preserve unmirrored imports/counters; ordinary reuse performs no copy/reset.

Baseline work package: build-release-dyn-clang64/scene-tables-check/runtime.before (includes camera/deformation and debug HDR fix).

Scene tables complete: RED4/27→GREEN178/178, schema608, CTest24/24 in20.03s, native1164,
15 fixed scene captures pixel-identical and sync-clean, resize2/2 clean. Independent review found
only obsolete scope documentation, now corrected. Evidence report2026-10-14-scene-tables.md.

## Gaussian GPU storage batch

Ten buffer requests are prepared by a resource-only subchain owned by each completed GPU frame slot.
This keeps dynamic counts/sorter requirements at their existing preparation point and preserves
the parent pass's immutable frame snapshot. Main runtime supplies its owning device lease; legacy
reference-only factory callers retain their existing obligation to keep the core alive.

Ruling: prepare the subchain only when a buffer is missing, undersized or its usage/flags change —
avoid registry map/set construction on normal frame reuse. Stage the pending resource snapshot,
validate sizes/addresses/mappings, then adopt borrowed handles after parameter uploads. Keep the
previous public frame snapshot on failure. The subchain drains before releasing sorter/asset/device
leases. CPU raster buffers and immutable point/SH uploads remain the next Gaussian sub-batches.

Complete: ownership RED1/35 → GREEN42/42; reviewed admission/lease regressions RED2/48 → GREEN48/48.
Registry admission now precedes commit/bind and accounts actual capacities. Reject legacy allocated-frame
lease promotion explicitly; create a fresh owning frame. Independent follow-up review accepts both fixes.
Release successful, CTest24/24 in32.87s, native1164. Static/rotation CPU+GPU before/after RGBA and HDR
identical; GPU maximize/restore2/2 clean,1280×720. Report2026-10-14-gaussian-managed-gpu.md.

CPU raster upload batch complete: two coherent buffers use the resource-only subchain, admission precedes
commit and publication, failed growth preserves live CPU/GPU results. Retained staging reference/owner
workspaces avoid steady-frame full-cloud allocation churn; preserve nonowning returned reuse view.
RED1/161 → GREEN192/192; GPU48/48; Release/CTest24/24 in27.77s; native1164; four static/rotation
CPU/GPU before/after RGBA/HDR identical; CPU maximize/restore2/2 clean. Reviewed workspace correction accepted.
Report2026-10-14-gaussian-managed-cpu.md.

Next: immutable Gaussian geometry/SH asset uploads. Preserve uniform_gpu_only staging-upload semantics;
add managed initialization descriptors with safely owned initial data, retain an owning device root in
production asset construction, and validate candidate addresses before publication. Do not mark the whole
Gaussian checkbox complete until these scene assets are managed as well.

Static-upload descriptor implementation in progress: request owns initial_data (an aliasing
shared_ptr<const byte> and byte count) plus an independent stable initialization_key. Reconcile
passes a temporary initial_bytes span only to synchronous create_buffer, and stored generation
metadata drops initial_data immediately. The producer releases its source after successful upload,
while retaining the content key/count in its declaration. Reuse matches the key; recreation with
no source refuses rather than silently making uninitialized storage. Initial data and zero/reset
policies cannot be combined. Duplicate descriptions with the same key can contribute the source.
Descriptor regression RED1/49 → GREEN65/65, including source release, same-key reuse, missing-source
recreation refusal, changed-key replacement, duplicate producer merge, reset conflict and byte-count bounds.
Release build passes; CTest24/24 in25.08s before six additional boundary assertions, those assertions
then pass in the focused65/65 run. CPU192/192 and schema609/609 remain green. Independent read-only
review found no protocol issue. Production static-asset integration has not yet been changed.

Asset integration plan: add a shared packed-asset upload overload so production can alias packed
geometry/SH vectors without a byte-array copy. Legacy const-reference upload wraps a copied shared
packed asset. Raster factory moves its packed result into shared storage, uploads synchronously,
then keeps its existing CPU geometry vector; the provider drops upload aliases before that move.
Asset subchain requests scene lifetime, uniform_gpu_only + storage/address flags. Append geometry/SH
ids, expose a retained snapshot, check candidate sizes/addresses via admission, and provide a
runtime create_gaussian_asset helper that supplies core_owner. Keep sorter as frame-owned pipeline
service; imported geometry assets elsewhere will need explicit retained ownership in final inventory.

Static assets complete: geometry/SH scene resources use the subchain and managed initial-data aliases;
producer/generation release aliases after upload. Production moves packed data through shared storage
and retains no extra packed copy; legacy const-ref convenience path validates then copies. Runtime helper
supplies core_owner. RED1/394 → GREEN404; initial-data65; CPU192; schema609; Release/CTest24/24 in25.87s;
native1164; four static/rotation before/after RGBA/HDR identical, GPU maximize/restore2/2 clean.
Independent review found no new issue. Report2026-10-14-gaussian-managed-assets.md.

Next RT audit anchors: ray_tracing owns mask_buffers/skin_buffers and micromap handles; acceleration_structure
owns BLAS entries and per-slot TLAS structures/instance records. Their capacities/descriptors vary by caster,
so the registry needs typed dynamic family elements or specialized requests, rather than pretending a
single descriptor fits every object. Backend internal AS storage/scratch remains part of each RHI object's
own implementation; move engine ownership/creation descriptors and retain completion/device roots.

TLAS batch complete: each slot's top-level structure and shader instance records share a resource-only
subchain, frame_slot life and borrowed handles. Admission validates/copies the current used record prefix
before committing. Fixes two old growth bugs: refusal dropped old records, and successful growth lost
the already-written prefix. Registry has typed AS objects, currently top-only descriptors (no geometry pointers).
Production RT phase carries core_owner and forwards it; phase teardown waits before freeing micromaps.
Append rt_instance_table id; RHI ABI stays32（2026-10-18 核对：RHI ABI 现为 33，证据 source/promise/rhi/rhi.contract.cppm:300；本文两处“ABI32”都是当时的基线值）. Native AS probe stale31 check corrected, default portable
regression added to CTest. RED4/265 → GREEN279, native AS362 and runtime1164, Release/CTest25/25 in26.09s;
RT/ML-scaled/animated-refit captures pixel-identical and sync-clean; resize2/2 clean. Review no new issue.
Report2026-10-14-tlas-resources.md.

Next: BLAS creation descriptors with owned geometry and dependency leases, then mask/skin outputs and
micromaps. TLAS schema's global scope and absent typed AS frame-table publication are still inventory
gaps to resolve in the RT audit. Do not check the RT checkbox until all these cases are covered.

BLAS creation batch complete: nonempty entries own scene resource subchains; request owns geometry
vectors and optional explicit dependency leases. Runtime forwards device root. Preserve allow_update,
micromap pointer, indexed/unindexed and skipped-index behavior. Entry moves transfer ownership without
drains; normal refit does not reconcile. Per-entry teardown drain cost remains outside frame work and
has not been benchmarked. Mask/skin/mm managed dependency leases will be wired when those types migrate.
RED1/280 → GREEN310, native AS393/runtime1164, Release/CTest25/25 in26.09s; three RT captures pixel-identical
and sync-clean; resize2/2 clean. Review no new issue. Report2026-10-14-blas-resources.md.

Next: mask/skin output buffers and micromaps through resource-only subchains, with their generation
leases passed to BLAS add. Inspect failure recording: mask bake is currently recorded during the caster
allocation loop; a later BLAS allocation refusal calls abandon and frees its destination before the
already-recorded command is submitted. Delay mask recording until all resource preparation succeeds
(skin/mm/build recording already occurs after the loop), then retain resources on later recording errors.
Add a phase-level failure regression before claiming that path fixed. This is an existing phase issue,
outside the verified BLAS-only tests, and must be resolved as part of the remaining RT migration.

Mask/skin output batch complete: scene GPU-only subchains, borrowed addresses, generation leases
retained by BLAS. Delay mask recording until all allocation succeeds. Keep resources and unready state
on partial AS recording failure; reject initial skin recording failure and propagate disable_skin_bake
to runtime. RED1/312, RED1/316, RED1/321 → final portable321/native Vulkan404. Final Release build and
CTest25/25 in25.84s pass. Three targeted RT/animation/ML captures are pixel-identical and sync-clean;
maximize/restore2/2 clean. Reviewed; report2026-10-14-bake-resources.md.

User requested stopping after this batch; pause here with changes uncommitted. Next batch starts with
typed micromap requests and upload/dependency ownership. Remaining RT audit: imported geometry retention,
TLAS schema scope and AS publication, plus per-slot readiness after empty/failed TLAS updates (suspected,
requires regression before claiming a defect). Shared textures/samplers and final inventory remain.

## Micromap batch

Resume authorized by user after the mask/skin checkpoint. Ruling: typed micromap declarations carry
scalar RHI metadata, a stable initialization key and shared immutable upload vectors. Raw RHI spans
are forbidden in stored requests; they are supplied only during synchronous creation. Generations and
providers release upload vectors after creation; same-key reuse needs no source, recreation requires it.
Scene subchains own micromaps and BLAS retains their generation leases. Backend storage/scratch remains
inside the RHI object. Preserve UNKNOWN four-state payload, build sequence and barriers.
Ownership regression RED1/356 confirmed before implementation. Frozen baseline:
build-release-dyn-clang64/micromap-managed-check/runtime.before (includes completed mask/skin batch).
RT smoke confirms14 micromaps/34940 triangles; animation and scaled ML baselines captured.

Complete: portable373/native Vulkan456, Release/CTest25/25 in26.58s. Three targeted captures have
zero pixel differences and clean sync logs; maximize/restore2/2 clean. Evidence and source-release
protocol tests recorded in2026-10-14-micromap-resources.md. Next: TLAS readiness/scope/publication audit,
then imported geometry retention. RT checkbox remains open until those are resolved.

## TLAS audit

Readiness regression RED2/380: empty/failed builds published stale structures. Add a per-slot ready bit,
clear it at begin/record_build, set only after successful recording. Retain allocation for reuse. Begin
at the start of phase update before skin/refit can fail. Readiness fix portable381/381 before adding
publication changes. Schema scope RED1/610 and typed-table publication RED1/200 confirmed separately.
Ruling: append a typed AS pointer to resolved_binding, publish each slot's current ready handle and
correct TLAS schema to per_frame_slot. Existing heap upload and feature gating remain unchanged.
Frozen package build-release-dyn-clang64/tlas-audit-check/runtime.before includes micromap migration.

Complete: also fixed ignored per-frame skin recording refusal (RED1/385). Final portable AS385,
actual Vulkan468, pass205, schema610. Release/CTest25/25 in33.90s; three RT captures pixel-identical
and sync-clean; resize2/2 clean. Record evidence2026-10-14-tlas-audit.md. Next: imported geometry
ownership/retention, then shared textures/samplers and final whole-plan review. Do not mark RT complete yet.

## Imported geometry retention

Geometry lease regression RED1/386. Ruling: keep geometry handle operations compatible through an
engine geometry_buffer view/lease wrapper. Legacy object_manager assignments transfer one owned RHI
reference into a shared holder; managed bindings retain generation leases. Production create_primitive
uses a scene resource-only subchain for vertex/index uploads and stores borrowed leased views. A scene
leaf drains its subchain on release; BLAS retains both source geometry leases. Backend setup remains
unchanged. Initial source spans are copied once into immutable temporary upload owners, released after
creation; this adds import-time CPU copying, outside frame work. No duplicate GPU allocation is requested.
Append engine geometry ids; legacy custom/static primitives remain imported owners with explicit leases
rather than forcing a duplicate upload. Frozen baseline geometry-managed-check/runtime.before includes
completed micromap and TLAS audit batches. Full scene captures are running before migration verification.

Implementation complete: portable392/native Vulkan475/schema610; Release and final CTest25/25 in31.86s.
Fifteen scene sweeps pixel-identical and resize2/2 sync-clean; pixel evidence is archived. Unused legacy static
batch class remains a caller-owned import with no production factory; CPU scene leaves are still
borrowed by RT phase and must not be concurrently removed. Concrete C++ geometry field type changes
while supporting legacy assignment/get/reset/dereference operations. Report2026-10-14-geometry-resources.md.
Next: shared samplers first, then texture/IBL/LUT image uploads and final whole-plan audit/review.

Geometry capture audit:14/15 initially identical; character baseline missed its executable-relative
FGD PNG and produced an invalid black capture. Preserve invalid logs/screenshots, copy identical FGD
fixture into frozen runtime.before, rerun character baseline. Now15/15 have zero changed pixels.
Archive names selected capture files and includes fixture hashes. Future full baselines must copy
this fixture before running character scenarios. This was a fixture discrepancy, not a shader regression.

## Shared images, samplers and pass uploads

User authorized final group continuation. Ruling: extend typed registry with sampler descriptions and
image initial-data ownership using the existing immutable source/key protocol. Shared images request
only whole views (layer_views=false); shadow/attachment defaults retain per-layer views. Runtime fields
use shared_ptr aliases whose control blocks own resource-only subchains; dropping last aliases drains
before releasing image/view/sampler objects and device. Initial source spans are copied into temporary
immutable upload owners, released after synchronous creation; no per-frame reconciliation. Preserve all
image formats/dimensions/mips/cube flags and sampler filters/compare parameters.
Image/sampler protocol RED2/394 before implementation, portable403 after initial integration. Shared
replacement regression27/27 checks first IBL publication, partial allocation refusal, repeated update
and LUT refusal. IBL now stages a complete triple/sampler, waits GPU completion, then replaces published
vectors instead of appending an inaccessible second triple. LUT replacement also waits before heap writes.
Full creation census found production RT SBT uses runtime's generic upload hook: migrated that hook to
managed buffer subchains as well. Remaining direct allocation sites are diagnostic/probe paths or unused
legacy helper definitions; presentation image/view ownership remains explicitly borrowed/backend-owned.
Append engine ids/kinds; RHI ABI32 unchanged. New frozen baseline shared-assets-check/runtime.before
includes previous geometry batch and the same executable-relative FGD fixture. Fifteen baseline captures
are sync-clean. Final sources now under fresh independent whole-plan read-only review.

Final review found two P1 paths: micromap build/barrier refusal still continued into dependent BLAS;
IBL replacement freed previous owners before fallible heap writes. Refusal regressions RED2/418 (AS)
and RED12/78 (shared replacement) confirmed. Fix pass: return unready with retained RT resources on
build/order refusal; retain old IBL until all heap writes succeed, rollback completed writes on refusal.
If rollback itself refuses, retain new images and refuse frame acquisition until a complete successful
IBL publication restores the heap. LUT publication now precedes owner replacement too. Tests inject
single refusal at each of the3 IBL slots, LUT write refusal, persistent rollback failure and recovery.
Normal rendering/upload contents remain unchanged. Partial fault retention is outside steady-frame work.

Complete: final portable AS418, actual Vulkan501, shared replacement88, schema610, Release/CTest25/25
in27.14s. Fifteen scene captures pixel-identical and sync-clean. CPU/GPU flowers before/after40-frame
sweeps have identical RGBA/HDR per path; resize2/2 clean. Independent whole-plan follow-up accepts both
P1 fixes and finds no new issue. Evidence2026-10-14-shared-assets.md.

Final allocation census: production storage creation routes through managed requests, including SBT.
Remaining calls are diagnostic/HDR-readback/probe allocations and unused legacy helper definitions.
Backend internal scratch/heap services, borrowed presentation images and explicit legacy imports retain
documented ownership. Full access/ordering graph and exhaustive bindless-element publication are future
work, outside the accepted ownership migration. No commit or push requested/performed. Whole goal achieved.
