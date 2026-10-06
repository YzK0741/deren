// Headless unit tests: deren.utility.dynamic_link (RHI plan v4 §7.4) and the promise
// contract it carries (the module deren.promise.rhi, §3.3 - §3.5, §4.1 - §4.2) =======
//
// The loader is the primitive the backend boundary is built on, so what is checked here is the
// contract rather than an implementation detail: a file that is not there is a returned error and
// not a crash, the platform suffix is completed when the caller leaves it off, a missing symbol is
// an error, a relative bare name is found, the library is unloaded when it goes out of scope, a
// moved-from library is empty instead of dangling, and each symbol lookup goes through the
// backend's own deleter exactly once.
//
// The library under test is tests/probe_backend.cpp, which CMake builds twice from one source
// file: the static half is linked into this test (so abi_export.hpp's static branch is exercised
// by the linker) and the DLL half is only ever opened at run time (the shared branch). Both halves
// are then driven through the SAME function, `check_core_contract()`, because "the two answer the
// same" is the whole point of the export keywords - and the C ABI is declared once, in
// promise/rhi/backend_entry.hpp, rather than again here.
#include "vk_test.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

import deren.promise.rhi;
import deren.utility.dynamic_link;

// After the imports it needs: the header names deren::promise::rhi types (see its own note).
#include "../promise/rhi/backend_entry.hpp"

namespace {

    namespace fs = std::filesystem;
    namespace rhi = deren::promise::rhi;

    // 推送常量的地址与长度都以4字节为单位；越界检查必须避免加法溢出。
    static_assert(rhi::validate_heap_push_range(0u, 4u, 4u) == rhi::error::ok);
    static_assert(rhi::validate_heap_push_range(4u, 4u, 8u) == rhi::error::ok);
    static_assert(rhi::validate_heap_push_range(1u, 4u, 8u) == rhi::error::invalid_argument);
    static_assert(rhi::validate_heap_push_range(0u, 1u, 8u) == rhi::error::invalid_argument);
    static_assert(rhi::validate_heap_push_range(0u, 0u, 8u) == rhi::error::invalid_argument);
    static_assert(rhi::validate_heap_push_range(4u, 8u, 8u) == rhi::error::invalid_argument);
    static_assert(rhi::validate_heap_push_range(12u, 4u, 8u) == rhi::error::invalid_argument);
    static_assert(rhi::validate_heap_push_range(4u, UINT64_MAX - 3u, 8u) == rhi::error::invalid_argument);

    // ABI 参数误标、截断及未知扩展必须被拒绝；这些用例在编译时执行，不需要GPU。
    static_assert(rhi::validate_structure(rhi::heap_buffer_write_info{}.header, rhi::structure_type::heap_buffer_write,
                                          sizeof(rhi::heap_buffer_write_info)) == rhi::error::ok);
    static_assert([] {
        auto info = rhi::heap_buffer_write_info{};
        info.header.s_type = rhi::structure_type::heap_push;
        return rhi::validate_structure(info.header, rhi::structure_type::heap_buffer_write, sizeof(info)) == rhi::error::invalid_argument;
    }());
    static_assert([] {
        auto info = rhi::heap_buffer_write_info{};
        info.header.struct_size = sizeof(rhi::structure_header) - 1u;
        return rhi::validate_structure(info.header, rhi::structure_type::heap_buffer_write, sizeof(info)) == rhi::error::invalid_argument;
    }());
    static_assert([] {
        auto info = rhi::heap_buffer_write_info{};
        rhi::structure_header extra{};
        info.header.next = &extra;
        return rhi::validate_structure(info.header, rhi::structure_type::heap_buffer_write, sizeof(info)) == rhi::error::unsupported;
    }());
    static_assert(rhi::buffer::interface_id != rhi::image::interface_id);
    static_assert(rhi::extension_interface_type(rhi::extension_kind::descriptor_heap) == rhi::descriptor_heap::interface_id);
    static_assert(std::is_standard_layout_v<rhi::vulkan_heap_image_info> && offsetof(rhi::vulkan_heap_image_info, header) == 0);
    static_assert(std::is_standard_layout_v<rhi::vulkan_command_buffer_info> && offsetof(rhi::vulkan_command_buffer_info, header) == 0);
    static_assert([] {
        auto info = rhi::heap_buffer_write_info{};
        info.header.struct_size = sizeof(info) - 1u;
        return rhi::validate_structure(info.header, rhi::structure_type::heap_buffer_write, sizeof(info)) == rhi::error::invalid_argument;
    }());
    static_assert([] {
        auto info = rhi::heap_buffer_write_info{};
        info.header.struct_size += 64u;
        return rhi::validate_structure(info.header, rhi::structure_type::heap_buffer_write, sizeof(info)) == rhi::error::ok;
    }());

    // From CMake ($<TARGET_FILE_NAME:probe_backend>): the DLL suffix is platform-dependent, so the
    // test does not hard-code it.
    constexpr std::string_view probe_file_name = VR_TEST_PROBE_BACKEND_FILE_NAME;
    constexpr std::string_view missing_file_name = "deren_probe_backend_that_does_not_exist.dll";
    constexpr std::string_view unload_probe_file_name = "deren_probe_backend_unload_probe.dll";
    constexpr std::string_view detach_probe_file_name = "deren_probe_backend_detach_probe.dll";

    // abi 18: the entry RETURNS the owning handle, so there is no second function to resolve and no
    // deleter name to look up - the contract's own typedef is used so this test cannot drift from the
    // header it is testing (promise/rhi/backend_entry.hpp).
    using make_core_fn = deren_make_api_core_fn;

    /** @brief a resolved C symbol: void const* to function pointer, constness dropped on purpose */
    template <typename function>
    function as_function(void const* address) {
        return reinterpret_cast<function>(const_cast<void*>(address));
    }

    /**
     * @brief a command list the probe NEVER handed out - the foreign-list refusal's subject
     *
     * `api_core::submit(command_list&)` has to tell "the list I handed out this frame" from "some
     * other list": without RTTI there is no honest way to check the dynamic type, so the contract's
     * precondition is the caller's - and a backend that cannot recognise the list must refuse it BY
     * NAME instead of guessing. This stand-in is that "some other list": it declares the same
     * interface and answers `invalid_argument` to everything, so a probe that accepted it would be
     * caught. It lives in the TEST, because "not handed out by the backend" is exactly what the test
     * knows and the probe cannot.
     */
    struct foreign_command_list final : rhi::command_list {
        [[nodiscard]] rhi::error use(rhi::image const&, rhi::image_use, rhi::image_use) noexcept override {
            return rhi::error::invalid_argument;
        }
        [[nodiscard]] rhi::error copy_image_to_buffer(rhi::buffer&, rhi::image const&, rhi::image_copy_region const&) noexcept override {
            return rhi::error::invalid_argument;
        }
        [[nodiscard]] rhi::error begin_gpu_timing() noexcept override {
            return rhi::error::invalid_argument;
        }
        [[nodiscard]] rhi::error mark_gpu_timing(std::uint32_t, std::string_view) noexcept override {
            return rhi::error::invalid_argument;
        }
    };

    /**
     * @brief a command buffer the probe NEVER handed out - the provenance rule's subject (abi 15)
     *
     * `command_buffer::execute()` has to tell "a buffer this backend made" from "some pointer a
     * caller has"; without RTTI the contract's precondition is the caller's, and the backend answers
     * `invalid_argument` for anything it does not recognise (never a cast of an unknown pointer).
     * This stand-in is that "not mine" case, and it lives in the TEST because the test is what knows
     * the difference.
     */
    struct foreign_command_buffer final : rhi::command_buffer {
        void release() noexcept override {
        }
        [[nodiscard]] rhi::error begin_recording(rhi::command_buffer_begin_info const&) override {
            return rhi::error::invalid_argument;
        }
        [[nodiscard]] rhi::error end_recording() noexcept override {
            return rhi::error::invalid_argument;
        }
        [[nodiscard]] rhi::command_list* recording() noexcept override {
            return nullptr;
        }
        [[nodiscard]] rhi::error execute(rhi::command_buffer&) override {
            return rhi::error::invalid_argument;
        }
    };

    /**
     * @brief everything the promise contract promises, driven through one pair of entry points
     *
     * Called once with the symbol resolved from the loaded DLL and once with the one the linker
     * found in the static half. `make_core` is the only thing that differs; every expectation below
     * has to hold for both - and since abi 18 the ownership it hands over needs no deleter argument.
     */
    void check_core_contract(make_core_fn make_core, char const* which_half) {
        // The ABI number is part of the contract, not an implementation detail: pin the value so a
        // silent renumbering is a test failure and not a mystery at a customer's machine. 1 -> 2 in the
        // recording-surface batch: SIX new virtuals landed on EXISTING tier-1 types (a vtable shift).
        // 2 -> 3 in the one-creation-structure batch: `deren_make_api_core()` grew the creation
        // descriptor it always described itself as taking, and the backend's own twin of that
        // structure is gone - no vtable moved, but the C entry's signature did.
        // 3 -> 4 in the owned-handle batch: `release()` landed as a pure virtual on seven EXISTING
        // tier-1 types, which is the vtable-shifting case the rule above names.
        // 4 -> 5 in the addressable-buffer batch: `acceleration_structure_address` moved from the
        // `device_address` ability to `ray_tracing`, so BOTH tier-2 abilities changed shape.
        // 5 -> 6 when `vulkan_escape` gained `native_buffer` (a virtual appended to an existing
        // ability).
        // 6 -> 7 in the image face (DYNAMIC_LINK_V2.md §17 landed): `image` gained `make_view()`,
        // the owned `image_view` interface and the image/sampler descriptors became defined types,
        // and `vulkan_escape` grew the image/sampler native-handle borrows.
        // plan §10.3 measured the mechanism; this line is the number itself.
        // ABI12 adds object identity and moves heap services into tagged descriptor_heap requests.
        // ABI13 is the frame face: `api_core` appended `walk_frames()` / `profiler()` (a vtable
        // shift) with the new tier-1 types, and the entry's out-parameter became `error_info*` -
        // the C signature is the other thing the number protects.
        // ABI14 is the frame verbs' completion (the boundary batch, one renumbering for one batch):
        // `api_core` appended `submit(command_list&)` / `frame_swapchain()`, `present()` changed
        // `void` -> `error` (a presentation that failed silently was information loss), and the two
        // tier-1 vtables that carry the new recording surface grew - `command_list` appended
        // `begin_gpu_timing()` / `mark_gpu_timing(index, name)`, `swapchain` appended `recreate()` /
        // `extent()`. Appends and a return-type change on tier-1 vtables are exactly the case the
        // number exists for.
        // ABI15 is the recording surface's ownership: `api_core` appended
        // `create_command_buffer(command_buffer_desc const&)` - a virtual on an existing tier-1 type -
        // and `command_buffer` is a NEW tier-1 interface (`release` / `begin_recording` /
        // `end_recording` / `recording` / `execute`) with its own `interface_type` value. The `next`
        // chain of `command_buffer_begin_info` is data, so the tagged structures that ride it do not
        // renumber anything (the same rule the appended `error` values follow).
        // ABI16 is the SAMPLER DESCRIPTION's width: `sampler_desc` gained `mag_filter` / `min_filter` /
        // `mipmap_mode` / `compare_enable` / `compare_op` so the engine can create the renderer's own
        // sampler set (NEAREST minification, NEAREST mipmapping, a `less_or_equal` comparison) and stay
        // value-for-value equal to the samplers the backend used to create for it. A descriptor that
        // travels by `const&` into `create_sampler()` is part of the ABI even though no vtable moved -
        // `struct_size` is the first member that makes the bump checkable rather than assumed.
        // ABI17 is the contract-only runtime's merged slice (③-D/E step 2): `vulkan_escape` APPENDED
        // `native_swapchain_image_format()` - a virtual on an existing tier-2 interface. The engine
        // builds the pipelines that render into the presentation image (the post chain, FXAA, the
        // upscale resolve) and the debug overlay BEFORE the first frame is acquired, and the contract's
        // only format query (`native_image_format(image const&)`) needs an image - the only swapchain
        // image a caller can name is `frame_image()`, which is nullptr until an acquire. So the surface
        // format had no pre-acquire answer, and this accessor is the one thing the slice adds to the
        // contract. (The number was previously reserved for `graphics_queue_family_index()`; that entry
        // was withdrawn when the queue family became a read-only runtime derivation.)
        // ABI18 is the SHARED flip: the C entry goes from THREE symbols to ONE - `deren_make_api_core`
        // RETURNS a `std::shared_ptr<api_core>` (ownership in the control block, so
        // `deren_destroy_api_core` has nothing left to do), and the version is the call's FIRST
        // ARGUMENT (so `deren_abi_version()` has nothing left to answer). The old spelling existed to
        // keep "one C++ runtime" out of the boundary's premises; that premise is now measured and
        // machine-checked (`cxx deren.exe imports libc++.dll`, which covers the DLL too). A C-entry
        // signature change is what the number protects, exactly as it did for 2 -> 3.
        // ABI19 is the CONSUMER-SIDE HANDSHAKE batch: `api_core` APPENDED `api_version() const noexcept`
        // - the object's own attestation of the number both halves compile - which the engine asks in the
        // runtime's constructor (a mismatch is a panic naming both numbers). An appended virtual on an
        // existing tier-1 interface is the vtable case the number exists for, EVEN THOUGH it produces no
        // backend symbol (the engine dispatches it through its own vtable copy: the boundary's measured
        // count and the import meter stay at ZERO). The same batch moved the loading and the acquisition
        // of the device root out of the runtime into deren.vulkan.backend_loader, called by main.cpp -
        // which is why this test's subject (the loader) is now something the app uses rather than
        // something the runtime hides.
        CHECK(rhi::abi_version == 19u);
        CHECK(static_cast<std::uint32_t>(rhi::error::ok) == 0u);
        CHECK(static_cast<std::uint32_t>(rhi::error::abi_mismatch) == 7u);

        // THE CREATION DESCRIPTOR IS THE CONTRACT'S ONE STRUCTURE, and this is the proof that it
        // crosses: every field is filled with a value the defaults would not produce, and the probe
        // echoes `window_width` back through `frame_begin()` (see probe_backend.cpp). `struct_size`
        // stays at the type's own default - it is the ABI guard, not a knob.
        rhi::create_info creation{};
        creation.window_width = 3;
        creation.window_height = 5;
        creation.vsync = false;
        creation.validation_layers = true;
        creation.window_visible = false;
        creation.window_title = "probe";

        // A mismatched ABI is refused before any object exists, and it is reported through the out
        // parameter - a null `api_core` plus the WHOLE diagnostic (abi 13: code, api, text, the
        // failure point), never a crash and never an exception.
        rhi::error_info mismatch_error{};
        CHECK(!make_core(rhi::abi_version + 1u, &creation, &mismatch_error));
        CHECK_MSG(mismatch_error.code == rhi::error::abi_mismatch, which_half);
        // THE BACKEND'S OWN NUMBER COMES BACK IN THE DIAGNOSTIC (abi 18): `deren_abi_version()` is
        // gone, so a host that disagrees learns the number it disagreed WITH from the refusal itself.
        CHECK_MSG(mismatch_error.native_code == static_cast<std::int32_t>(rhi::abi_version), which_half);
        // ... and a caller that passes no out parameter is still not crashed into (the backend has
        // to tolerate the null: the engine passes one, a probe or a script may not).
        CHECK(!make_core(rhi::abi_version + 1u, &creation, nullptr));

        // NO DESCRIPTOR IS A CALLER BUG, not a request for the standard context: the answer is a null
        // pointer and `invalid_argument`, and the contract spells "standard context" as `create_info{}`
        // (backend_entry.hpp). A backend that defaulted here would hide the bug.
        rhi::error_info missing_desc_error{};
        CHECK(!make_core(rhi::abi_version, nullptr, &missing_desc_error));
        CHECK_MSG(missing_desc_error.code == rhi::error::invalid_argument, which_half);

        // The matching call hands out a live object and says so in the out parameter. The sentinel
        // is not `ok`, so a backend that never wrote it fails the check below.
        rhi::error_info make_error{.code = rhi::error::abi_mismatch};
        std::shared_ptr<rhi::api_core> core = make_core(rhi::abi_version, &creation, &make_error);
        CHECK_MSG(make_error.code == rhi::error::ok, which_half);
        CHECK(core != nullptr);
        if (core == nullptr) {
            return;
        }
        CHECK(core->type() == rhi::api_core::interface_id);

        // Ownership is real and it came WITH the answer: the control block holds the backend's own
        // deleter (for the DLL half, one built inside that DLL), the count is observable, and copying
        // the handle does not hand the object out twice.
        CHECK(core.use_count() == 1);
        {
            std::shared_ptr<rhi::api_core> const borrowed = core;
            CHECK(core.use_count() == 2);
        }
        CHECK(core.use_count() == 1);

        // ---- tier-2: what the backend says it can do, and what it hands over ---------------
        // The probe announces only its working address ability. What matters here is the shape of the answer:
        // a bit set (not an ordered enum), no bit outside the known set, and every announced
        // ability reachable through `query_extension()` with the kind it claims.
        rhi::ability_bits const abilities = core->abilities();
        // TWO ABILITIES SINCE abi 19: the escape joined device_address, because the ENGINE’s runtime
        // asks it for the presentation image’s format before any frame exists (and derives the queue
        // family from its handles) - so a root the ENGINE can be injected with has to serve it. The probe
        // broadcasts both bits and answers both kinds with a matching object; the invariant walk below is
        // what keeps that honest.
        CHECK_MSG(abilities == (rhi::to_bits(rhi::extension_kind::device_address) |
                                rhi::to_bits(rhi::extension_kind::vulkan_escape)),
                  which_half);
        CHECK((abilities & ~rhi::all_abilities()) == rhi::no_abilities);

        // ---- G1: THE SAME INVARIANT WALKED OVER EVERY DEFINED BIT, instead of spelled out per kind -------
        // `all_extension_kinds()` is the CONTRACT's own list, so a sixth ability cannot be forgotten here:
        // for each kind, announced => `query_extension()` answers with an object whose `kind()` matches,
        // and not announced => it answers null. "Reported but not retrievable" AND "retrievable but not
        // reported" both fail, for the probe backend's two halves and - through this same function - for
        // the real backend, whose answer today is the empty set (see core's startup self-check, gate G2).
        for (rhi::extension_kind const kind : rhi::all_extension_kinds()) {
            rhi::extension* const ability = core->query_extension(kind);
            if (rhi::has_ability(abilities, kind)) {
                CHECK_MSG(ability != nullptr, which_half);
                if (ability != nullptr) {
                    CHECK(ability->kind() == kind);
                    CHECK(ability->type() == rhi::extension_interface_type(kind));
                }
            } else {
                CHECK_MSG(ability == nullptr, which_half);
            }
        }

        // ---- tier-1: a factory, and a virtual call that comes back out of the backend ---------
        // The one ability the probe actually implements, fetched by the kind it announces - the caller
        // asked for this kind, so the downcast below is a static_cast and not a dynamic_cast (-fno-rtti).
        // The traversal above already checked that an announced bit answers with a matching object.
        rhi::extension* const address_ability = core->query_extension(rhi::extension_kind::device_address);
        CHECK(rhi::query_extension<rhi::device_address>(*core) == address_ability);
        CHECK(rhi::query_extension<rhi::descriptor_heap>(*core) == nullptr);

        // The descriptor is a POD that crosses the boundary by value; the answer is a polymorphic
        // handle the caller can only see through the base. The other factories take descriptors
        // that are still forward-declared, so they cannot even be called yet (S1 defines them) -
        // which is exactly what "the shape can be reviewed before the shapes it carries" means.
        rhi::buffer* const buffer = core->create_buffer(rhi::buffer_desc{.size = 64u});
        CHECK(buffer != nullptr);
        if (buffer != nullptr) {
            CHECK(buffer->type() == rhi::buffer::interface_id);
            CHECK(buffer->size() == 64u);
            if (address_ability != nullptr) {
                // -fno-rtti: the caller knows what it asked for, so the downcast is a static_cast
                // and not a dynamic_cast (promise/rhi/rhi.extension.cppm says the same).
                auto* const address = static_cast<rhi::device_address*>(address_ability);
                // The answer depends on the size the descriptor carried, so this is a real round
                // trip: the descriptor went in, the virtual call came back out, and the arithmetic
                // the probe documents (0x1000 + size + offset) held on the way.
                CHECK(address->buffer_address(*buffer, 0u) == 0x1000ull + 64ull);
                CHECK(address->buffer_address(*buffer, 8u) == 0x1000ull + 64ull + 8ull);
            }
        }

        // ---- tier-1: THE OWNED REFERENCE IS RELEASED THROUGH THE CONTRACT ---------------------
        // `rhi::object_manager<T>` is the owner spelling added with `release()` (abi 3 -> 4): the
        // destructor (or `reset()`) calls the handle's `release()`, which runs INSIDE the backend and
        // drops ONE reference - it is deliberately not called `destroy()`, because a backend may serve
        // the same resource to several callers and then the object outlives the call. The proof here is
        // a round trip: the probe's `release()` zeroes the size its descriptor wrote into the buffer,
        // so a release that never crossed cannot show up as `size() == 0`.
        {
            rhi::object_manager<rhi::buffer> owned{core->create_buffer(rhi::buffer_desc{.size = 128u})};
            CHECK(static_cast<bool>(owned));
            CHECK(owned->size() == 128u);

            // move-only: the reference is transferred, and the source is left empty rather than
            // aliasing the same object (which would release it twice)
            rhi::object_manager<rhi::buffer> moved{std::move(owned)};
            CHECK(!static_cast<bool>(owned));
            CHECK(moved->size() == 128u);

            // release-without-release hands the raw handle back; the caller takes over the call
            rhi::buffer* const raw = moved.release();
            CHECK(raw != nullptr);
            CHECK(!static_cast<bool>(moved));
            CHECK(raw->size() == 128u); // no reference dropped yet
            raw->release();             // ... and now the caller drops it
            CHECK(raw->size() == 0u);

            // an empty manager is free: reset() twice is a no-op, and the object above is untouched
            // (the probe's buffer is static, so this is about the manager, not about the object)
            rhi::object_manager<rhi::buffer> empty{};
            empty.reset();
            empty.reset();
            CHECK(!static_cast<bool>(empty));
        }

        // ---- tier-1: the frame calls --------------------------------------------------------
        // No device means no command pool: a null command list is an answer, not a crash. The frame
        // face (abi 13) is here too, on the probe's two-slot ring: `frame_begin()` and the walker
        // answer the SAME cursor, `image_index` is the probe's ECHO OF THE CREATION DESCRIPTOR
        // (`creation.window_width` = 3), and the open's by-value POD carries the echo across with
        // its decision - a structure handed to `deren_make_api_core()` and never read there cannot
        // produce the number.
        // The list is answered only while a frame is OPEN (abi 14's rule, the real backend's too),
        // which is why this line comes before any `frame_begin()`: nothing is in flight yet.
        CHECK(core->begin_commands() == nullptr);
        // ... and `present()` (abi 14, now answering) refuses that same no-frame state by name: a
        // frame that was never acquired has nothing to show.
        CHECK_MSG(core->present() == rhi::error::not_ready, which_half);
        rhi::frame_walker* const walker = core->walk_frames();
        CHECK(walker != nullptr);
        CHECK(core->walk_frames() == walker); // the same borrowed view every call, never a new object
        CHECK(walker->slot_count() == 2u);    // the probe's ring
        rhi::submit_info const first = core->frame_begin();
        CHECK(first.frame_index == walker->position()); // one cursor, two faces agreeing
        CHECK(first.image_index == 3u);
        walker->walk_to_next();
        rhi::submit_info const second = core->frame_begin();
        CHECK(second.frame_index == walker->position());
        CHECK(second.frame_index == 1u); // the ring moved through the face
        CHECK(second.image_index == 3u);
        // the open: decision and frame in one by-value POD, and the echo rides in it
        rhi::frame_open_info const open = walker->wait_and_acquire();
        CHECK(open.result.code == rhi::error::ok);
        CHECK(open.frame.frame_index == walker->position());
        CHECK(open.frame.image_index == 3u);

        // ---- the abi 14 surface, on the same open frame ---------------------------------------
        // THE PRESENTATION SURFACE IS A BORROWED VIEW like the two above - the same object every
        // call - and its `extent()` is the descriptor echo again (`window_width` x `window_height` =
        // 3 x 5): the descriptor crossed, and the view answers with what it carried. `recreate()`
        // answers the STATE the contract names for a sized descriptor (a zero one is the deferred
        // state, checked below).
        rhi::swapchain* const surface = core->frame_swapchain();
        CHECK(surface != nullptr);
        CHECK(core->frame_swapchain() == surface); // the same borrowed view every call
        CHECK(surface->extent().width == 3u);
        CHECK(surface->extent().height == 5u);
        CHECK(surface->extent().depth == 1u);
        CHECK_MSG(surface->recreate() == rhi::error::ok, which_half);
        // THE TIMING VERBS ARE ON THE LIST (abi 14), and the list is the borrowed recording view -
        // reachable only because the open above opened the frame. The probe's device cannot
        // timestamp (its profiler says `unsupported`, and so do these two), but the ORDER rule is
        // measured here all the same: the positional index is checked before the capability, so an
        // index that is not the next mark is refused BY NAME.
        rhi::command_list* const commands = core->begin_commands();
        CHECK(commands != nullptr); // a frame is open: wait_and_acquire above opened it
        CHECK(core->begin_commands() == commands);
        CHECK(commands->begin_gpu_timing() == rhi::error::unsupported);
        CHECK_MSG(commands->mark_gpu_timing(1, "out of order") == rhi::error::invalid_argument, which_half);
        CHECK(commands->mark_gpu_timing(0, "frame begin") == rhi::error::unsupported);
        CHECK(commands->mark_gpu_timing(2, "out of order") == rhi::error::invalid_argument);
        // SUBMIT (abi 14): the frame's own list is accepted and hands the frame over; a list the
        // backend did not hand out is refused by name; and after the hand-over there is no frame to
        // submit any more.
        foreign_command_list foreign{};
        CHECK_MSG(core->submit(foreign) == rhi::error::invalid_argument, which_half);
        CHECK_MSG(core->submit(*commands) == rhi::error::ok, which_half);
        CHECK(core->begin_commands() == nullptr); // handed over: no frame is open to record into
        CHECK(core->submit(*commands) == rhi::error::not_ready);

        // ---- the abi 15 surface: the OWNED command buffer ---------------------------------------
        // `create_command_buffer()` hands out ONE reference; the probe answers with its static
        // stand-in (the same shape its buffer has), reset on every creation - so a second create
        // answers with the SAME pointer, and what this block measures is the LIFE CYCLE, not identity.
        rhi::command_buffer* const recorded = core->create_command_buffer(rhi::command_buffer_desc{.kind = rhi::command_buffer_kind::primary});
        CHECK_MSG(recorded != nullptr, which_half);
        if (recorded != nullptr) {
            CHECK(recorded->type() == rhi::command_buffer::interface_id);
            // the borrowed recording view: the same object every call, exactly like the frame's list
            rhi::command_list* const recorded_view = recorded->recording();
            CHECK(recorded_view != nullptr);
            CHECK(recorded->recording() == recorded_view);
            // ... and the frame-scoped verbs refuse a list that is not the frame's (the contract's own
            // window for `use` / the timing pair)
            CHECK(recorded_view->begin_gpu_timing() == rhi::error::not_ready);
            // the recording lifecycle, portable usage bits and all
            CHECK(recorded->begin_recording(rhi::command_buffer_begin_info{.usage = rhi::to_bits(rhi::command_buffer_usage::one_time_submit)}) == rhi::error::ok);
            CHECK(recorded->end_recording() == rhi::error::ok);
            // A SECONDARY comes back as the probe's one stand-in (documented above), which is enough
            // for the provenance rule the next two lines measure: a foreign buffer is refused by name,
            // this backend's own is accepted.
            rhi::command_buffer* const secondary = core->create_command_buffer(rhi::command_buffer_desc{.kind = rhi::command_buffer_kind::secondary});
            CHECK(secondary == recorded); // one static stand-in per probe, reset on each create
            foreign_command_buffer foreign{};
            CHECK_MSG(recorded->execute(foreign) == rhi::error::invalid_argument, which_half);
            CHECK_MSG(recorded->execute(*recorded) == rhi::error::ok, which_half);
            // RELEASE drops the one reference, and the probe echoes it: every later verb is not_ready
            recorded->release();
            CHECK(recorded->begin_recording(rhi::command_buffer_begin_info{}) == rhi::error::not_ready);
            CHECK(recorded->recording() == nullptr);
        }
        // a kind outside the two roles is the factory's one refusal (`nullptr`, the contract's rule)
        CHECK(core->create_command_buffer(rhi::command_buffer_desc{.kind = static_cast<rhi::command_buffer_kind>(99u)}) == nullptr);

        // ---- the abi 16 surface: THE WIDENED SAMPLER DESCRIPTOR -----------------------------------
        // What the probe CAN witness is the factory's own contract: one reference handed out, the right
        // interface id, and a `release()` that arrives. WHAT IT CANNOT WITNESS is the descriptor's new
        // fields actually crossing (the probe implements no `vulkan_escape`, and a sampler carries no
        // accessor to echo through) - so that is the SPIKE's job, against the real backend: it creates a
        // sampler spelled with NEAREST minification and a `less_or_equal` comparison and reads the native
        // handle back. The end-to-end witness is the render gate: the engine's six samplers are the ones
        // the passes bind, so a mis-laid-out descriptor would move all fourteen captures.
        {
            rhi::sampler_desc desc{};
            desc.address_mode = rhi::sampler_address_mode::clamp_to_edge;
            desc.max_lod = 0.0f;
            desc.mag_filter = rhi::sampler_filter::linear;
            desc.min_filter = rhi::sampler_filter::nearest;
            desc.mipmap_mode = rhi::sampler_mipmap_mode::nearest;
            desc.compare_enable = true;
            desc.compare_op = rhi::sampler_compare_op::less_or_equal;
            rhi::sampler* const made = core->create_sampler(desc);
            CHECK_MSG(made != nullptr, which_half);
            if (made != nullptr) {
                CHECK(made->type() == rhi::interface_type::sampler);
                made->release();
            }
            // ... and a descriptor spelled with the DEFAULTS is the old behaviour exactly, which is what
            // lets a caller built against abi 15 keep the samplers it was compiled against.
            rhi::sampler_desc defaults{};
            CHECK(defaults.struct_size == sizeof(rhi::sampler_desc));
            CHECK(defaults.mag_filter == rhi::sampler_filter::linear);
            CHECK(defaults.min_filter == rhi::sampler_filter::linear);
            CHECK(defaults.mipmap_mode == rhi::sampler_mipmap_mode::linear);
            CHECK(!defaults.compare_enable);
            CHECK(defaults.compare_op == rhi::sampler_compare_op::never);
        }

        // the profiler face: the probe cannot timestamp, which is the `unsupported` story - a
        // constant zero and a named refusal, never silence and never a fake measurement
        rhi::gpu_profiler* const profiler = core->profiler();
        CHECK(profiler != nullptr);
        CHECK(core->profiler() == profiler);
        CHECK(profiler->stage_count() == 0u);
        std::string_view stage_name{};
        std::uint64_t duration_ns = 0;
        CHECK_MSG(profiler->get_stage_info(0, &stage_name, &duration_ns) == rhi::error::unsupported,
                  which_half);
        // and the present that answers: a frame was acquired, so the probe echoes the call with ok
        CHECK_MSG(core->present() == rhi::error::ok, which_half);
        core->wait_idle();

        // ---- abi 16: THE BORROWED FRAME IMAGE HANDS OUT OWNED VIEWS (③-D/E item B) -------------------
        // The asymmetry is the whole point, and it is what a caller can get wrong: the IMAGE is borrowed
        // (its `release()` is a no-op that the probe counts), while a VIEW made from it is a new owned
        // object whose `release()` is real. A range outside the swapchain image's one-layer/one-mip shape
        // is REFUSED rather than clamped - the contract's rule for every `make_view()`.
        {
            rhi::image* const frame = core->frame_image();
            CHECK_MSG(frame != nullptr, which_half); // a frame is open at this point in the probe's fiction
            if (frame != nullptr) {
                rhi::image_view* const whole = frame->make_view(rhi::image_view_desc{});
                CHECK_MSG(whole != nullptr, which_half);
                if (whole != nullptr) {
                    CHECK(whole->type() == rhi::interface_type::image_view);
                    whole->release(); // the OWNED view: this is the call that has to arrive
                }
                rhi::image_view_desc outside{};
                outside.layer_count = 2; // a swapchain image has one layer
                CHECK(frame->make_view(outside) == nullptr);
                rhi::image_view_desc wrong_mip{};
                wrong_mip.base_mip = 1;
                CHECK(frame->make_view(wrong_mip) == nullptr);
                // ... and releasing the IMAGE ITSELF is the borrowed no-op: it carries no reference.
                frame->release();
            }
        }

        // DESTRUCTION IS THE CONTROL BLOCK'S JOB NOW (abi 18), and that is observable as OWNERSHIP
        // ARITHMETIC rather than as a wrapped deleter: the test can no longer name the function that
        // destroys the object (there is no second entry point to resolve), but it can pin that a second
        // reference keeps the SAME object alive and that the last one releases it. The descriptor is
        // passed again here and the out parameter is deliberately null: a backend has to tolerate both.
        {
            std::shared_ptr<rhi::api_core> scoped = make_core(rhi::abi_version, &creation, nullptr);
            CHECK(scoped != nullptr);
            if (scoped != nullptr) {
                CHECK(scoped.use_count() == 1);
                std::shared_ptr<rhi::api_core> const second = scoped; // a second OWNER, not a second object
                CHECK(second.get() == scoped.get());
                CHECK_MSG(scoped.use_count() == 2, which_half);
            }
            // ... the last reference goes here, and the control block's deleter - built inside the
            // backend - is what runs. A double delete or a leak on that path is what the sanitizer job
            // watches; no account of it is available to THIS half any more, which is the trade abi 18
            // made when it deleted `deren_destroy_api_core`.
        }
    }

    void test_a_missing_file_is_a_returned_error(fs::path const& directory) {
        auto const missing =
            deren::utility::dynamic_link::load((directory / std::string{missing_file_name}).string());
        CHECK(!missing.has_value());
        if (missing.has_value()) {
            return;
        }
        // The system's error number AND its text survive: a caller can branch on the number
        // (2 = ERROR_FILE_NOT_FOUND, 3 = ERROR_PATH_NOT_FOUND, 126 = ERROR_MOD_NOT_FOUND) and log
        // the text as it came from the OS.
        CHECK(missing.error().code != 0);
        CHECK(!missing.error().message.empty());
    }

    void test_the_platform_suffix_is_completed(fs::path const& directory) {
        fs::path const complete = directory / std::string{probe_file_name};
        fs::path const without_suffix = complete.parent_path() / complete.stem();
        auto const loaded = deren::utility::dynamic_link::load(without_suffix.string());
        CHECK(loaded.has_value());
        if (loaded.has_value()) {
            CHECK(loaded->native_handle() != nullptr);
        }
    }

    void test_loading_the_probe_dll_and_using_its_symbols(fs::path const& directory) {
        // 字符串视图里的 NUL 不能让系统悄悄只加载前半段路径。
        std::string nul_path = (directory / std::string{probe_file_name}).string();
        nul_path.push_back('\0');
        nul_path += "ignored.dll";
        auto const embedded_nul = deren::utility::dynamic_link::load(nul_path);
        CHECK(!embedded_nul.has_value());
        auto const empty = deren::utility::dynamic_link::load("");
        CHECK(!empty.has_value());
        if (!empty.has_value()) {
            CHECK(empty.error().message.find("empty") != std::string::npos);
        }

        auto loaded = deren::utility::dynamic_link::load((directory / std::string{probe_file_name}).string());
        CHECK(loaded.has_value());
        if (!loaded.has_value()) {
            return;
        }
        CHECK(loaded->native_handle() != nullptr);

        // THE ONE EXPORT (abi 18). The probe DLL exports exactly its entry point; the two symbols that
        // shaped abi 17 are GONE with it - the version is the call's first argument and the deleter
        // lives in the returned control block - so their absence is checked, not assumed.
        CHECK(!loaded->symbol("deren_no_such_symbol_in_the_probe_backend").has_value());
        CHECK(!loaded->symbol("").has_value());
        CHECK(!loaded->symbol("deren_abi_version").has_value());
        CHECK(!loaded->symbol("deren_destroy_api_core").has_value());

        auto const make = loaded->symbol("deren_make_api_core");
        CHECK(make.has_value());
        if (!make.has_value()) {
            return;
        }

        // The function driven here is the one RESOLVED FROM THE DLL, not the one the linker would give
        // this test: that is the plan's rule (§4.1) - an object made inside the library is owned and
        // destroyed inside the library - and with abi 18 that rule travels in the control block the
        // call returns.
        check_core_contract(as_function<make_core_fn>(make.value()), "dll");

        // A moved-from library is empty (not a second owner of the same handle), and the symbols
        // taken from the destination keep working.
        deren::utility::dynamic_link::library moved{std::move(*loaded)};
        CHECK(moved.native_handle() != nullptr);
        CHECK(loaded->native_handle() == nullptr);
        CHECK(moved.symbol("deren_make_api_core").has_value());
    }

    void test_the_library_is_unloaded_when_it_goes_out_of_scope(fs::path const& directory) {
        // A copy in the *current* directory, opened by its bare name: this is the relative branch
        // (classic search order) and it doubles as the unload proof - on Windows a loaded image
        // holds its file, so the file only becomes deletable after the destructor called
        // FreeLibrary.
        fs::path const copy = fs::current_path() / std::string{unload_probe_file_name};
        fs::copy_file(directory / std::string{probe_file_name}, copy, fs::copy_options::overwrite_existing);
        {
            auto const loaded = deren::utility::dynamic_link::load(copy.filename().string());
            CHECK(loaded.has_value());
            if (loaded.has_value()) {
                CHECK(loaded->symbol("deren_make_api_core").has_value());
#if defined(_WIN32)
                std::error_code still_mapped{};
                CHECK(!fs::remove(copy, still_mapped)); // sharing violation: the image is mapped
#endif
            }
        }
        std::error_code remove_error{};
#if defined(_WIN32)
        CHECK(fs::remove(copy, remove_error)); // the destructor unloaded it
#else
        static_cast<void>(fs::remove(copy, remove_error)); // POSIX allows unlinking a loaded .so
#endif
    }

    void test_a_detached_library_is_not_unloaded(fs::path const& directory) {
        // THE INVERSE OF THE UNLOAD PROOF, and the reason `detach()` exists (DYNAMIC_LINK_V2.md §13):
        // a detached library keeps its handle, so its file stays mapped after this object is gone.
        // MEASURED on the real backend - unloading it after a context had been built and torn down
        // never returned - so "the product never unloads" has to be expressible, and this is what
        // proves the expression works rather than assuming it.
        fs::path const copy = fs::current_path() / std::string{detach_probe_file_name};
        fs::copy_file(directory / std::string{probe_file_name}, copy, fs::copy_options::overwrite_existing);
        {
            auto loaded = deren::utility::dynamic_link::load(copy.filename().string());
            CHECK(loaded.has_value());
            if (!loaded.has_value()) {
                return;
            }
            CHECK(loaded->symbol("deren_make_api_core").has_value());

            void* const detached = loaded->detach();
            CHECK(detached != nullptr);
            // THE OBJECT IS EMPTY AFTERWARDS: the handle left with the caller, and every remaining
            // operation says so instead of using a handle it no longer owns.
            CHECK(loaded->native_handle() == nullptr);
            CHECK(!loaded->symbol("deren_make_api_core").has_value());
            // destructor runs here: it must NOT unload (the handle is gone; nothing left to close)
        }
#if defined(_WIN32)
        std::error_code still_mapped{};
        CHECK(!fs::remove(copy, still_mapped)); // sharing violation: detach did NOT unload it
                                                // The file stays: it is still mapped by THIS test process, which is exactly the claim. The
                                                // next run starts by overwriting it, and the mapping dies with the process.
#else
        std::error_code remove_error{};
        static_cast<void>(fs::remove(copy, remove_error)); // POSIX unlinks a loaded .so happily
#endif
    }

    void test_the_static_half_behaves_the_same() {
        // No library is involved here: abi_export.hpp's static branch produces ordinary C symbols,
        // and the same contract is reached through the linker. `deren_make_api_core` is the
        // declaration from promise/rhi/backend_entry.hpp - this test does not spell the C ABI out by
        // hand, so a drift between the header and the backend is a compile/link error.
        check_core_contract(&deren_make_api_core, "static");
    }
} // namespace

int main(int argc, char** argv) {
    // The probe DLL sits next to this executable (both are outputs of the CMake binary directory)
    // while the working directory is <binary dir>/test-run, so argv[0] is where to look. Deriving
    // the path here keeps generated string literals with Windows path separators out of the build
    // ("\p" would be an escape sequence).
    fs::path const self = argc > 0 && argv != nullptr && argv[0] != nullptr ? fs::absolute(fs::path{argv[0]})
                                                                            : fs::current_path();
    fs::path const directory = self.parent_path();

    test_a_missing_file_is_a_returned_error(directory);
    test_the_platform_suffix_is_completed(directory);
    test_loading_the_probe_dll_and_using_its_symbols(directory);
    test_the_library_is_unloaded_when_it_goes_out_of_scope(directory);
    test_a_detached_library_is_not_unloaded(directory);
    test_the_static_half_behaves_the_same();

    return deren::vk_test::finish("test_dynamic_link");
}
