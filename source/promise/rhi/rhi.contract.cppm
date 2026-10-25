// -*- C++ -*-
// ============================================================================
// module: deren.promise.rhi:contract
//
// The two pieces every other partition of deren.promise.rhi needs: the ABI number the
// backend and the engine compare, and the error enum that turns a failure into a
// return value instead of an exception (RHI plan v4, §4.1 item 3 and §4.2).
//
// `deren::promise::rhi::abi_version` is the compile-time constant whose twin is the
// backend's `deren_abi_version()`. Plan §8.1 row 4 makes the comparison a tested
// check rather than a convention: the loader resolves the symbol and the test
// asserts the returned number equals this constant. The value is part of the C
// ABI: it moves when the SHAPE of something that crosses the boundary changes in a way
// an older engine could not survive - a virtual on an existing interface (tier 1 or
// tier 2), a by-value structure's width, or an entry point's signature. Appending an
// enum value or a new interface does not move it; a virtual on an existing type does.
//
// The ABI number and the error codes here are plain values an ordinary C caller could read: no engine
// state, no exception, and no standard-library container travels across the boundary on their account.
// (The RHI ownership containers live in this partition too, but they are the contract's OWN types - both
// sides spell them from this one definition - and their only allocation is a control block.) Both sides
// of the boundary compile this partition (plan §4.1 item 1); neither exports a symbol for it.
// ============================================================================
module;

#include <atomic>   // the shared handle's reference counts
#include <concepts> // object_type: the containers are constrained to RHI handles
#include <cstddef>
#include <cstdint>
#include <memory> // std::shared_ptr: the aliasing owner an engine lease carries
#include <source_location>
#include <string_view>
#include <type_traits>
#include <utility>

export module deren.promise.rhi:contract;

// ONE PROJECT DEPENDENCY, AND ONLY ONE: `deren.utility.shared` is the process-wide half (the one
// panic sink and the log), which BOTH halves of the boundary already link; the validate-and-cast
// helpers below terminate through it, so the failure report cannot fork into a second routine.
import deren.utility.shared;

/**
 * @file source/promise/rhi/rhi.contract.cppm
 * @brief the ABI number the backend and the engine compare, and the `error` enum every promise entry
 *        point reports through.
 * @ingroup promise
 *
 * Both are part of the C ABI rather than of any implementation: `abi_version` is the number
 * `deren_abi_version()` returns and `deren_make_api_core()` refuses to build against, and `error`
 * is what a failure travels in because an exception must not cross the boundary (§4.2).
 * Deliberately the smallest partition in the contract: both sides compile it, so anything added here
 * is added to both compilations at once.
 */

export namespace deren::promise::rhi {

    /// The C ABI number of this build of the contract.
    ///
    /// `deren_abi_version()` in the backend returns it and `deren_make_api_core()`
    /// refuses any other value, which is the "engine and backend disagree" failure
    /// path the skeleton keeps testable on purpose (plan §4.2).
    ///
    /// 1 -> 2 in the recording-surface batch: SIX virtuals were added to EXISTING tier-1 types
    /// (`api_core::frame_image`/`frame_readback_buffer`, `image::format`, `buffer::mapped`,
    /// `command_buffer::use`/`copy_image_to_buffer`), which shifts the vtable every caller reaches
    /// through. Appending `error` values or adding a NEW interface does not move this number; a
    /// virtual on an existing type does. FROM S2 ON THIS IS STRICTER: the DLL ABI promise is live
    /// then, so a renumbering breaks binaries in the field rather than only recompiling this tree.
    ///
    /// 2 -> 3 in the one-creation-structure batch: `deren_make_api_core()` grew the parameter it was
    /// always described as taking - a `deren::promise::rhi::create_info const*` - and the contract's
    /// creation structure is now the ONLY one (the backend's `vulkan::core_create_info` twin and the
    /// `to_backend_create_info()` translation between them are gone). No vtable moved, but the C
    /// entry's SIGNATURE did: an engine built for 2 calls that symbol with two arguments and would
    /// have its third read from whatever the stack held, which is exactly the failure this number
    /// exists to catch before anything is allocated (the mismatch is refused with `error::abi_mismatch`).
    ///
    /// 3 -> 4 in the owned-handle batch: `release()` landed as a pure virtual on SEVEN existing tier-1
    /// types (`buffer`, `image`, `sampler`, `shader`, `pipeline`, `swapchain`, `query`), which shifts the
    /// vtable every caller reaches through - the same reason 1 -> 2 moved the number. It is an ADD to
    /// existing types, not a new interface, so it is exactly the case the rule above names.
    ///
    /// A RENAME IN PLACE DOES NOT MOVE IT, and that is worth spelling out where the number is defined:
    /// the entry was briefly called `destroy()`, was renamed to `release()` to say "this drops ONE
    /// reference; the resource may be shared/reused and therefore outlive the call", and the number
    /// stayed 4. The SLOT is unchanged (same position, same order), only the symbol name is, so a build
    /// compiled against one spelling and a build compiled against the other remain binary compatible -
    /// the rule is about the SHAPE of the vtables, not about their spelling.
    /// 4 -> 5 in the addressable-buffer batch: `acceleration_structure_address` MOVED from the
    /// `device_address` ability to `ray_tracing`, where the only ability that can produce its operand
    /// lives. Both tier-2 abilities change shape (one loses a virtual, the other gains one), which is
    /// exactly the case the rule above names: an engine built for 4 would dispatch those slots
    /// differently. The move is what lets `device_address` be announced at all - while it carried an
    /// operand no backend could produce yet, announcing it was forbidden by the ability's own rule.
    /// 5 -> 6 when `vulkan_escape` gained `native_buffer`: a virtual APPENDED to an existing tier-2
    /// ability. Appending keeps the slots already there where they were, but the vtable every caller
    /// reaches through is a different shape, which is the case this number exists for. It is the call
    /// that lets an escaping pass use a contract buffer's raw handle instead of the allocator's detail
    /// map, i.e. one more way for the engine to stop reaching into the backend.
    /// NOT a bump - recorded so the decision outlives the session: the extension.cppm revision
    /// (codex/dynamic-link-v3 @ ab14ca6) deleted the documented `deren_ext_<ability>_v1` C
    /// function-table requirement, leaving `query_extension()` as the only ability entrance. What
    /// was removed is a PROSE requirement (the gate walks abilities() bits against objects, not
    /// symbols), not a virtual, an entry point, or any type the ABI numbers exist to compare - no
    /// vtable shape moves, so the rule above gives no reason for a bump.
    /// 6 -> 7 in the image face (the decided design, landed):
    /// `image` GAINED `make_view()` and with it the owned `image_view` interface, `image_desc` /
    /// `sampler_desc` / `image_view_desc` became defined types, `image_format` grew the creation
    /// formats and the `depth` role, and `vulkan_escape` grew `native_image` / `native_image_view` /
    /// `native_sampler`. Every change APPENDS to an existing vtable or adds a new interface, which is
    /// the case the number exists for: an engine compiled for 6 dispatches those slots differently.
    /// The `depth` format value is pinned at 0x7FFFFFFF so later named formats can append without
    /// moving it.
    /// 7 -> 8 in the pipeline face: `shader_desc` / `pipeline_desc` became defined types (the blend
    /// vocabulary is the FOUR MODES the survey found behind the raw blend constants, the depth
    /// compare the two the renderer uses), the factories gain real implementations, and
    /// `vulkan_escape` grew `native_pipeline` / `native_shader_module`. Append-only; the number
    /// exists because the vtables the engine dispatches through changed shape.
    /// 8 -> 9: vulkan_escape appends heap_ready/heap_properties. The latter returns a
    /// value-only POD mirror, never the backend's heap_limits reference; the former
    /// reports actual heap readiness. Appending changes the vtable shape.
    /// 9 -> 10: vulkan_escape appends native image/buffer heap descriptor writes.
    /// Native image view data is a value-only POD; pNext is deliberately unsupported.
    /// 10 -> 11: vulkan_escape appends native heap bind/push and binding snapshots.
    /// Native primary/secondary command buffers are borrowed from this device's recording domain.
    /// 11 -> 12: all RHI objects carry a sealed interface identity; heap services move
    /// from Vulkan escape to descriptor_heap with tagged, size-checked request structures.
    /// NOT a bump - recorded so the decision outlives the session (the error-mechanism batch):
    /// `error` grew six appended values (13-18), and `error_zone` / `zone_of()` / `graphics_api` /
    /// `error_info` were added as NEW types and a constexpr free function. No virtual moved, no
    /// signature changed shape, and the factory's `error*` out-parameter is still an `error*` - an
    /// appended enum value is data an old engine simply receives as `operation_failed`-class news it
    /// has never seen, which the rule above gives no reason to number. The `error*` -> `error_info*`
    /// entry-signature change that DOES move the number arrives with the frame face (abi 12 -> 13),
    /// in one step with the new tier-1 types.
    /// 12 -> 13 in the frame face: `api_core` APPENDED `walk_frames()` / `profiler()` - a virtual on
    /// an existing tier-1 type shifts every vtable that reaches through it, which is exactly the case
    /// the rule names - and `frame_walker` / `gpu_profiler` / `frame_open_info` are new tier-1 types.
    /// The factory entry's out-parameter changed `error*` -> `error_info*` in the SAME bump (one
    /// renumbering for one batch): the C entry's SIGNATURE is what the number protects, and an engine
    /// built for 12 calling 13's symbol would read a 40-byte diagnostic where it wrote a 4-byte enum.
    /// 13 -> 14 in the frame verbs' completion (the boundary batch, one renumbering for one batch):
    /// `api_core` appended `submit(command_buffer&)` and `frame_swapchain()`, `present()` changed
    /// `void` -> `error` (a presentation that failed silently was the information loss the error
    /// mechanism exists to end), `command_buffer` appended the GPU timing recording verbs
    /// (`begin_gpu_timing()` / `mark_gpu_timing(index, name)`), and `swapchain` appended
    /// `recreate()` / `extent()` - every one an append or a return-type change on a tier-1 vtable,
    /// which is the case the number exists for. The swapchain verbs live on the SWAPCHAIN, not on
    /// the context, because a rebuild is the swapchain's own behaviour; the extent is the
    /// presentation's fact and the render scale stays the caller's own decision.
    /// 14 -> 15 in the recording-surface batch (the command buffer's ownership lands): `api_core`
    /// appended `create_command_buffer(command_buffer_desc const&)` - a virtual on an existing tier-1
    /// type, the case the rule names - and `command_buffer` is a NEW tier-1 interface (`release()` /
    /// `begin_recording()` / `end_recording()` / `recording()` / `execute()`) with its own
    /// `interface_type` value. The `next` chain of `command_buffer_begin_info` is data, not a vtable,
    /// so a later tagged structure (the Vulkan inheritance one this batch adds, and any other) does
    /// not renumber this: the same rule the appended `error` values follow.
    /// 15 -> 16 in the sampler-description batch (③-D/E item C): `sampler_desc` GAINED FIELDS
    /// (`mag_filter` / `min_filter` / `mipmap_mode` / `compare_enable` / `compare_op`), and a DESCRIPTOR
    /// that travels across the boundary is part of the ABI even though no vtable changed - it is passed
    /// by `const&` into `create_sampler()`, so a backend built against the old width would read a
    /// different structure than the caller passed. The mechanism that makes the bump honest is already
    /// the structure's first member: `struct_size`, which the backend checks and refuses on a mismatch
    /// (the `buffer_desc` / `image_desc` rule). The reason the WIDTH had to change at all: the engine now
    /// creates its own sampler set (six samplers whose NEAREST minification, NEAREST mipmapping and
    /// `less_or_equal` comparison have to survive the trip), so the two knobs the old description carried
    /// were no longer enough to keep the pictures identical.
    /// AND ONE APPENDED `image_format` VALUE, WHICH DOES NOT RENUMBER THIS (③-D/E A1.2): `r16_sfloat`
    /// (one half-float channel - the ray-traced visibility image the engine now creates itself) joins
    /// the enum with an explicit value placed after the `depth` role, so every enumerator that already
    /// existed keeps the number it had. That is the rule the appended `error` values follow (see the
    /// `image_format` note in rhi.api_core.cppm), and it is why this is not a bump: nothing already
    /// crossing the boundary changed shape, size or spelling. An older backend handed the new value
    /// refuses it (its format switch has no case) rather than misreading it.
    /// (The append's own convention - explicit HIGH-BIT value at the end of the list when the range ends
    /// in a role/sentinel enumerator, and no existing number ever moves - is written down beside
    /// `image_format` in rhi.api_core.cppm.)
    /// AND ITS FIRST FOLLOW-UP, SAME RULE (2026-10-17): `r32_uint` joins the enum at 0x80000001u for the
    /// visibility buffer the geometry path is gaining - one 32-bit unsigned channel holding a packed
    /// visibility id. No enumerator moved, nothing already crossing the boundary changed shape, and an
    /// older backend refuses the value (its image-format switch has no case), so this is not a
    /// renumbering either. The `abi_version` number does not move.
    /// 16 -> 17 in the contract-only runtime's merged slice (③-D/E step 2, S2+S3 - the SECOND thing the
    /// abi-17 number was held for, and the reason it is spelled here): `vulkan_escape` APPENDED
    /// `native_swapchain_image_format()` - a virtual on an existing tier-2 interface, which is the case
    /// the number exists for. WHY IT IS NEEDED, MEASURED: the engine builds the pipelines that render
    /// INTO the presentation image (the post chain, FXAA, the upscale resolve) and the debug overlay
    /// BEFORE the first frame is acquired (`main.cpp` calls `runtime.create_passes()` after the runtime
    /// is constructed and before the frame loop), and the surface's format is a session-stable fact
    /// those builders must be created with. The contract's only format query at the time,
    /// `vulkan_escape::native_image_format(image const&)` (REMOVED in abi 34 with the raw heap view path),
    /// needed an IMAGE - and the only swapchain image the engine can name is `api_core::frame_image()`,
    /// which the contract deliberately answers with nullptr until an acquire has happened. So the fact
    /// had no pre-acquire answer at all, and this is
    /// the one escape accessor the slice adds: the swapchain image's format as the backend resolved it,
    /// asked of the context rather than of an image. (The withdrawn entry at this number was
    /// `graphics_queue_family_index()`; that one was replaced by a read-only runtime derivation and is
    /// NOT coming back.)
    /// 17 -> 18 in the SHARED flip (the last batch of the dynamic-backend migration): the C entry
    /// surface goes from THREE symbols to ONE. `deren_make_api_core` RETURNS a
    /// `std::shared_ptr<api_core>` instead of a raw pointer (the `shared_object` spelling arrived with abi
    /// 44; the SHAPE - a C++ handle carrying the control block - is this number's), and `deren_destroy_api_core` plus
    /// `deren_abi_version()` are DELETED with it: ownership lives in the control block the backend
    /// built (so no host names a deleter and no raw pointer crosses), and the version became the call's
    /// FIRST ARGUMENT - a mismatch is still refused before any object exists, with the backend's own
    /// number carried back inside the `error_info` it fills. WHY THE OLD SHAPE COULD BE RETIRED RATHER
    /// THAN KEPT: the raw-pointer-plus-deleter spelling existed to keep "one C++ runtime" out of the
    /// boundary's premises (m02244). That premise is now MEASURED and machine-checked - every image
    /// imports libc++.dll and uses the UCRT heap, so one runtime and one heap are shared and a control
    /// block allocated on one side may be released on the other (scripts/check_backend_boundary.py's
    /// `cxx` check, which now covers the executable AND the backend DLL). The C ENTRY's signature is
    /// what the abi number protects, so this is a bump even though no vtable moved.
    /// `-Wreturn-type-c-linkage` is silenced at the declaration and at the definition, because the C
    /// linkage is there for the NAME - an undecorated symbol a host resolves as a string.
    /// 18 -> 19 in the CONSUMER-SIDE HANDSHAKE batch: `api_core` APPENDED `api_version() const noexcept`
    /// (rhi.api_core.cppm), the object's own attestation of the number both halves compile - asked by the
    /// engine in the runtime's constructor, where a mismatch is a PANIC (a different number means the two
    /// halves disagree about this vtable and about every structure the contract passes by value, so
    /// continuing is undefined behaviour rather than a degraded mode). An appended virtual on an existing
    /// tier-1 interface is exactly the case the number exists for, EVEN THOUGH IT PRODUCES NO BACKEND
    /// SYMBOL: the engine dispatches it through its own vtable copy, so the boundary's measured count and
    /// the import meter stay at ZERO - what changes is the interface SHAPE, and that is what this number
    /// protects. The same batch moves the loading and the acquisition of the device root out of the
    /// runtime into `deren.engine.backend_loader`, called by `main.cpp` (`runtime` now takes the
    /// `shared_ptr` and refuses an empty one), which is a construction-path change in the same breath.
    /// 19 -> 20 in the RECORDING FACE's verbs (step 1's verbs half -
    /// the descriptors half landed unbumped a batch earlier): `command_buffer` APPENDED the portable
    /// record series the engine's passes record with - begin_rendering/end_rendering,
    /// bind_pipeline/bind_vertex_buffer/bind_index_buffer, draw/draw_indexed, dispatch/
    /// draw_mesh_tasks/draw_mesh_tasks_indirect, the five dynamic-state verbs, barrier (group and
    /// single-image), copy_image/copy_buffer/clear_color_image - and `viewport` / `index_type` /
    /// `cull_mode` / `image_copy` are the small vocabulary those verbs take. Every verb is an APPEND
    /// to an existing tier-1 vtable (the series is declared once on the borrowed view, which the
    /// owning `command_buffer` reaches through `recording()`), which is the case the number exists
    /// for; the descriptors the verbs take had already landed as plain PODs, so this bump is the
    /// interface half and nothing else. Push was still a descriptor-heap operation in that series;
    /// ABI 52 subsequently moves it to command_recorder::push_constants and removes the heap entry.
    /// NOT BUMPED AGAIN BY THE OWNER'S CONVENIENCE ENTRY POINTS (added after this line, unchanged number):
    /// `command_buffer` gained NON-VIRTUAL inline forwarders for the series - `buffer->draw(...)`,
    /// `buffer->barrier(...)` and the rest - each delegating to the same `recording()` object the note
    /// above names. The series is still declared ONCE (on the borrowed view, where the pure virtuals and
    /// their implementations live); an owner-side entry point that is not virtual appends NO vtable slot,
    /// no dispatch crosses the boundary through it, and `command_buffer`'s shape and implementers are
    /// untouched. The number moves for an APPENDED tier-1 slot, which this is not.
    /// 20 -> 21 in the RECORDING FACE's owner half (the append the note above anticipated): `api_core`
    /// APPENDED the pure virtual `make_command_buffer(command_buffer_desc) ->
    /// std::shared_ptr<command_buffer>` (the `shared_object` spelling is abi 44's) - a NEW vtable slot on an
    /// existing tier-1 interface, which is
    /// exactly the case the number exists for - and `command_list` was DELETED rather than renumbered
    /// (its `interface_type` enumerator and its number stay retired in place, and the series it carried
    /// is declared on `command_buffer` itself). The same batch's PODs are additive and carry NO bump:
    /// `pipeline_desc::compute_code`, `barrier_group`'s stage hint and its global memory barrier, and the
    /// appended `buffer_use` VALUE the last of those needed - the first two are guarded by `struct_size`,
    /// and adding a value never renumbers anything (the rule stated above).
    /// 21 -> 22 in the BASIC-HANDLE batch: `vulkan_escape` APPENDED three slots - `get_basis()`,
    /// `device_proc(api_basis&, char const*)` and `shader_group_handles(api_basis&, pipeline const&, ...)` -
    /// which is a tier-2 interface growing, the same shape as 5 -> 6 and 7 -> 8 above. The new `api_basis` type
    /// is an EMPTY, VIRTUAL-FREE tag struct (`s_type` only) that no boundary crosses by value and no interface
    /// gets a slot for, so the type alone moves nothing; the three appends are the whole reason for the number.
    /// (`device_proc` WAS REMOVED AGAIN IN ABI 24 - see below - so that escape now carries two of the three.)
    /// 22 -> 23 in the SHADER-BINDING-TABLE batch: the `ray_tracing` ability's `trace_rays` slot CHANGED SHAPE
    /// rather than being appended to - it takes four `shader_binding_table_region` parameters now, because the
    /// shape it had (`commands, width, height, depth`) could not describe a launch at all and had no implementer
    /// and no caller. That is the case the number exists for: a caller compiled against the old spelling would
    /// pass three integers where the callee reads four regions. The new `shader_binding_table_region` type is a
    /// plain by-value POD (adding a TYPE moves nothing on its own - it is the slot it appears in), and it is
    /// FROZEN once shipped, like `image_copy_region`.
    /// 23 -> 24 in the RAY-TRACING LAUNCH batch, and THREE THINGS MOVED - all of them interface changes, because
    /// this batch moved a verb to the face it belongs on and deleted what that left behind:
    ///  * `command_buffer` (tier 1) GAINED `trace_rays(...)` as an APPENDED slot - the launch is an ordered
    ///    recording command like `draw`/`dispatch`/`draw_mesh_tasks`, and the backend serves it against the
    ///    `vkCmdTraceRaysKHR` pointer it resolves once at startup.
    ///  * the `ray_tracing` ability LOST its `trace_rays` (moved to the recording face): a launch reachable only
    ///    through an ANNOUNCED ability is unreachable on a backend that serves the recording face without having
    ///    frozen the ability's acceleration-structure shapes - which is this backend's state, because those
    ///    shapes are still the S1 design surface. Its remaining three slots did NOT move (the removed one was
    ///    last).
    ///  * `vulkan_escape` LOST `device_proc(...)`, whose last caller was that launch: the BACKEND resolves the
    ///    entry point now, so nothing in the engine asks a device for one through a basis. Its `shader_group_handles`
    ///    SHIFTED DOWN one slot - which is precisely the case the number exists for, and why the removal is
    ///    recorded here rather than done quietly.
    /// TWO PODs JOINED with no bump of their own (a type moves nothing until a slot carries it): the general
    /// `shader_binding_table_region` and the session's `shader_binding_table_properties`, the three numbers a pass
    /// builds a table from. A third `buffer_use`/`buffer_flag`-style addition is a VALUE and, by the rule above,
    /// renumbers nothing.
    /// 24 -> 25 in the HOST-IMAGE-COPY batch: `image` GAINED one APPENDED virtual, `get_content()`, which
    /// answers the image's CONTENT in host memory (`image_content`) instead of a handle to it. It is the
    /// read-back shape the engine sees from now on, and the backend serves it with `VK_EXT_host_image_copy`
    /// (no staging buffer, no copy command, no submission). The two new TYPES (`image_content`, and the
    /// `bytes_per_pixel(image_format)` helper they rely on) move nothing on their own - it is the slot that
    /// carries them - and `image_content`'s invariant is FROZEN with it: a backend that cannot fill it exactly
    /// answers an error rather than a partial buffer.
    /// 25 -> 26 in the ACCELERATION-STRUCTURE batch: the acceleration structure became TIER-1 FURNITURE (like
    /// `buffer` and `image`), which is two APPENDED SLOTS on two existing interfaces -
    /// `api_core::create_acceleration_structure()` and the recording face's
    /// `command_buffer::build_acceleration_structure()` / `refit_acceleration_structure()`. The new TYPES
    /// (`acceleration_structure`, `acceleration_structure_desc`, `acceleration_structure_geometry`,
    /// `acceleration_structure_instance`) and the new `interface_type::acceleration_structure` value carry no
    /// bump of their own. THE `ray_tracing` ABILITY IS RETIRED in the same number: its three
    /// acceleration-structure verbs are gone (their operands were never more than forward declarations in the
    /// extension file, so no backend could serve them), its BIT stays unused and its `interface_type` value
    /// stays retired - a bit and a number are never reused (see `rhi.extension.cppm`).
    /// 26 -> 27 in the MICROMAP batch (plan S1's P4): the opacity micromap got the SAME tier-1 treatment the
    /// acceleration structure got one number earlier - `api_core::create_micromap()` and the recording face's
    /// `command_buffer::build_micromap()` are two APPENDED SLOTS, and the new types (`micromap`,
    /// `micromap_desc`, `micromap_usage`, `micromap_triangle`) plus `interface_type::micromap = 13` carry no bump
    /// of their own. The geometry that consults one declares it with `acceleration_structure_geometry::
    /// opacity_micromap` - a CONTRACT handle, never a driver's.
    /// 27 -> 28: swapchain::format() appends the pre-acquire presentation format query.
    /// 28 -> 29: image::share() and image_view::get_image() append owning parent-reference access.
    /// 29 -> 30: object stores a concrete implementation name instead of an interface enum.
    /// 30 -> 31: immutable texture groups and command-buffer group loading.
    // ABI 32 adds ordinary indirect drawing to the recording interface.
    // ABI33: owned readback storage uses vessel allocator across the DLL boundary.
    // NOT A BUMP (the descriptor space ability): `descriptor_space` is a NEW tier-2 ability, so it is a
    // new `extension_kind` bit (12) and a new interface type - no existing vtable moves. Its vocabulary
    // (`descriptor_position` / `descriptor_space_properties` / `descriptor_write`) is NEW by-value POD
    // types, and `structure_type::descriptor_write` is an APPENDED enum value (6) - the same "appending a
    // value moves nothing" rule the error enumeration and the earlier structure tags follow. The
    // texture-group object and every existing verb keep their slots: the ability is the servable face of
    // detail the backend already owns (its descriptor heap + group table), not a re-shape of it.
    // ABI 34 retires the LAST native-vocabulary transition. `vulkan_escape::native_image_format()` is DELETED:
    // it answered the resolved VkFormat of an image, no caller asked for it, and the raw heap view path it
    // existed for is portable now. Removing a virtual from an existing tier-2 interface shifts every slot
    // after it - the case `abi_version` exists for, so the number moves even though no by-value POD changed.
    // The three transition structures `vulkan_heap_image_desc` / `vulkan_heap_image_info` /
    // `vulkan_command_buffer_info` are DELETED with it (no caller ever set their `next` chain); their
    // `structure_type` values are RETIRED and never reused, and a heap request that still chains one is
    // refused with `unsupported` instead of being walked. The heap image write is now described ONLY by the
    // portable `image_view_desc` (role/layers/mips/format derived from the contract image).
    // ABI 35 is the MERGEABLE COMMAND BUFFER batch: `api_core` APPENDED two virtuals - the
    // `make_mergeable_command_buffer` factory and the `submit(std::span<mergeable_command_buffer_ref const>)`
    // overload. That is the vtable-append case the number exists for. The new tier-1 interface
    // `mergeable_command_buffer` is a SIBLING of `command_buffer` (both derive `object` directly; the
    // recording verbs are declared twice on purpose) and therefore carries no number of its own. The
    // existing submission (`submit(command_buffer&)`), `execute()` and the frame path keep their slots and
    // their meaning: this batch adds ONE more way to reach the queue (an ordered, self-contained batch,
    // atomically validated, with no implicit barriers) without re-shaping anything already there.
    // ABI 36 is the RECORDING-TIME BATCH MERGE: `command_buffer` APPENDED
    // `execute(std::span<mergeable_command_buffer_ref const>)` (the default body answers `unsupported`, so
    // a backend whose merge is submit-only still compiles and names the refusal) - a vtable append on an
    // existing tier-1 interface, the case the number exists for. The heap recording requests also gained
    // an APPENDED target (`heap_bind_info::mergeable_commands` / `heap_push_info::mergeable_commands`):
    // a mergeable segment publishes its binding/push data through the same ability as a command_buffer,
    // one target of the two set. Existing fields, slots and meanings do not move.
    // ---- THE MOST RECENT BATCHES, NEWEST FIRST (a new note goes directly below this line) -----------
    // ABI 45 APPENDS ONE FIELD TO `command_buffer_begin_info`: `inherit_descriptor_heaps` (default true). The
    // GUI overlay records ITS OWN mergeable segment now, and the Dear ImGui Vulkan backend binds its own classic
    // descriptor sets - so the backend must be able to start that segment WITHOUT chaining the engine's
    // descriptor-heap inheritance (a heap-inheriting continuation makes every ImGui bind trip VUID-...-11295/96).
    // The field is APPENDED to a by-value POD, so it is a number move; the default keeps every existing caller's
    // meaning, and a caller compiled before the field simply gets the default.
    // ABI 44 PUTS THE SHARED RHI OWNERSHIP ON THE CONTRACT'S OWN CONTAINER: every shared RHI handle the
    // contract returns is `shared_object<T>` (one control block; the factory reference dropped exactly once
    // at the last copy) instead of `std::shared_ptr<T>`: `image::share()`, `image_view::get_image()`,
    // `frame_resources::retain_image()`, `make_command_buffer()`, `mergeable_command_buffer_ref`,
    // `texture_group_ref`, and the `deren_make_api_core` entry's root. The MOVE-ONLY container took the
    // same step in the batch before this one (no separate number needed): the factories return the
    // contract's own `unique_object<T>`, and the `std::unique_ptr` + release-deleter spelling is gone -
    // same one-reference drop, same slots. `api_core` gains `release()` so the
    // root obeys the same contract drop rule as every other handle (for the root that call destroys it).
    // `shared_object<T>` also answers `weak` (a nested observation handle), `borrow()` (a backend-owned
    // pointer) and an aliasing constructor (an engine owner keeping a borrowed RHI pointer alive). The
    // virtual return types move, so this is a number move; the by-value PODs and the factory uniforms do
    // not move.
    // ABI 43 DELETES THE ESCAPE ENTIRELY: the `vulkan_escape` tier-2 interface (one surviving slot,
    // `native_command_buffer`), its `extension_kind::vulkan_escape` announcement, and the backend/probe/
    // GUI plumbing around it are gone. The GUI overlay receives the contract's `command_buffer` handle and
    // casts it to the backend's SHARED concrete layout (`denen.vulkan.command_buffer`, the same opaque
    // handle + concrete tag + shared layout rule `rhi::main_context` follows), so the contract exposes no
    // escape and no native type at all - the ability BIT stays as a retired value (bits are never reused).
    // Removing a tier-2 interface and an announced bit is a number move; `all_abilities` drops the bit and
    // `all_extension_kinds` its entry.
    // ABI 42 FINISHES THE ESCAPE: `vulkan_escape` loses `enabled_device_extensions`, `native_buffer`,
    // `native_image`, `native_image_view`, `native_sampler`, `get_basis` and `shader_group_handles` -
    // the last of them were test-only once the engine moved to the portable abilities. `api_basis` and
    // `vulkan_command_buffer_inheritance_info` (and their two structure tags) are deleted with them; the
    // tags' VALUES stay retired. ONE escape slot survives: `native_command_buffer`, because the Dear ImGui
    // Vulkan backend records its own draw data into the frame's command buffer. Removing tier-2 virtuals
    // shifts every later slot: that is the number.
    // ABI 41 puts the RESOURCE FACTORIES behind standard-library ownership: every `create_*` factory
    // (swapchain/buffer/image/acceleration structure/micromap/sampler/shader/pipeline/query/command
    // buffer) and `image::make_view` now RETURN `object_ptr<T>` - a `std::unique_ptr` whose deleter calls the
    // contract's `release()` (abi 43 replaced that spelling with the contract's own `unique_object<T>`,
    // same one-reference drop; the name in this note is the one the batch shipped). A caller that wants
    // shared ownership converted it with `std::shared_ptr(std::move(owned))` (abi 44 replaced that with
    // `shared_object<T>`); the contract carries no second ownership vocabulary, and views
    // (frame image/buffer/command list) are NOT releasable: releasing one is a precondition violation that
    // panics. A virtual's return type is part of its ABI, so this is a number move; no by-value POD changes
    // shape and no slot moves.
    // ABI 40 RETIRES THE ESCAPE'S DEVICE HANDLES: `native_instance()` / `native_physical_device()` /
    // `native_device()` / `native_queue()` / `native_swapchain_image_format()` are DELETED. Their last
    // non-test caller was the GUI plugin, which now receives the OPAQUE `rhi::main_context` (abi 39) and
    // casts it to its API's shared layout; the tests/spike that still needed a native device/queue do the
    // same. Removing virtuals from an existing tier-2 interface shifts every slot after them: that is the
    // number. `native_command_buffer` and the resource-handle borrows stay (raw recording/verification).
    // ABI 39 INTRODUCES THE MAIN-THREAD CONTEXT: `rhi::main_context` (an OPAQUE handle carrying only the
    // concrete-identity tag) and the appended `api_core::main_context()` virtual that answers it. It is
    // the contract's answer to "a non-engine consumer needs the API handles": one opaque hand-off the
    // API-specific plugin casts to its own layout, instead of adding native accessors here or to the
    // escape. The virtual is appended (a vtable append on an existing tier-1 interface) and has a default
    // body answering nullptr, so a backend with nothing to hand out compiles unchanged.
    // ABI 38 REMOVES THREE ZERO-CALL ESCAPE SLOTS: `vulkan_escape::enabled_instance_extensions()`,
    // `native_pipeline()` and `native_shader_module()` are DELETED - nothing in this repository called
    // them (the device-extension list is still asked for, the two resource handles moved to portable
    // contract factories/abilities), and dead vocabulary is removed rather than kept "in case". Removing
    // virtuals from an existing tier-2 interface shifts every slot after them: that is the number.
    // ABI 37 RETIRES THE SECONDARY COMMAND BUFFER: `command_buffer_kind::secondary` becomes a reserved value
    // the factories refuse by name, and `command_buffer::execute(command_buffer&)` is DELETED - the last
    // single-secondary recording-time merge, replaced by the batch `execute(span)` the multithreaded passes
    // use. Removing a virtual from an existing tier-1 interface shifts every slot after it (the case the
    // number exists for); the enum VALUE stays and is never reused. No by-value POD changes shape.
    // NOT A BUMP (the validate-and-cast helpers): `is_concrete()` / `do_concrete()` below are inline and
    // template FREE functions over the existing `object::type` field - no vtable slot, no by-value POD,
    // no entry-point signature moved, and the NAMES stay the backend's (the contract owns only the
    // operation). They live here because EVERY backend needs the same "check the concrete name, then
    // cast to the concrete layout" pair, and one copy is what keeps the next backend from reinventing
    // it (or from weakening the panic/refusal split its own doc states).
    // ABI 46 changes the shared ownership control-block protocol: strong ownership now
    // carries an implicit weak reference. Old inline drop code cannot share these blocks.
    // ABI47 appends the enabled buffer-int64-atomics capability query.
    // ABI48 adds compute indirect recording and its enabled-capability query.
    // ABI 49: shared command_recorder base and single extension recording target.
    // ABI 50: root operations split into borrowed resources/commands/frames subviews.
    // ABI 51: immutable general resource groups, pipeline binding layout and recording/factory verbs.
    // ABI 52: unified recorder push_constants, retired heap push API/property.
    // ABI 53: deferred recording application and native interop lifecycle.
    // ABI 54: remove incomplete query/swapchain factories and the unused mesh_shader extension.
    // Retired extension bits retain their numeric identity and are never reused.
    // ABI 55: logical fixed-slot descriptor writes and binding on descriptor_space.
    inline constexpr std::uint32_t abi_version = 55u;

    /// Why a promise entry point could not do what it was asked.
    ///
    /// An exception must not cross the boundary (plan §4.2), so every failure a
    /// backend can report travels as one of these through an out-parameter.
    ///
    /// `abi_mismatch` = 7 is not a system error number: it is the code the minimal
    /// use case measured in plan §10.3 reports, and it is kept here so that the
    /// refusal stays observable from the engine side.
    ///
    /// Values 13-18 are APPENDED (2026-10-05, the error-mechanism batch) - never renumber, never
    /// reuse: an old engine compiled against 0-12 receives a code it has never seen and still has to
    /// route it somewhere, which appending keeps possible. Each value names the class of failure the
    /// CALLER acts on, because that is what a code crossing a boundary is for; the raw API-specific
    /// number travels beside it in `error_info::native_code`.
    ///
    /// `suboptimal` (Vulkan's present/acquire state) deliberately has NO value here: it is a state,
    /// not a failure, and the same VkResult means "carry on" at acquire and "rebuild" at present -
    /// the translation is the CALL SITE's decision, not the enum's.
    enum class error : std::uint32_t {
        ok = 0,                     ///< the call did what it was asked
        abi_mismatch = 7,           ///< the caller's abi_version is not the backend's
        unsupported = 8,            ///< the backend has no mechanism that can serve THIS resource/format
        invalid_argument = 9,       ///< the region does not fit the image, or the destination is too small
        not_ready = 10,             ///< no frame is in flight, or the frame that drew it is not done
        device_lost = 11,           ///< the device refused the submission/copy (VkResult failure)
        operation_failed = 12,      ///< underlying operation failed without a more precise error channel
        out_of_date = 13,           ///< the swapchain/target expired: rebuild it and retry, skipping this frame
        surface_lost = 14,          ///< the window/surface is gone: rebuild it; if the window closed, exit the loop
        timeout = 15,               ///< a wait or a query ran out of time: retryable, or degrade and carry on
        out_of_device_memory = 16,  ///< device memory exhausted: RECOVERABLE - free resources and retry
        out_of_host_memory = 17,    ///< host memory exhausted: usually fatal
        initialization_failed = 18, ///< creation failed and no code above names the cause: report and exit
    };

    /// Shader parameter writes use four-byte units. Subtract only after comparing offset to avoid
    /// overflow even for a UINT64_MAX byte count. Empty writes are invalid, not silent no-ops.
    [[nodiscard]] constexpr error validate_push_constants_range(std::uint32_t offset, std::uint64_t size,
                                                                std::uint32_t limit) noexcept {
        return size == 0 || (offset & 3u) != 0 || (size & 3u) != 0 || offset > limit || size > limit - offset
                   ? error::invalid_argument
                   : error::ok;
    }

    /// Which subsystem an `error` belongs to - as the CONTRACT derives it, never as a stored field.
    ///
    /// A zone stored next to the code would be a second source of truth able to contradict it (the
    /// same reason `validate_structure` answers a code rather than filling in a struct): inside the
    /// contract the zone is a pure function of the code, so `zone_of` is the only way it exists.
    /// Zones that genuinely carry independent information - the application's own graphics / asset /
    /// config / platform / internal vocabulary - live at the application layer, where the caller knows
    /// WHICH subystem asked for the work; the contract does not carry that field for them.
    enum class error_zone : std::uint8_t {
        api = 0,      ///< the device / window / frame pipeline refused or deferred the call
        resource = 1, ///< the backend has no mechanism that can serve the request
        argument = 2, ///< the caller passed something that cannot work as given
        internal = 3, ///< the backend's own machinery refused, mismatched or broke
    };

    /// The zone of @p code - total over the enum, so a new value forces its way through this switch
    /// (-Werror) and cannot silently inherit another zone.
    [[nodiscard]] constexpr error_zone zone_of(error const code) noexcept {
        switch (code) {
        case error::unsupported:
            return error_zone::resource;
        case error::invalid_argument:
            return error_zone::argument;
        // The api zone is "the device/window/frame state refused or deferred the call": a lost
        // device, an expired swapchain, a gone surface, a wait that ran out, memory the device or
        // host no longer has - and `not_ready`, which is the same family (the frame pipeline is
        // simply not there yet). The caller's move in every one of these is retry / rebuild /
        // degrade, not "fix the arguments" and not "report a backend bug".
        case error::not_ready:
        case error::device_lost:
        case error::out_of_date:
        case error::surface_lost:
        case error::timeout:
        case error::out_of_device_memory:
        case error::out_of_host_memory:
            return error_zone::api;
        // internal = the machinery, not the request: `ok` (a successful call's zone when a caller
        // wants one), the ABI handshake's own refusal, a catch-all failure and a creation that
        // failed under no heading above.
        case error::ok:
        case error::abi_mismatch:
        case error::operation_failed:
        case error::initialization_failed:
            return error_zone::internal;
        }
        return error_zone::internal; // unreachable for a valid enumerator; -Wswitch still guards above
    }

    /// Which graphics API produced a diagnostic. Append-only, same rule as `error` itself.
    enum class graphics_api : std::uint8_t {
        unknown = 0,
        vulkan = 1,
    };

    /// The decision AND the diagnosis of one failure, as the backend's failure point reported it.
    ///
    /// FROZEN BY RULE - the contract's by-value PODs are frozen (`submit_info` set the precedent):
    /// this struct is returned and carried BY VALUE, so its size is part of the calling convention an
    /// old engine and a new backend would disagree on. Any field addition moves `abi_version` - it is
    /// the same reason the factory's out-parameter changes `error*` -> `error_info*` only together
    /// with that bump. (An out-parameter structure the CALLER allocates could carry a `struct_size`
    /// header and grow safely; a by-value one cannot.)
    ///
    /// Three premises this definition rests on, each one a reason the next edit should think twice:
    /// 1. BOTH HALVES COMPILE WITH THE SAME TOOLCHAIN. `std::source_location`'s layout is
    ///    implementation-defined; the engine and a backend DLL compare layouts, not definitions.
    ///    That was already implied by `vstd` (the trimmed std module both halves share) and by the
    ///    BMI exchange; stating it here makes it a clause. The static_asserts below pin the layout
    ///    clang64 compiles, so a toolchain change fails loudly here instead of at a call site.
    /// 2. `message` and `where`'s text point into the BACKEND's static storage - which outlives the
    ///    call only because invariant 4 holds: the backend DLL is loaded once and NEVER unloaded
    ///    (the loader's `detach()` exists for exactly this). A backend that unloads hands its host
    ///    dangling text.
    /// 3. `native_code` is SIGNED on purpose: the Vulkan codes it carries are negative
    ///    (VK_ERROR_OUT_OF_DATE_KHR = -1000001004), and that number stored in an unsigned field is a
    ///    different number by the time a human reads it.
    struct error_info {
        error code = error::ok;                   ///< the decision layer: what the caller acts on
        graphics_api api = graphics_api::unknown; ///< who produced the diagnostic
        std::int32_t native_code = 0;             ///< the raw API code (signed; see premise 3)
        std::string_view message = {};            ///< the backend's static text, possibly empty
        std::source_location where = {};          ///< captured at the BACKEND's failure point, not the engine's call site
    };

    // The pinned layout (clang64: clang 22.x, libc++, x86-64). `code` 4 bytes, `api` 1 + 3 padding,
    // `native_code` 4, then `message` (a char const* + a size_t) and `where` land on their 8-byte
    // alignments. `where` is 8 bytes, not 24: libc++'s std::source_location holds ONE pointer to a
    // static __impl record (file/function/line/column), which also means the text it names lives in
    // static storage - premise 2's guarantee, measured rather than assumed. Trivially copyable is the
    // DESIGN constraint - the struct crosses a C-shaped boundary by value - and standard layout is
    // what makes offsetof below defined at all.
    static_assert(std::is_trivially_copyable_v<error_info>, "error_info crosses the boundary by value; it must stay trivially copyable");
    static_assert(std::is_standard_layout_v<error_info>, "error_info's pinned offsets below need standard layout");
    static_assert(sizeof(error_info) == 40, "error_info changed size - by-value PODs are frozen: moving abi_version is part of the change");
    static_assert(offsetof(error_info, code) == 0, "error_info's pinned layout moved");
    static_assert(offsetof(error_info, api) == 4, "error_info's pinned layout moved");
    static_assert(offsetof(error_info, native_code) == 8, "error_info's pinned layout moved");
    static_assert(offsetof(error_info, message) == 16, "error_info's pinned layout moved");
    static_assert(offsetof(error_info, where) == 32, "error_info's pinned layout moved");

    /// Concrete implementation identity. Names must have static storage duration.
    /// A name is not proof of device ownership or object lifetime.
    class object {
    public:
        std::string_view const type;

    protected:
        constexpr explicit object(std::string_view const name) noexcept
            : type(name) {
        }
        ~object() = default; // Destruction stays on each interface's release path.
    };

    /// WHAT AN RHI HANDLE IS, for the containers below: every template here is constrained to a type
    /// that inherits `object`, so a non-RHI type cannot be wrapped in one (there would be no `release()`
    /// for the container to call, and the constraint says so at compile time).
    template <typename candidate>
    concept object_type = std::derived_from<candidate, object>;

    /// ONE REFERENCE TO AN RHI OBJECT - the factories' return type.
    ///
    /// Move-only RAII and nothing else: the destructor calls `release()`, the contract's "drop the
    /// caller's ONE reference" (not `delete`: the backend answers reference counting itself and a shared
    /// resource may outlive the call). Deliberately not a standard-library pointer - its drop is a
    /// contract verb, and spelling that as a deleter type turns one verb into two spellings.
    template <typename rhi_object>
    class unique_object {
    public:
        unique_object() noexcept = default;

        /// take the reference @p instance carries (null is allowed: a factory refusal answers with one)
        unique_object(rhi_object* const instance) noexcept
            : instance{instance} {
            static_assert(object_type<rhi_object>, "unique_object manages only a contract object handle");
        }

        unique_object(unique_object&& other) noexcept
            : instance{other.instance} {
            other.instance = nullptr;
        }

        unique_object& operator=(unique_object&& other) noexcept {
            if (this != &other) {
                this->reset();
                this->instance = other.instance;
                other.instance = nullptr;
            }
            return *this;
        }

        unique_object(unique_object const&) = delete;
        unique_object& operator=(unique_object const&) = delete;

        ~unique_object() noexcept {
            this->reset();
        }

        /// DROP THE REFERENCE, ONCE (idempotent): a shared resource may outlive this call
        void reset() noexcept {
            static_assert(object_type<rhi_object>, "unique_object manages only a contract object handle");
            if (this->instance != nullptr) {
                this->instance->release();
                this->instance = nullptr;
            }
        }

        /// give up the reference WITHOUT dropping it (the caller takes over the `release()`)
        [[nodiscard]] rhi_object* release() noexcept {
            rhi_object* const released = this->instance;
            this->instance = nullptr;
            return released;
        }

        void swap(unique_object& other) noexcept {
            rhi_object* const temporary = this->instance;
            this->instance = other.instance;
            other.instance = temporary;
        }

        [[nodiscard]] rhi_object* get() const noexcept {
            return this->instance;
        }
        [[nodiscard]] rhi_object* operator->() const noexcept {
            return this->instance;
        }
        [[nodiscard]] rhi_object& operator*() const noexcept {
            return *this->instance;
        }
        [[nodiscard]] explicit operator bool() const noexcept {
            return this->instance != nullptr;
        }

        friend bool operator==(unique_object const& value, std::nullptr_t) noexcept {
            return value.instance == nullptr;
        }
        friend bool operator!=(unique_object const& value, std::nullptr_t) noexcept {
            return value.instance != nullptr;
        }
        friend bool operator==(unique_object const& left, unique_object const& right) noexcept {
            return left.instance == right.instance;
        }
        friend bool operator!=(unique_object const& left, unique_object const& right) noexcept {
            return left.instance != right.instance;
        }

    private:
        rhi_object* instance = nullptr;
    };

    namespace detail {
        /// THE ONE CONTROL BLOCK `shared_object` and its nested `weak` count: the strong count owns the
        /// single `release()` the factory reference stands for, `drop` is what that release runs (null =
        /// a BORROWED handle: it owns no reference and drops nothing), and the weak count keeps the block
        /// alive for the weak handles.
        struct shared_object_control final {
            shared_object_control(void (*drop_instance)(void*) noexcept, void* const own_instance) noexcept
                : owned{own_instance}
                , drop{drop_instance} {
            }
            /// the object whose reference the block owns; NOT the alias target (an aliasing handle may
            /// point at a different object than the one the control block drops)
            void* owned = nullptr;
            std::atomic<std::uint32_t> strong{1};
            /// One implicit weak reference belongs to the strong group until object destruction
            /// finishes. External weak releases can never delete a block still used by the drop.
            std::atomic<std::uint32_t> weak{1};
            void (*drop)(void*) noexcept = nullptr;
        };
    } // namespace detail

    /// SHARED OWNERSHIP OF ONE RHI REFERENCE - the contract's counterpart of a shared pointer.
    ///
    /// Copies share ONE control block, so the factory's single reference is dropped EXACTLY ONCE when the
    /// last copy goes (`release()`, as in `unique_object`). `borrow()` makes a non-owning handle that keeps
    /// a backend-owned pointer shareable but drops nothing (the frame-view case); the aliasing constructor
    /// binds a borrowed pointer to an ENGINE owner's lifetime (the engine's lease pattern).
    template <typename rhi_object>
    class shared_object {
    public:
        /// the non-owning observer of a shared handle (see `lock()`)
        class weak;

        shared_object() noexcept = default;
        shared_object(std::nullptr_t) noexcept {
        }

        /// take ownership of the one reference @p instance carries
        explicit shared_object(rhi_object* const instance) noexcept
            : instance{instance} {
            static_assert(object_type<rhi_object>, "shared_object manages only a contract object handle");
            if (instance != nullptr) {
                this->control = new detail::shared_object_control{&shared_object::drop_reference, instance};
            }
        }

        /// A BORROWED handle: shares the pointer with copies and drops no reference. A null control block is
        /// the non-owning spelling: copies copy the pointer, destruction clears it, `weak()` is empty and no
        /// allocation happens. The owner must keep @p instance alive for the whole use (standard §6.2.2).
        static shared_object borrow(rhi_object& instance) noexcept {
            shared_object borrowed;
            borrowed.instance = &instance;
            return borrowed;
        }

        shared_object(unique_object<rhi_object>&& owner) noexcept
            : shared_object(owner.release()) {
        }

        /// a handle to a DERIVED object as a handle to the interface: the SAME control block is shared,
        /// so the one reference is still dropped exactly once.
        template <typename derived_object>
            requires std::derived_from<derived_object, rhi_object>
        shared_object(shared_object<derived_object> const& other) noexcept
            : instance{static_cast<rhi_object*>(other.get())}
            , control{other.control}
            , alias_owner{other.alias_owner} {
            this->acquire();
        }

        template <typename derived_object>
            requires std::derived_from<derived_object, rhi_object>
        shared_object(shared_object<derived_object>&& other) noexcept
            : instance{static_cast<rhi_object*>(other.get())}
            , control{other.control}
            , alias_owner{std::move(other.alias_owner)} {
            other.instance = nullptr;
            other.control = nullptr;
        }

        template <typename derived_object>
            requires std::derived_from<derived_object, rhi_object>
        shared_object(unique_object<derived_object>&& owner) noexcept
            : shared_object(static_cast<rhi_object*>(owner.release())) {
        }

        /// AN ALIAS TO A BORROWED RHI POINTER tied to an engine owner's lifetime (the engine lease shape):
        /// the owner keeps the resource alive and this handle owns no RHI reference of its own.
        /// an ENGINE-OWNED handle (a test's std::make_shared<fake_root>, say) adopted as an alias: the
        /// shared pointer's own reference is the lifetime, so no RHI release runs.
        template <typename owner_type>
            requires std::derived_from<owner_type, rhi_object>
        shared_object(std::shared_ptr<owner_type> owner) noexcept
            : instance{owner.get()}
            , alias_owner{std::move(owner)} {
        }

        template <typename owner_type>
        shared_object(std::shared_ptr<owner_type> owner, rhi_object* const pointer) noexcept
            : instance{pointer}
            , alias_owner{std::move(owner)} {
        }

        /// the same alias with a CONTRACT-OWNED root: shares that handle's control block (so the root
        /// outlives the borrowed pointer) without dropping an extra reference of its own.
        template <typename owner_object>
            requires std::derived_from<owner_object, object>
        shared_object(shared_object<owner_object> const& owner, rhi_object* const pointer) noexcept
            : instance{pointer}
            , control{owner.control} {
            this->acquire();
        }

        shared_object(shared_object const& other) noexcept
            : instance{other.instance}
            , control{other.control}
            , alias_owner{other.alias_owner} {
            this->acquire();
        }

        shared_object(shared_object&& other) noexcept
            : instance{other.instance}
            , control{other.control}
            , alias_owner{std::move(other.alias_owner)} {
            other.instance = nullptr;
            other.control = nullptr;
        }

        shared_object& operator=(shared_object const& other) noexcept {
            if (this != &other) {
                shared_object copy{other};
                this->swap(copy);
            }
            return *this;
        }

        shared_object& operator=(shared_object&& other) noexcept {
            if (this != &other) {
                this->reset();
                this->instance = other.instance;
                this->control = other.control;
                this->alias_owner = std::move(other.alias_owner);
                other.instance = nullptr;
                other.control = nullptr;
            }
            return *this;
        }

        ~shared_object() noexcept {
            this->reset();
        }

        /// drop this copy's share (the last copy performs the one `release()`)
        void reset() noexcept {
            if (this->control == nullptr) {
                this->instance = nullptr;
                this->alias_owner.reset();
                return;
            }
            if (this->control->strong.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                if (this->control->drop != nullptr) {
                    this->control->drop(this->control->owned);
                }
                if (this->control->weak.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    delete this->control;
                }
            }
            this->instance = nullptr;
            this->control = nullptr;
        }

        void swap(shared_object& other) noexcept {
            rhi_object* const other_instance = other.instance;
            other.instance = this->instance;
            this->instance = other_instance;
            detail::shared_object_control* const other_control = other.control;
            other.control = this->control;
            this->control = other_control;
            this->alias_owner.swap(other.alias_owner);
        }

        [[nodiscard]] rhi_object* get() const noexcept {
            return this->instance;
        }
        [[nodiscard]] rhi_object* operator->() const noexcept {
            return this->instance;
        }
        [[nodiscard]] rhi_object& operator*() const noexcept {
            return *this->instance;
        }
        [[nodiscard]] explicit operator bool() const noexcept {
            return this->instance != nullptr;
        }

        /// how many shared copies hold the reference (the aliased owner's count for an alias); 0 = empty
        [[nodiscard]] std::uint32_t use_count() const noexcept {
            if (this->control != nullptr) {
                return this->control->strong.load(std::memory_order_acquire);
            }
            return this->alias_owner != nullptr ? static_cast<std::uint32_t>(this->alias_owner.use_count()) : 0u;
        }
        /// True for an owning reference or lifetime alias, false for borrow()/empty.
        /// An alias owner must actually retain its target; this cannot inspect arbitrary owner types.
        [[nodiscard]] bool owns_lifetime() const noexcept {
            return instance != nullptr && ((control != nullptr && control->drop != nullptr) || alias_owner != nullptr);
        }
        [[nodiscard]] bool unique() const noexcept {
            return this->use_count() == 1u;
        }

        /// whether @p other shares THIS handle's control block (the std::shared_ptr::owner_before identity)
        [[nodiscard]] bool shares_control(shared_object const& other) const noexcept {
            return this->control != nullptr ? this->control == other.control
                                            : this->alias_owner.get() == other.alias_owner.get();
        }

        friend bool operator==(shared_object const& value, std::nullptr_t) noexcept {
            return value.instance == nullptr;
        }
        friend bool operator!=(shared_object const& value, std::nullptr_t) noexcept {
            return value.instance != nullptr;
        }
        friend bool operator==(shared_object const& left, shared_object const& right) noexcept {
            return left.instance == right.instance;
        }
        friend bool operator!=(shared_object const& left, shared_object const& right) noexcept {
            return left.instance != right.instance;
        }

    private:
        template <typename>
        friend class shared_object;

        void acquire() noexcept {
            if (this->control != nullptr) {
                this->control->strong.fetch_add(1, std::memory_order_relaxed);
            }
        }

        static void drop_reference(void* const instance) noexcept {
            static_cast<rhi_object*>(instance)->release();
        }

        rhi_object* instance = nullptr;
        detail::shared_object_control* control = nullptr;
        std::shared_ptr<void const> alias_owner;
    };

    /// THE WEAK SIDE of a shared RHI handle: observes without owning and locks into a shared handle while a
    /// strong reference is still alive. Nested in `shared_object` because it exists for exactly this family.
    template <typename rhi_object>
    class shared_object<rhi_object>::weak {
    public:
        weak() noexcept = default;

        weak(shared_object<rhi_object> const& owner) noexcept
            : instance{owner.instance}
            , control{owner.control} {
            if (this->control != nullptr) {
                this->control->weak.fetch_add(1, std::memory_order_relaxed);
            }
        }

        weak(weak const& other) noexcept
            : instance{other.instance}
            , control{other.control} {
            if (this->control != nullptr) {
                this->control->weak.fetch_add(1, std::memory_order_relaxed);
            }
        }

        weak(weak&& other) noexcept
            : instance{other.instance}
            , control{other.control} {
            other.instance = nullptr;
            other.control = nullptr;
        }

        weak& operator=(shared_object<rhi_object> const& owner) noexcept {
            weak copy{owner};
            this->swap(copy);
            return *this;
        }
        weak& operator=(weak const& other) noexcept {
            if (this != &other) {
                weak copy{other};
                this->swap(copy);
            }
            return *this;
        }
        weak& operator=(weak&& other) noexcept {
            if (this != &other) {
                this->release_control();
                this->instance = other.instance;
                this->control = other.control;
                other.instance = nullptr;
                other.control = nullptr;
            }
            return *this;
        }

        ~weak() noexcept {
            this->release_control();
        }

        /// a shared handle while at least one strong reference is alive, empty otherwise
        [[nodiscard]] shared_object<rhi_object> lock() const noexcept {
            if (this->control == nullptr) {
                return {};
            }
            std::uint32_t count = this->control->strong.load(std::memory_order_acquire);
            while (count != 0) {
                if (this->control->strong.compare_exchange_weak(count, count + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    shared_object<rhi_object> locked;
                    locked.instance = this->instance;
                    locked.control = this->control;
                    return locked;
                }
            }
            return {};
        }

        [[nodiscard]] bool expired() const noexcept {
            return this->control == nullptr || this->control->strong.load(std::memory_order_acquire) == 0;
        }

        [[nodiscard]] std::uint32_t use_count() const noexcept {
            return this->control != nullptr ? this->control->strong.load(std::memory_order_acquire) : 0u;
        }

        void reset() noexcept {
            this->release_control();
        }

        void swap(weak& other) noexcept {
            rhi_object* const other_instance = other.instance;
            other.instance = this->instance;
            this->instance = other_instance;
            detail::shared_object_control* const other_control = other.control;
            other.control = this->control;
            this->control = other_control;
        }

    private:
        void release_control() noexcept {
            if (this->control != nullptr) {
                if (this->control->weak.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    delete this->control;
                }
                this->control = nullptr;
                this->instance = nullptr;
            }
        }

        rhi_object* instance = nullptr;
        detail::shared_object_control* control = nullptr;
    };

    /// A BORROWED, BACKEND-OWNED MAIN-THREAD CONTEXT (abi 39).
    ///
    /// The device-side state a non-engine consumer on the main thread brings itself up from - the GUI
    /// overlay is the caller, and it is the reason this exists: that consumer needs the API's own handles,
    /// and handing it ONE opaque context is the alternative to growing `vulkan_escape` with one virtual per
    /// handle.
    ///
    /// IT IS OPAQUE HERE ON PURPOSE. The contract carries only the concrete-identity tag (`object::type`),
    /// no native vocabulary and no handle members: an API-specific consumer checks the tag with
    /// `is_concrete()` and casts the handle to the concrete layout ITS API's backend and plugin share (for
    /// Vulkan: `deren::vulkan::main_context` in `denen.vulkan.main_context`, versioned with that
    /// backend/plugin pair rather than with this contract). A contract that named the layout would put the
    /// native vocabulary right back.
    ///
    /// OWNERSHIP: the `api_core` owns it; `api_core::main_context()` answers a BORROWED pointer valid for
    /// the context's lifetime. There is no `release()` and no reference to keep - the caller must never
    /// delete it.
    struct main_context : object {
        explicit main_context(std::string_view const name) noexcept
            : object(name) {
        }
    };

    /// THE CONCRETE-IDENTITY MECHANISM, SHARED BY EVERY BACKEND: a handle's `type` is the concrete
    /// implementation name (ABI 30), the backend OWNS the name constants, and the validate-then-cast
    /// operation lives HERE so a second backend cannot reinvent it (a D3D12 backend validates its own
    /// names with the same two calls).
    ///
    /// WHY THE GRANULARITY IS CONCRETE, NOT THE INTERFACE: one interface has several concrete
    /// implementations with different layouts behind it (`buffer` is owned and, separately, the
    /// backend's borrowed read-back slot; `image` is owned and the borrowed swapchain image), so the
    /// `static_cast` a true answer permits is defined only for the ONE layout the name names.
    ///
    /// A FALSE ANSWER IS A CALLER CONDITION at the function's entrance: a handle this backend did not
    /// hand out, which the contract answers `rhi::error::invalid_argument` (or `nullptr` from a
    /// factory). It is never a panic. The panic belongs to `do_concrete()`, which runs only after the
    /// provenance has already been established - see its note.

    /// @brief whether @p value is the concrete implementation @p name names
    [[nodiscard]] constexpr bool is_concrete(object const& value, std::string_view const name) noexcept {
        return value.type == name;
    }

    /// @brief the concrete implementation behind @p value, as its own type
    ///
    /// PANICS ON A MISMATCH, AND THAT IS DELIBERATE: by the time this is called the provenance has
    /// already been established (registry membership, an object-identity comparison, ...), so a name
    /// that does not match is the BACKEND'S own bookkeeping being inconsistent - a bug in the code
    /// holding the invariant, which is what `deren::utility::panic` exists for ("a failure the code
    /// EXPECTS travels as a value; a violated invariant terminates"). Callers that instead have a
    /// handle which may legitimately be foreign use `is_concrete()` and answer
    /// `rhi::error::invalid_argument`; they never route a container's caller bug through this cast.
    ///
    /// @param value the handle to cast; MUST already be proven to belong to the calling backend
    /// @param name the concrete name the cast requires, taken from that backend's own constants
    /// @param where the CALLER's site, captured by the default argument; the panic reports it
    /// @tparam concrete_t the backend's concrete adapter type; spelled in snake_case because it is a
    ///         local name for the type, not a conventional `T`
    template <typename concrete_t>
    [[nodiscard]] concrete_t const* do_concrete(object const& value, std::string_view const name,
                                                std::source_location const where = std::source_location::current()) noexcept {
        if (!is_concrete(value, name)) {
            deren::utility::panic(deren::utility::vessel::format(
                                      "rhi: do_concrete: a handle named \"{}\" is not the \"{}\" this cast requires - "
                                      "provenance was already established, so the name and the registry disagree",
                                      value.type, name),
                                  where);
        }
        return static_cast<concrete_t const*>(&value);
    }

    /// the mutable spelling, for the sites that record INTO the handle (a command buffer, a build target)
    template <typename concrete_t>
    [[nodiscard]] concrete_t* do_concrete(object& value, std::string_view const name,
                                          std::source_location const where = std::source_location::current()) noexcept {
        if (!is_concrete(value, name)) {
            deren::utility::panic(deren::utility::vessel::format(
                                      "rhi: do_concrete: a handle named \"{}\" is not the \"{}\" this cast requires - "
                                      "provenance was already established, so the name and the registry disagree",
                                      value.type, name),
                                  where);
        }
        return static_cast<concrete_t*>(&value);
    }

    enum class structure_type : std::uint32_t {
        unknown = 0,
        heap_image_write = 1,
        heap_buffer_write = 2,
        heap_bind = 3,
        heap_push = 4,                  ///< retired in ABI 52; never reused
        command_buffer_inheritance = 5, ///< attachment formats in the portable RHI vocabulary
        /// appended with the descriptor space ability: one portable sampled-descriptor write into a
        /// `descriptor_position` (see `descriptor_write`). A value, not a vtable, so appending it moves
        /// no `abi_version` - the same rule the `vulkan_*` tags below follow.
        descriptor_write = 6,
        descriptor_image_write = 7,
        descriptor_buffer_write = 8,
        descriptor_acceleration_structure_write = 9,
        descriptor_bind = 10,
        /// RETIRED in abi 34: the heap image write carries no native view any more. The VALUE stays (a tag,
        /// like an ability bit, is never reused) and the request validation refuses it.
        vulkan_heap_image = 0x10000,
        /// RETIRED in abi 34: a recording verb takes a contract command_buffer, never a raw VkCommandBuffer.
        vulkan_command_buffer = 0x10001,
        /// RETIRED in abi 42: the native attachment-inheritance chain is gone; the portable
        /// `command_buffer_inheritance_info` is the only spelling. The VALUE stays (values are never reused).
        vulkan_command_buffer_inheritance = 0x10002,
        /// RETIRED in abi 42: `api_basis` (and the escape's legacy SBT path) is gone; the portable
        /// `shader_group_access` answers that data. The VALUE stays.
        vulkan_device_basis = 0x10003,
    };

    /// next 只借用到同步调用结束。ABI 34 之后没有"未解释就静默丢链"的请求：除
    /// `vulkan_command_buffer_inheritance`（secondary recording 的附件格式，没有可移植拼写）以外，
    /// 带 `next` 的请求一律被拒绝（validate_structure(..., allow_next=false) 回答 unsupported）。
    struct structure_header {
        structure_type s_type = structure_type::unknown;
        std::uint32_t struct_size = sizeof(structure_header);
        structure_header const* next = nullptr;
    };

    [[nodiscard]] constexpr error validate_structure(structure_header const& header, structure_type const expected,
                                                     std::size_t const required_size, bool const allow_next = false) noexcept {
        if (header.struct_size < sizeof(structure_header) || header.struct_size < required_size || header.s_type != expected) {
            return error::invalid_argument;
        }
        if (!allow_next && header.next != nullptr) {
            return error::unsupported;
        }
        return error::ok;
    }

} // namespace deren::promise::rhi
