// -*- C++ -*-
// ============================================================================
// file: tests/spike_backend_boundary.cpp
//
// THE BOUNDARY SPIKE (DYNAMIC_LINK_V2.md §4 step 1b, plan_rhi_v4.md §9 items 14-17).
//
// WHY THIS EXISTS AND WHY IT IS NOT IN `VR_TEST_TARGETS`: the plan's own review found that
// the four known-unknowns of the DLL flip were all sitting at the END of the critical path -
// sanitizers + an instrumented DLL, the real DLL's `shared_ptr` deleter, two static
// mimalloc instances, and a module BMI crossing a target boundary. Measuring them after the
// whole contract migration means one boundary failure invalidates weeks of interface work.
// So the flip's boundary is measured EARLY, in a throwaway build tree
// (`-DDEREN_BACKEND_SPIKE=ON`), and this test is the measurement.
//
// It is deliberately NOT part of the ctest set: it loads a DLL and (optionally) creates a
// real device, which the headless CI set cannot do, and `tests/test_docs.cpp` compares
// `VR_TEST_TARGETS` against the workflow - a GPU test in that list would be a lie in CI.
//
// WHAT IT MEASURES, ONE CHECK PER QUESTION:
//
//   Q5 dependency closure  `load()` succeeds on an ABSOLUTE path, which on Windows uses
//                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
//                          and therefore NEVER searches %PATH% (utility/dynamic_link.cppm:19-20).
//                          The real backend imports `vulkan-1.dll`, `libc++.dll`, USER32/GDI32
//                          and the statically linked glfw's system libraries; the probe DLL
//                          imported only KERNEL32/libc++/UCRT, so THIS is the new information.
//   Q6 CMake file set      the spike tree builds at all: `deren_vulkan` is a SHARED library
//                          with a PUBLIC `FILE_SET CXX_MODULES`, and this test consumes the
//                          CONTRACT (from the STATIC `promise` target) without consuming the
//                          backend's module. Whether those two facts can coexist is the
//                          question; a configure/build failure is the answer.
//   Q4 BMI across target   `deren_abi_version()` resolved from the DLL answers the number the
//                          TEST compiled from its own copy of the contract module - the
//                          "both sides compile the contract" rule, checked across a real
//                          image boundary. With `--with-device`, virtual calls on an object
//                          CONSTRUCTED INSIDE the DLL dispatch through the exe's own vtable
//                          copy, which is the stronger form of the same question.
//   Q2 the deleter         with `--with-device`: the `shared_ptr` is built with the deleter
//                          RESOLVED FROM THE DLL, so the `delete` runs inside the library that
//                          allocated the object. The static half of the probe proved the shape
//                          against a probe; this proves it against `deren::vulkan::core`.
//   Q1 sanitizers          not observable from inside a single test: it is decided at BUILD
//                          time (an instrumented DLL cannot put a second ASan runtime into the
//                          instrumented process - CMakeLists' probe_backend comment). The
//                          spike tree is configured twice, with and without the sanitizer
//                          flags, and the result is recorded in the spike's own report.
//   Q3 two mimalloc        likewise a property of the LINK, not of a call: measured with
//                          `llvm-nm` over both images (both must contain `mi_malloc`/`mi_free`)
//                          plus the owning-STL audit in scripts/check_backend_boundary.py.
//
// THE SAFE DEFAULT: without `--with-device` this test never creates a window or a device, so
// it runs anywhere and still answers Q4 (weak form), Q5, Q6 and the ABI handshake. With
// `--with-device` it constructs the real context - which creates a real window through the
// backend's own default path - and that is the one part that needs a machine with a display
// and a Vulkan device.
// ============================================================================
#include "vk_test.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

import deren.promise.rhi;
import deren.utility.dynamic_link;

// After the imports it needs: the header names deren::promise::rhi types (see its own note).
#include "../promise/rhi/backend_entry.hpp"

namespace {

    namespace rhi = deren::promise::rhi;

    /// A MARK ON STDERR, UNBUFFERED AND FLUSHED, so a hang can be located.
    ///
    /// This exists because the first version of this spike had none: its stdout went to a redirected
    /// file, `fwrite` to a redirected stream is BLOCK-buffered, and the process was terminated before
    /// any of it was flushed - so a run that had got all the way to a live device left an EMPTY file and
    /// no evidence at all (the backend's own `debug.log` is what eventually showed how far it got).
    /// stderr is unbuffered by default and the `fflush` makes that explicit, so every mark that printed
    /// is a mark that happened.
    void mark(char const* what) {
        std::fprintf(stderr, "spike: %s\n", what);
        std::fflush(stderr);
    }

    // From CMake ($<TARGET_FILE:deren_vulkan>): the ABSOLUTE path, because the absolute branch is
    // the one whose search flags the plan depends on (a bare name would take the classic order,
    // which does search %PATH% - a different question).
    constexpr std::string_view spike_dll_path = VR_SPIKE_BACKEND_DLL;

    using make_core_fn = rhi::api_core* (*)(std::uint32_t, rhi::create_info const*, rhi::error_info*);
    using destroy_core_fn = void (*)(rhi::api_core*);

    /** @brief a resolved C symbol: `void const*` to function pointer, constness dropped on purpose */
    template <typename function>
    function as_function(void const* address) {
        return reinterpret_cast<function>(const_cast<void*>(address));
    }

    /**
     * @brief a command list the REAL backend never handed out - the foreign-list refusal's subject
     *
     * abi 14 put `submit(command_list&)` on the contract, and with it the question "is this list
     * mine?". Without RTTI the backend cannot check the dynamic type, so the contract's precondition
     * is the caller's, and a backend that cannot recognise the list must refuse it BY NAME rather
     * than guess. This stand-in is the "not mine" half of that pair: it answers `invalid_argument`
     * to everything, so a backend that accepted it would be caught here. It lives in the SPIKE,
     * because "not handed out by the backend" is what this side knows and the DLL cannot.
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
     * @brief a command buffer the REAL backend never handed out (abi 15's provenance rule)
     *
     * `command_buffer::execute()` must tell a buffer this backend made from a pointer a caller holds;
     * without RTTI the contract's precondition is the caller's, and a backend that cannot recognise
     * the buffer must refuse it BY NAME (`invalid_argument`) rather than cast an unknown pointer. This
     * stand-in is the "not mine" case, and it lives in the SPIKE because "not handed out by the
     * backend" is what this side knows and the DLL cannot.
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

    /// Q4 (weak form) + the ABI handshake, driven through the symbols the DLL itself exports.
    void check_the_handshake(make_core_fn make_core) {
        // A mismatched ABI is refused BEFORE any object exists, and the refusal is a value, not a
        // crash and not an exception (§4.2). The wrong number is deliberately the right one plus
        // one: it is the shape a stale engine would present. The abi 13 channel reports the WHOLE
        // diagnostic, so the assertions below read the fields a bare `error` could not carry. (abi 14
        // did not change this structure, and neither did abi 15: the frame verbs - `api_core::submit()` /
        // `frame_swapchain()`, `swapchain::recreate()` / `extent()`, the two timing verbs on
        // `command_list` - were APPENDED, and abi 15 appended the owned `command_buffer` interface plus
        // `api_core::create_command_buffer()`, with `present()`'s return changing from void to error in
        // 14 - exactly the vtable case the number exists for; the number itself is compared symbolically
        // below, never spelled here, so a renumbering cannot silently pass this file.)
        rhi::create_info const creation{};
        rhi::error_info status{};
        rhi::api_core* const refused = make_core(rhi::abi_version + 1u, &creation, &status);
        CHECK(refused == nullptr);
        CHECK(status.code == rhi::error::abi_mismatch);
        CHECK(status.native_code == 0); // a handshake refusal is its own class: no underlying VkResult
        CHECK_MSG(!status.message.empty(), "the refusal carries static text naming what disagreed");

        // A refused call still ends up inside a `shared_ptr` in the engine's own code, which is why
        // the deleter tolerates nullptr; `check_the_real_context` below covers the other half.
        CHECK(status.code != rhi::error::ok);

        // AND NO CREATION DESCRIPTOR IS A NAMED REFUSAL, not a defaulted context: `create_info{}` is
        // how the standard context is spelled (backend_entry.hpp), so a null pointer is a caller bug
        // that the REAL backend reports with a code rather than papering over.
        rhi::error_info missing{};
        CHECK(make_core(rhi::abi_version, nullptr, &missing) == nullptr);
        CHECK_MSG(missing.code == rhi::error::invalid_argument, "the real backend refuses a null create_info by name");
    }

    /// Q2 + Q4 (strong form): the real context, its virtuals, and its destruction inside the DLL.
    void check_the_real_context(make_core_fn make_core, destroy_core_fn destroy_core) {
        // THE CONTRACT'S ONE CREATION STRUCTURE, filled here the way the application fills it at
        // startup (main.cpp) - the same type, and the same call shape, the flip will use.
        //
        // `window_visible = false` IS A MEASURED REQUIREMENT, NOT A PREFERENCE: a VISIBLE window does not
        // come up in this session at all (this spike's first version hung with 0.5 s of CPU and no window
        // ever appeared), while the HIDDEN path runs to completion - which is exactly what the
        // application's own scripted capture does (`main.cpp` sets
        // `core_options.window_visible = capture.frames == 0`, so a capture gets no visible window). The
        // pixels, the handles and the lifetime ordering are identical either way, so hiding it costs this
        // measurement nothing; it only stops the spike depending on a window station that can map a
        // visible window.
        rhi::create_info creation{};
        creation.window_width = 640;
        creation.window_height = 480;
        creation.window_visible = false;

        mark("calling deren_make_api_core (instance/device/swapchain/heap are built in here)");
        rhi::error_info status{};
        rhi::api_core* const raw = make_core(rhi::abi_version, &creation, &status);
        mark("deren_make_api_core returned");
        CHECK(status.code == rhi::error::ok);
        CHECK(raw != nullptr);
        if (raw == nullptr) {
            return; // the backend refuses by returning null; the startup diagnosis is its own
        }

        // THE DELETER IS THE DLL'S OWN SYMBOL, not the one the linker would have given this test:
        // an object allocated inside the library must be freed inside it (plan §4.1 item 3). The
        // `shared_ptr` lives in an inner scope ON PURPOSE, so the destroy runs while the library is
        // still loaded - the ordering the engine has to reproduce with its own member order.
        {
            std::shared_ptr<rhi::api_core> core{raw, destroy_core};

            // A virtual call on an object CONSTRUCTED INSIDE THE DLL, dispatching through the
            // vtable compiled into this executable: the cross-image form of "both sides compile the
            // contract" (Q4).
            mark("calling abilities() across the image boundary");
            rhi::ability_bits const abilities = core->abilities();
            CHECK(rhi::has_ability(abilities, rhi::extension_kind::vulkan_escape));
            // THE SECOND BIT, ANNOUNCED ONLY ONCE BUFFERS BECAME PRODUCTIBLE (abi 5 moved the
            // acceleration-structure half of this ability to `ray_tracing`, which is the only ability
            // that can hand out that operand - so this bit stopped being hostage to a resource no
            // backend could make).
            CHECK(rhi::has_ability(abilities, rhi::extension_kind::device_address));

            mark("calling query_extension(vulkan_escape)");
            rhi::extension* const escape = core->query_extension(rhi::extension_kind::vulkan_escape);
            CHECK(escape != nullptr);
            if (escape != nullptr) {
                CHECK(escape->kind() == rhi::extension_kind::vulkan_escape);

                // The escape's whole promise is that these are the backend's LIVE handles, so a
                // non-null answer is the check that the ability is servable rather than announced.
                auto* const vulkan = static_cast<rhi::vulkan_escape*>(escape);
                CHECK(vulkan->native_instance() != nullptr);
                CHECK(vulkan->native_device() != nullptr);
                CHECK(vulkan->native_queue() != nullptr);

                // The escape does NOT hand out proc addresses (deliberately, §3.4): the front end
                // resolves `vkGetDeviceProcAddr` itself. The absence is part of the measured shape,
                // so the plan's own gap - "where does the front end get that function pointer" - is
                // recorded rather than rediscovered later.
            }

            // The resource factories are the S3 design surface and answer nullptr today; what this
            // ---- THE FIRST OWNED RESOURCE ACROSS THE CONTRACT -------------------------------------
            // `create_buffer` was the S3 gate ("it must answer null, not crash") until step 2a gave the
            // descriptor a shape and the backend a real implementation. What is checked now is the
            // whole owned-handle path on a REAL allocation: the factory builds the buffer inside the
            // DLL, the descriptor's size and its HOST-VISIBILITY decide what `mapped()` answers, and the
            // caller's single `release()` gives the reference back to the allocator (which is the
            // decrement `free_buffer` performs - release, not necessarily destruction).
            //
            // THE INITIAL BYTES ARE IN THE DESCRIPTOR rather than a second upload call: a backend that
            // keys resources on CONTENT can only recognise identical content if it is handed the content
            // at creation (rhi.api_core.cppm's `buffer_desc` note), so this is the shape that keeps the
            // renderer's deduplication alive across the boundary.
            std::array<std::byte, 64> initial_bytes{};
            for (std::size_t index = 0; index < initial_bytes.size(); ++index) {
                initial_bytes[index] = static_cast<std::byte>(index);
            }

            mark("create_buffer: a host-visible storage buffer, 64 B with initial contents");
            rhi::buffer_desc host_visible{};
            host_visible.size = 64u;
            host_visible.usage = rhi::buffer_usage::storage_coherent;
            host_visible.flags = rhi::to_bits(rhi::buffer_flag::device_address);
            host_visible.initial_bytes = std::span<std::byte const>(initial_bytes.data(), initial_bytes.size());

            rhi::object_manager<rhi::buffer> owned{core->create_buffer(host_visible)};
            CHECK(static_cast<bool>(owned));
            if (owned) {
                CHECK(owned->size() == 64u);
                // A HOST-VISIBLE buffer answers with the bytes the caller may write; an EMPTY span is the
                // contract's spelling of "this one is not host-visible" (see the type's note).
                CHECK(!owned->mapped().empty());
                CHECK(owned->mapped().size() == 64u);
            }

            mark("create_buffer: a GPU-ONLY buffer, allocate-only, must not be host-visible");
            rhi::buffer_desc gpu_only{};
            gpu_only.size = 256u;
            gpu_only.usage = rhi::buffer_usage::storage_gpu_only;
            rhi::object_manager<rhi::buffer> device_local{core->create_buffer(gpu_only)};
            CHECK(static_cast<bool>(device_local));
            if (device_local) {
                CHECK(device_local->size() == 256u);
                CHECK(device_local->mapped().empty());
            }

            mark("create_buffer: a zero-byte descriptor is refused by name-less nullptr (no buffer asked for)");
            rhi::buffer_desc nothing{};
            CHECK(core->create_buffer(nothing) == nullptr);

            // ---- THE device_address ABILITY, ON A REAL ALLOCATION ---------------------------------
            // The descriptor asked for `buffer_flag::device_address`, so the ability has to answer with
            // the buffer's real device address - and `vkGetBufferDeviceAddress` is only defined for a
            // buffer created with that usage, which is also why the ABILITY answers 0 for one created
            // without it. Both halves are checked here: the bit is a promise about service, and the
            // service is what these two lines measure.
            mark("query_extension(device_address) and buffer_address() on both buffers");
            rhi::extension* const address_extension = core->query_extension(rhi::extension_kind::device_address);
            CHECK(address_extension != nullptr);
            if (address_extension != nullptr) {
                CHECK(address_extension->kind() == rhi::extension_kind::device_address);
                auto* const addresses = static_cast<rhi::device_address*>(address_extension);
                if (owned) {
                    // the addressable one: a real address, and the offset is carried through
                    std::uint64_t const base = addresses->buffer_address(*owned, 0u);
                    CHECK(base != 0u);
                    CHECK(addresses->buffer_address(*owned, 16u) == base + 16u);
                }
                if (device_local) {
                    // the one that asked for no address: 0, not a guess
                    CHECK(addresses->buffer_address(*device_local, 0u) == 0u);
                }

                // ---- THE ESCAPE'S native_buffer(), WHICH IS WHAT MAKES THE ENGINE MIGRATION POSSIBLE
                // The raw Vulkan calls that take a buffer (`vkCmdBindVertexBuffers`, `VkDescriptorBufferInfo`,
                // an acceleration structure's build geometry) need the handle itself, and a device address
                // cannot stand in for it. This is the call that lets those sites stop reading the
                // allocator's detail map - so the two buffers must answer with two DISTINCT non-null
                // handles, or the engine would be handed the same VkBuffer for both.
                //
                // The escape object is fetched again here because the first one is scoped to its own
                // block above; both fetches answer the same member, so this is one object, not two.
                rhi::extension* const escape_extension = core->query_extension(rhi::extension_kind::vulkan_escape);
                auto* const escape_native = escape_extension != nullptr ? static_cast<rhi::vulkan_escape*>(escape_extension) : nullptr;
                if (owned && device_local && escape_native != nullptr) {
                    void* const native_host = escape_native->native_buffer(*owned);
                    void* const native_device = escape_native->native_buffer(*device_local);
                    CHECK(native_host != nullptr);
                    CHECK(native_device != nullptr);
                    CHECK(native_host != native_device);
                }
            }

            // ---- abi 16: THE SAMPLER DESCRIPTOR'S APPENDED FIELDS, ON THE REAL DEVICE ----------------
            // The engine creates the renderer's own sampler set now, so these five fields cross for real:
            // NEAREST minification, NEAREST mipmapping and a `less_or_equal` comparison have to survive the
            // trip, or the samplers behind the captures are not the samplers the renderer asked for. A
            // descriptor passed by `const&` that is laid out differently on the two sides fails SILENTLY,
            // which is why this is measured against the backend instead of asserted in a comment: the two
            // descriptions must answer with two DISTINCT non-null native handles, exactly like the buffers.
            mark("create_sampler: nearest/nearest/nearest + less_or_equal, and the defaults beside it");
            rhi::sampler_desc compare_desc{};
            compare_desc.address_mode = rhi::sampler_address_mode::clamp_to_edge;
            compare_desc.max_lod = 0.0f;
            compare_desc.mag_filter = rhi::sampler_filter::linear;
            compare_desc.min_filter = rhi::sampler_filter::nearest;
            compare_desc.mipmap_mode = rhi::sampler_mipmap_mode::nearest;
            compare_desc.compare_enable = true;
            compare_desc.compare_op = rhi::sampler_compare_op::less_or_equal;
            rhi::sampler_desc default_desc{};
            default_desc.address_mode = rhi::sampler_address_mode::repeat;
            default_desc.max_lod = 12.0f;
            rhi::object_manager<rhi::sampler> compare_sampler{core->create_sampler(compare_desc)};
            rhi::object_manager<rhi::sampler> default_sampler{core->create_sampler(default_desc)};
            CHECK(static_cast<bool>(compare_sampler));
            CHECK(static_cast<bool>(default_sampler));
            if (compare_sampler && default_sampler) {
                rhi::extension* const sampler_escape_extension = core->query_extension(rhi::extension_kind::vulkan_escape);
                auto* const sampler_escape = sampler_escape_extension != nullptr ? static_cast<rhi::vulkan_escape*>(sampler_escape_extension) : nullptr;
                CHECK(sampler_escape != nullptr);
                if (sampler_escape != nullptr) {
                    void* const compare_native = sampler_escape->native_sampler(*compare_sampler);
                    void* const default_native = sampler_escape->native_sampler(*default_sampler);
                    CHECK(compare_native != nullptr);
                    CHECK(default_native != nullptr);
                    CHECK(compare_native != default_native);
                }
            }

            // frame_image() before any acquire is nullptr by contract; the frame verbs refuse rather
            // than record nonsense. Driving them here would need a swapchain acquisition.

            // ---- THE IMAGE FACE (abi 7, §17's design), ON THE REAL DEVICE --------------------------
            // create_image with CONTENT (the contract's dedup-visible path), a view of the whole image,
            // a view of ONE mip/layer range, the refusals, and the escape's borrowed handles. This is
            // the section §17's landing order asks for before any engine call site moves: real image,
            // real views, release, on hardware.
            mark("create_image: 4x4 rgba8 with initial content, sampled + transfer destination");
            rhi::image_desc image_desc{};
            image_desc.extent = rhi::image_extent{.width = 4u, .height = 4u, .depth = 1u};
            image_desc.format = rhi::image_format::rgba8_unorm;
            image_desc.flags = rhi::to_bits(rhi::image_flag::sampled) | rhi::to_bits(rhi::image_flag::transfer_destination);
            image_desc.debug_name = "spike image";
            std::array<std::byte, 4u * 4u * 4u> texels{};
            for (std::size_t index = 0; index < texels.size(); ++index) {
                texels[index] = static_cast<std::byte>(index & 0xFFu);
            }
            image_desc.initial_bytes = std::span<std::byte const>(texels.data(), texels.size());

            rhi::object_manager<rhi::image> image_handle{core->create_image(image_desc)};
            CHECK(static_cast<bool>(image_handle));
            if (image_handle) {
                CHECK(image_handle->extent().width == 4u);
                CHECK(image_handle->extent().height == 4u);
                CHECK(image_handle->format() == rhi::image_format::rgba8_unorm);

                mark("make_view: the whole image, one mip range, then two refusals");
                rhi::image_view_desc whole{};
                whole.layer_count = 0; // "all remaining layers"
                whole.mip_count = 0;   // "all remaining mips"
                rhi::object_manager<rhi::image_view> full_view{image_handle->make_view(whole)};
                CHECK(static_cast<bool>(full_view));

                rhi::image_view_desc range{};
                range.base_layer = 0u;
                range.layer_count = 1u;
                range.base_mip = 0u;
                range.mip_count = 1u;
                rhi::object_manager<rhi::image_view> layer_view{image_handle->make_view(range)};
                CHECK(static_cast<bool>(layer_view));

                rhi::image_view_desc bad_layer{};
                bad_layer.base_layer = 4u; // the image has one layer
                CHECK(image_handle->make_view(bad_layer) == nullptr);
                rhi::image_view_desc bad_mip{};
                bad_mip.base_mip = 1u; // the image has one mip
                CHECK(image_handle->make_view(bad_mip) == nullptr);

                mark("escape: native_image / native_image_view / native_sampler on real objects");
                rhi::extension* const image_escape = core->query_extension(rhi::extension_kind::vulkan_escape);
                if (image_escape != nullptr && full_view) {
                    auto* const natives = static_cast<rhi::vulkan_escape*>(image_escape);
                    CHECK(natives->native_image(*image_handle) != nullptr);
                    CHECK(natives->native_image_view(*full_view) != nullptr);

                    rhi::sampler_desc sampler_desc{};
                    sampler_desc.address_mode = rhi::sampler_address_mode::clamp_to_edge;
                    sampler_desc.max_lod = 1.0f;
                    rhi::object_manager<rhi::sampler> sampler_handle{core->create_sampler(sampler_desc)};
                    CHECK(static_cast<bool>(sampler_handle));
                    if (sampler_handle) {
                        CHECK(natives->native_sampler(*sampler_handle) != nullptr);
                    }
                }
            }

            // ---- THE PIPELINE FACE (abi 8): the refusal paths, on the real device ------------------
            // A real SPIR-V module cannot ride in the spike, so the SUCCESS path is witnessed where
            // the real shaders live - the render gate, once every pass pipeline goes through the
            // factory. What the spike pins here is the refusal discipline: a stage-less/empty module
            // and an unknown-format color attachment answer nullptr, never a half-built object.
            mark("create_shader/create_pipeline: the refusal paths answer nullptr");
            rhi::shader_desc empty_shader{};
            empty_shader.stage = rhi::shader_stage::vertex;
            empty_shader.debug_name = "spike empty shader";
            CHECK(core->create_shader(empty_shader) == nullptr);
            rhi::image_format const bad_formats[] = {rhi::image_format::unknown};
            rhi::pipeline_desc bad_pipeline{};
            bad_pipeline.color_formats = std::span<rhi::image_format const>(bad_formats);
            bad_pipeline.vertex_code = std::span<std::byte const>();
            bad_pipeline.debug_name = "spike unknown-format pipeline";
            CHECK(core->create_pipeline(bad_pipeline) == nullptr);

            // ---- THE FRAME FACE (abi 13): the two borrowed views, on the real device --------------
            // STRUCTURE ONLY, ON PURPOSE: the contract has no submit verb yet, so a frame this test
            // opened could never be handed back - presenting it would wait on a present-ready
            // semaphore nothing signals, and tearing the context down while an image is acquired is
            // invalid. The full wait -> latch -> acquire -> present -> walk loop is witnessed by the
            // engine's own frame path and the render gate (14 scenes, hashes frozen). Two profiler
            // lines the handoff names are machine- or stage-dependent and recorded here as such: the
            // "no-timing device" refusal (this machine times) and the named-stage report after real
            // marks (the engine's frame loop, once it walks the face) are witnessed where they run.
            mark("walk_frames/profiler: the frame face's two borrowed views");
            rhi::frame_walker* const walker = core->walk_frames();
            CHECK(walker != nullptr);
            CHECK(core->walk_frames() == walker); // the same borrowed view every call, never a new object
            CHECK(walker->slot_count() > 0u);
            CHECK(walker->position() < walker->slot_count()); // the cursor, read before any frame
            rhi::gpu_profiler* const profiler = core->profiler();
            CHECK(profiler != nullptr);
            CHECK(core->profiler() == profiler);
            CHECK(profiler->stage_count() == 0u); // nothing has latched: no frame has run in this test
            std::string_view stage_name{};
            std::uint64_t duration_ns = 0;
            CHECK_MSG(profiler->get_stage_info(profiler->stage_count(), &stage_name, &duration_ns) == rhi::error::invalid_argument,
                      "an index at the count is refused by name");
            CHECK_MSG(profiler->get_stage_info(0, &stage_name, &duration_ns) == rhi::error::invalid_argument,
                      "no frame has latched, so even index 0 is out of range on a timing device");

            // ---- THE PRESENTATION SURFACE AND THE FRAME VERBS (abi 14), STRUCTURE ONLY -----------
            // The same posture as the frame face above, for the same measured reason: no frame is
            // acquired in this test, so a verb that needs one can only be measured on its REFUSAL
            // path. (`submit()` is on the contract now, so the ring COULD be driven - but a frame
            // this test opened and did not complete would leave an acquired image and a signalled
            // acquire semaphore at teardown, which is what the frame face's note above records as
            // the reason not to. The full wait -> acquire -> record -> submit -> present -> walk
            // loop is witnessed by the engine's frame path and the 14 frozen render hashes, and
            // abi 14's positional mark rule is witnessed on the probe's device-less list by
            // tests/test_dynamic_link.cpp.)
            mark("frame_swapchain: the presentation surface's borrowed view");
            rhi::swapchain* const surface = core->frame_swapchain();
            CHECK(surface != nullptr);
            CHECK(core->frame_swapchain() == surface); // the same borrowed view every call, never a new object
            rhi::image_extent const output = surface->extent();
            // A real swapchain exists in this context, and a zero-sized one is the deferred state
            // (the constructor's build path never created targets from it), so the extent is real.
            CHECK(output.width > 0u);
            CHECK(output.height > 0u);
            CHECK(surface->extent().width == output.width); // asking twice does not move a generation
            CHECK(surface->extent().height == output.height);

            // THE TIMING VERBS LIVE ON THE LIST, AND WITH NO FRAME IN FLIGHT THERE IS NO LIST: that
            // is the structural fact this context can witness about them - the handle the verbs
            // would be called on does not exist, so an out-of-order mark cannot even be attempted
            // here (its refusal is measured where a list exists, see the note above).
            CHECK(core->begin_commands() == nullptr);

            // SUBMIT: with no frame context the list to hand over does not exist. What this test CAN
            // measure is the other half of the verb's precondition - a list the backend never handed
            // out is refused BY NAME, never accepted and never guessed at.
            foreign_command_list foreign{};
            CHECK_MSG(core->submit(foreign) == rhi::error::invalid_argument,
                      "a command list this backend did not hand out is refused by name");

            // PRESENT: no frame was ever acquired, so there is nothing to show - the answer is
            // `not_ready`, and the backend never reaches vkQueuePresentKHR with a present-ready
            // semaphore nothing has signalled (which is exactly why that guard exists).
            CHECK_MSG(core->present() == rhi::error::not_ready,
                      "present with no acquired frame is refused by name, not run");

            // ---- THE OWNED COMMAND BUFFER (abi 15), ON THE REAL DEVICE ---------------------------
            // This is the one part of abi 15 that CAN be driven without a frame, and it is driven end
            // to end: two buffers are created (each with its own command pool), both are recorded, the
            // primary executes the secondary, and the refusals are measured by name. What a frame would
            // add is submission/presentation, which the engine's frame path and the render gate cover.
            mark("create_command_buffer: an owned primary and secondary, recorded and executed");
            rhi::object_manager<rhi::command_buffer> primary{core->create_command_buffer(rhi::command_buffer_desc{.kind = rhi::command_buffer_kind::primary})};
            rhi::object_manager<rhi::command_buffer> secondary{core->create_command_buffer(rhi::command_buffer_desc{.kind = rhi::command_buffer_kind::secondary})};
            CHECK(static_cast<bool>(primary));
            CHECK(static_cast<bool>(secondary));
            // a kind outside the two roles the contract names is the factory's one refusal
            CHECK(core->create_command_buffer(rhi::command_buffer_desc{.kind = static_cast<rhi::command_buffer_kind>(99u)}) == nullptr);
            if (primary && secondary) {
                CHECK(primary->type() == rhi::command_buffer::interface_id);
                // the borrowed recording view: the same object every call, and NOT the frame's list
                rhi::command_list* const view = primary->recording();
                CHECK(view != nullptr);
                CHECK(primary->recording() == view);
                CHECK_MSG(view->begin_gpu_timing() == rhi::error::not_ready,
                          "the frame-scoped timing verbs refuse a list that is not the frame's");
                CHECK_MSG(view->mark_gpu_timing(0, "not the frame's") == rhi::error::not_ready,
                          "the frame-scoped mark verb refuses the same way");
                // THE NATIVE HANDLE: the escape answers it for a buffer the CALLER owns, outside any
                // frame - which is the read-back's case (its read runs after the frame has landed).
                rhi::extension* const escape_extension = core->query_extension(rhi::extension_kind::vulkan_escape);
                CHECK(escape_extension != nullptr);
                if (escape_extension != nullptr) {
                    auto* const escape = static_cast<rhi::vulkan_escape*>(escape_extension);
                    CHECK_MSG(escape->native_command_buffer(*view) != nullptr,
                              "an owned buffer's raw handle answers through the escape outside a frame");
                }

                // THE SECONDARY, the executable path: recorded with no usage flags (so executing it
                // outside a render pass instance is legal) and ended - only an ended secondary may be
                // executed.
                CHECK(secondary->begin_recording(rhi::command_buffer_begin_info{}) == rhi::error::ok);
                CHECK(secondary->end_recording() == rhi::error::ok);

                // THE CHAIN: a `render_pass_continue` continuation must declare its attachment
                // inheritance, and that rides the tagged structure - the portable begin info stays free
                // of Vulkan's attachment model. Here the inheritance is EMPTY (no colour, no depth) and
                // a chain this backend does not serve is refused BY NAME, never dropped.
                rhi::object_manager<rhi::command_buffer> continuation{core->create_command_buffer(rhi::command_buffer_desc{.kind = rhi::command_buffer_kind::secondary})};
                CHECK(static_cast<bool>(continuation));
                if (continuation) {
                    rhi::vulkan_command_buffer_inheritance_info const inheritance = {};
                    rhi::command_buffer_begin_info const continuation_begin{
                        .usage = rhi::to_bits(rhi::command_buffer_usage::render_pass_continue),
                        .next = &inheritance.header,
                    };
                    CHECK_MSG(continuation->begin_recording(continuation_begin) == rhi::error::ok,
                              "a render_pass_continue begin carries its inheritance through the tagged chain");
                    CHECK(continuation->end_recording() == rhi::error::ok);
                    rhi::heap_bind_info const wrong_chain = {};
                    rhi::command_buffer_begin_info const refused_begin{.usage = 0u, .next = &wrong_chain.header};
                    CHECK_MSG(continuation->begin_recording(refused_begin) == rhi::error::unsupported,
                              "a chain type this backend does not serve is refused by name, not dropped");
                }

                // THE PRIMARY: begun, hands the ended secondary over, ended. The foreign buffer is the
                // provenance refusal (a pointer this backend did not hand out).
                foreign_command_buffer foreign{};
                CHECK(primary->begin_recording({.usage = rhi::to_bits(rhi::command_buffer_usage::one_time_submit)}) == rhi::error::ok);
                CHECK_MSG(primary->execute(foreign) == rhi::error::invalid_argument,
                          "a command buffer this backend did not hand out is refused by name");
                CHECK_MSG(primary->execute(*secondary) == rhi::error::ok,
                          "an ended secondary is executed by the recording primary");
                CHECK(primary->end_recording() == rhi::error::ok);
            }

            mark("about to leave the scope: two releases and the DLL's deleter (core teardown) run next");
        } // <- the managers release their buffers, then the DLL's deleter runs
        mark("core teardown returned");
    }

} // namespace

int main(int argc, char** argv) {
    bool with_device = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view{argv[index]} == "--with-device") {
            with_device = true;
        }
    }
    mark(with_device ? "start (--with-device)" : "start (safe mode)");

    // Q5 + Q6: an ABSOLUTE path through the platform's restricted search flags, on a library that
    // has to satisfy its own dependency closure (vulkan-1.dll, libc++.dll, USER32/GDI32, glfw's
    // system libraries) - the probe DLL had none of those.
    auto loaded_result = deren::utility::dynamic_link::load(std::string{spike_dll_path});
    CHECK(loaded_result.has_value());
    if (!loaded_result.has_value()) {
        deren::vk_test::write_line("spike: load failed for {}: {}", spike_dll_path, loaded_result.error().message);
        return deren::vk_test::finish("spike_backend_boundary");
    }

    // ---- THE LIBRARY IS DETACHED, NEVER UNLOADED, AND THAT IS A MEASUREMENT ---------------------
    // `FreeLibrary` on this backend NEVER RETURNS once it has been initialised. MEASURED, with the
    // stderr marks above: every step of this test completed (a real context built inside the DLL, the
    // contract's virtuals called across the image boundary, the DLL's own deleter run and returned) and
    // then the process sat at 0.45 s of CPU for 80+ s with 10 threads and 76 MB that never moved - the
    // only step left was the loader's destructor calling `FreeLibrary`. So the unload is what hangs,
    // not the test, and it is the DLL's process-detach path that blocks (GLFW is initialised inside it
    // and this backend never calls `glfwTerminate` - `core.constructor.cppm` says so in as many words -
    // so a detach that waits on that state is a deadlock, not a slow teardown).
    //
    // `detach()` IS THE DESIGNED EXIT FOR THIS (`dynamic_link::library`, with its own test in
    // tests/test_dynamic_link.cpp), and the plan's invariant 4 already says the DLL lives to the end of
    // the process - so the product does not unload either. Recorded in DYNAMIC_LINK_V2.md §13, where it
    // is a step-4 item: the engine's loader object must detach rather than let its destructor unload,
    // or the product deadlocks where nothing is watching (the window has already closed).
    auto loaded = std::move(*loaded_result);
    CHECK(loaded.native_handle() != nullptr);

    // Q4 (weak form): the DLL's compiled contract number equals the one this executable compiled.
    auto const version_symbol = loaded.symbol("deren_abi_version");
    CHECK(version_symbol.has_value());
    if (version_symbol.has_value()) {
        auto const abi_version = as_function<std::uint32_t (*)()>(version_symbol.value());
        CHECK(abi_version() == rhi::abi_version);
        deren::vk_test::write_line("spike: deren_abi_version() = {} (this executable compiled {})", abi_version(), rhi::abi_version);
    }

    auto const make_symbol = loaded.symbol("deren_make_api_core");
    auto const destroy_symbol = loaded.symbol("deren_destroy_api_core");
    CHECK(make_symbol.has_value());
    CHECK(destroy_symbol.has_value());
    if (make_symbol.has_value() && destroy_symbol.has_value()) {
        make_core_fn const make_core = as_function<make_core_fn>(make_symbol.value());
        destroy_core_fn const destroy_core = as_function<destroy_core_fn>(destroy_symbol.value());

        check_the_handshake(make_core);
        if (with_device) {
            check_the_real_context(make_core, destroy_core);
        } else {
            deren::vk_test::write_line("spike: device path skipped (pass --with-device to build a real context)");
        }
    }

    // THE LAST CALL ON THE LOADER: hand the handle back so the destructor cannot unload (see the note).
    static_cast<void>(loaded.detach());
    mark("all checks done; the library was DETACHED (never unloaded) - see the note");
    return deren::vk_test::finish("spike_backend_boundary");
}
