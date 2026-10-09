// -*- C++ -*-
// ============================================================================
// file: tests/probe_backend.cpp
//
// The probe backend of tests/test_dynamic_link.cpp: the smallest stand-in for the
// real promise/rhi/ contract (the module deren.promise.rhi, RHI plan v4 §3.3 - §3.5, §4.1).
// CMake builds this one source file twice - as a DLL (with DEREN_API_SHARED +
// DEREN_API_BUILD) and as a static library (with neither) - and the test links the
// static half while opening the DLL half at run time, so both branches of
// utility/abi_export.hpp are exercised and the two halves must answer the same.
//
// This file imports the contract and includes the ABI surface instead of declaring
// either itself (it used to declare the C ABI; the declarations live in
// promise/rhi/backend_entry.hpp now), which is also the check that the header can be
// compiled by the backend side: the three entry points below are EXACTLY the ones
// the header declares, and deriving from the module's abstract classes is what makes
// the compiler verify that every virtual is implemented.
//
// What the probe implements, and why it is this small:
//
//   - `api_core::abilities()` announces device_address, and
//     `query_extension()` answers it and returns nullptr for anything else.
//     ABI12 removed the old inert descriptor_heap: this probe has no recording DOMAIN (abi 14's
//     recording VIEW is a static list it hands out - see the last bullet).
//     That pair is what the plan's "named failure, no silent downgrade" rests on
//     (§1.9, §3.6): an engine that asks for ray_tracing gets a null, not a stub.
//   - The factories return nullptr except `create_buffer()`: a probe has no device,
//     so it has nothing to hand out for the rest.
//   - `create_buffer()` hands out a STATICALLY allocated probe buffer and stores the
//     descriptor's size in it. That is what makes the tier-2 path testable without
//     answering the ownership question: `device_address::buffer_address()` is asked
//     about that very object, so the test sees a POD descriptor cross into the DLL,
//     a virtual call come back out, and both halves agree. Static storage also means
//     the engine has nothing to free - releasing a resource through the backend
//     (plan §4.2) is S1's, and this round does not pretend to answer it.
//   - `probe_buffer::release()` completes the owned-handle path the contract now spells
//     (`rhi::buffer::release()` + `rhi::object_manager<T>`): there is no reference count to drop
//     on a static buffer, but the CALL has to arrive, so `release()` zeroes the size the
//     descriptor wrote. The test drives it through `object_manager` and reads `size()`
//     back - the same round-trip shape as the descriptor echo below.
//   - `frame_begin()` counts frames AND echoes the creation descriptor's `window_width`
//     in its by-value POD result, so a cross-boundary virtual call with a by-value POD
//     result is observable too - and so is the one structure `deren_make_api_core()`
//     takes: the test fills `create_info`, the probe carries the field in and hands it
//     back, and a request that never reached the library cannot produce the number.
//   - `deren_make_api_core()` takes the contract's creation descriptor
//     (`rhi::create_info const*`), refuses a mismatched ABI with `abi_mismatch` and a
//     missing descriptor with `invalid_argument`, and allocates the object with `new`
//     so that the deleter the host resolved from THIS library is the one that frees it.
//   - THE abi 14 SURFACE is implemented for the same compile-time reason every other virtual is
//     (a pure virtual left unimplemented is an abstract class, not a probe): the two `swapchain`
//     verbs on a STATIC presentation view (`extent()` echoes the creation descriptor's window size,
//     `recreate()` answers the deferred state for a zero one), the two timing verbs on a STATIC
//     recording list (`begin_commands()` hands it out while a frame is open; the positional rule
//     answers `invalid_argument`, the capability answers `unsupported` exactly as the profiler face
//     does), `frame_swapchain()` and `submit()` on the context, and `present()` answering `error`
//     instead of nothing. Every one of them refuses by NAME where it cannot serve - nothing here
//     silently downgrades, which is what makes the double (DLL + static) build a contract check.
//   - THE abi 15 SURFACE is one more pure virtual on `api_core`: `create_command_buffer()`, answered
//     with a STATIC owned stand-in whose echo is the contract's whole lifecycle - the descriptor's
//     kind goes in, `begin_recording()`'s usage flags are remembered, `execute()` tells its own buffer
//     from a foreign one, and `release()` drops the one reference so every later verb answers
//     `not_ready`. abi 21 appended `make_command_buffer()`, answered with the SAME stand-in behind a
//     NO-OP deleter: the object is a static member, so the control block must not delete it (the
//     contract's own `release()` is what a caller observes instead).
// ============================================================================
import deren.promise.rhi;

#include "../promise/rhi/backend_entry.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected> // std::expected: the contract's `image::get_content()` (abi 25) answers one
#include <source_location>
#include <span>
#include <string_view>
#include <vector>

namespace {

    namespace rhi = deren::promise::rhi;

    struct impl; // the walker back-references it; defined below with the rest of the probe

    /// The probe's buffer: a real object, statically allocated (see the banner).
    struct probe_buffer final : rhi::buffer {
        /// THE PROBE'S `release()` IS OBSERVABLE ON PURPOSE, which is what makes the owned-handle path
        /// testable at all: the buffer is statically allocated, so there is no reference count to drop,
        /// but the call still has to ARRIVE (the test releases through `object_manager`, then reads
        /// `size()` - a released probe buffer answers 0, so a `release()` that never crossed cannot pass
        /// for one that did). `size()` is the channel because it is already this object's observable
        /// field: the `create_buffer` descriptor's size goes in, `release()` takes it away.
        void release() noexcept override {
            this->released = true;
            this->size_bytes = 0;
        }

        [[nodiscard]] std::uint64_t size() const noexcept override {
            return this->size_bytes;
        }

        /// The probe's buffer is not host-visible, and the contract's answer for that is an EMPTY span
        /// rather than a promise it cannot keep (only a host-visible buffer returns bytes).
        [[nodiscard]] std::span<std::byte> mapped() noexcept override {
            return {};
        }

        std::uint64_t size_bytes = 0;
        bool released = false; ///< set by release(); the test reads it through size() == 0
    };

    /// THE BORROWED FRAME IMAGE AND THE OWNED VIEW IT HANDS OUT (③-D/E item B, abi 16). `frame_image()`
    /// answers a BORROWED view in the real backend, but a view MADE FROM one is a new owned object whose
    /// `release()` is real - and that asymmetry is exactly what a caller can get wrong, so the probe mirrors
    /// it: the image's own `release()` is the borrowed no-op, the view's is observable, and a range outside
    /// the single-layer/single-mip shape is refused by name (nullptr), never clamped.
    struct probe_image_view final : rhi::image_view {
        void release() noexcept override {
            this->released = true;
        }

        bool released = false;
    };

    struct probe_frame_image final : rhi::image {
        void release() noexcept override {
            // BORROWED: the real backend logs once and releases nothing (the engine owns no reference to the
            // swapchain image). The probe's observable channel is an echo instead of a log, so a test can tell
            // "the borrowed release was called" from "someone released the image itself".
            ++this->borrowed_releases;
        }

        [[nodiscard]] rhi::image_extent extent() const noexcept override {
            return {.width = 4, .height = 4, .depth = 1};
        }

        [[nodiscard]] rhi::image_format format() const noexcept override {
            return rhi::image_format::bgra8_srgb;
        }

        /// abi 25: the read-back. The probe's frame image stands in for a BORROWED swapchain image, which the
        /// real backend also refuses with `unsupported` (its surface lists no HOST_TRANSFER) - so the probe
        /// answers the same named error rather than inventing bytes a GPU never wrote.
        [[nodiscard]] std::expected<rhi::image_content, rhi::error> get_content(rhi::image_copy_region const&) const override {
            return std::unexpected(rhi::error::unsupported);
        }

        [[nodiscard]] rhi::image_view* make_view(rhi::image_view_desc const& desc) override {
            uint32_t const layers = desc.layer_count == 0 ? 1u : desc.layer_count;
            uint32_t const mips = desc.mip_count == 0 ? 1u : desc.mip_count;
            if (desc.base_layer != 0u || layers != 1u || desc.base_mip != 0u || mips != 1u) {
                return nullptr; // a swapchain image is one layer, one mip: refused, not clamped
            }
            this->view.released = false;
            return &this->view;
        }

        probe_image_view view{};
        uint32_t borrowed_releases = 0;
    };

    /// THE SAMPLER STAND-IN, AND WHY IT ECHOES ITS DESCRIPTOR (abi 16): `sampler_desc` gained five fields
    /// in that batch, and a DESCRIPTOR that travels by `const&` across the boundary is exactly the sort of
    /// thing a compiler cannot tell you was mis-laid-out - the backend would simply read different bytes.
    /// So the probe stores what it was handed, and `test_dynamic_link` asserts each new field arrived with
    /// the value the caller spelled. `release()` is observable for the same reason `probe_buffer`'s is: the
    /// object is statically allocated, so the call has to ARRIVE.
    struct probe_sampler final : rhi::sampler {
        void release() noexcept override {
            this->released = true;
            this->seen = {};
        }

        rhi::sampler_desc seen = {}; ///< the descriptor `create_sampler` was handed, verbatim
        bool released = false;
    };

    /// device_address, implemented over that buffer: the answer depends on the size
    /// the descriptor carried, so the test can tell "the ability was called about
    /// this object" from "the ability was called about something else".
    ///
    /// IT HAS ONE METHOD (buffer addresses) since abi 5: the acceleration-structure half moved to
    /// `ray_tracing`, which is the only ability that can hand out that operand.
    struct probe_device_address final : rhi::device_address {

        [[nodiscard]] std::uint64_t buffer_address(rhi::buffer const& resource, std::uint64_t offset) const noexcept override {
            return address_base + resource.size() + offset;
        }

        static constexpr std::uint64_t address_base = 0x1000ull;
    };

    /// THE PROBE'S ESCAPE (abi 19's injection test needs it): the engine's runtime asks the escape
    /// for facts it cannot get anywhere else - the presentation image's FORMAT before a frame is
    /// acquired (`native_swapchain_image_format`), the enabled device extensions (the backend's own
    /// gate for ray query / mesh shaders), and the raw handles the frame path borrows.
    ///
    /// THE HANDLES ARE NULL AND THAT IS THE HONEST ANSWER: a probe has no instance, device or queue,
    /// and every consumer of them in the engine guards the null (see `ray_query_available_of` in
    /// runtime.constructor.cppm, which answers `false` rather than dereferencing). The EXTENSION LIST
    /// is empty for the same reason, so both device-capability derivations answer "not available" -
    /// which is what a device-less probe can honestly claim. Broadcasting `vulkan_escape` alongside
    /// `device_address` is what makes the probe usable as an injected root at all.
    struct probe_escape final : rhi::vulkan_escape {
        [[nodiscard]] void* native_instance() const noexcept override {
            return nullptr;
        }
        [[nodiscard]] void* native_physical_device() const noexcept override {
            return nullptr;
        }
        [[nodiscard]] void* native_device() const noexcept override {
            return nullptr;
        }
        [[nodiscard]] void* native_queue() const noexcept override {
            return nullptr;
        }
        [[nodiscard]] void* native_command_buffer(rhi::command_buffer&) const noexcept override {
            return nullptr;
        }
        [[nodiscard]] std::span<char const* const> enabled_instance_extensions() const noexcept override {
            return {};
        }
        [[nodiscard]] std::span<char const* const> enabled_device_extensions() const noexcept override {
            return {};
        }
        [[nodiscard]] void* native_buffer(rhi::buffer const&) const noexcept override {
            return nullptr;
        }
        [[nodiscard]] void* native_image(rhi::image const&) const noexcept override {
            return nullptr;
        }
        [[nodiscard]] void* native_image_view(rhi::image_view const&) const noexcept override {
            return nullptr;
        }
        [[nodiscard]] void* native_sampler(rhi::sampler const&) const noexcept override {
            return nullptr;
        }
        [[nodiscard]] std::uint32_t native_image_format(rhi::image const&) const noexcept override {
            return static_cast<std::uint32_t>(probe_image_format);
        }
        [[nodiscard]] void* native_pipeline(rhi::pipeline const&) const noexcept override {
            return nullptr;
        }
        [[nodiscard]] void* native_shader_module(rhi::shader const&) const noexcept override {
            return nullptr;
        }
        /// THE ONE NON-NULL ANSWER, and the reason the engine's runtime can ask this at all before a
        /// frame exists (abi 17): the presentation image's format is a session-stable fact, and the
        /// probe has to name SOMETHING for it. `VK_FORMAT_B8G8R8A8_UNORM` = 44 is the value a Vulkan
        /// backend would report for the common swapchain; the number is not a promise about pixels.
        [[nodiscard]] std::uint32_t native_swapchain_image_format() const noexcept override {
            return static_cast<std::uint32_t>(probe_image_format);
        }
        /// abi 22's BASIC-HANDLE slots, answered the way a DEVICE-LESS probe must: it has no basis to hand out
        /// (null), and the method that takes one writes no handle. A pass that reaches it through this face
        /// therefore takes its own documented "unavailable" path instead of recording against a device that is not
        /// there, which is exactly what the probe is for: the boundary, witnessed with no GPU behind it.
        /// (abi 24 removed the `device_proc` slot this class used to answer with "no entry point".)
        [[nodiscard]] rhi::api_basis* get_basis() const noexcept override {
            return nullptr;
        }
        // abi 24: `device_proc` was REMOVED from `vulkan_escape` (the ray-tracing launch resolves its entry point
        // in the backend now), so the probe has no override for it - and it must not keep one, because the point
        // of this stand-in is that it answers exactly the contract's slots.
        [[nodiscard]] bool shader_group_handles(rhi::api_basis&, rhi::pipeline const&, std::uint32_t, std::uint32_t, std::span<std::uint8_t>) const noexcept override {
            return false;
        }

        /// B8G8R8A8_UNORM, as the contract's `std::uint32_t` spelling of a VkFormat.
        static constexpr int probe_image_format = 44;
    };

    /// The probe's frame ring: two virtual slots, one cursor, and an open that echoes the creation
    /// descriptor - the frame face's shape (abi 13), witnessed without a device. The ring is the ONE
    /// cursor: `frame_begin()` reports the slot `position()` names, so the "two sources of truth"
    /// shape the face exists to end cannot exist even here. (The bodies live below `impl`, which
    /// they reach through.)
    struct probe_frame_walker final : rhi::frame_walker {
        impl* owner = nullptr;

        [[nodiscard]] std::uint32_t slot_count() const noexcept override;
        [[nodiscard]] std::uint32_t position() const noexcept override;
        [[nodiscard]] rhi::frame_open_info wait_and_acquire() override;
        void walk_to_next() noexcept override;
    };

    /// The probe cannot timestamp anything, which is exactly the profiler face's `unsupported` story:
    /// a constant zero and a named refusal - never silence, never a fake measurement.
    struct probe_gpu_profiler final : rhi::gpu_profiler {

        [[nodiscard]] std::uint32_t stage_count() const noexcept override {
            return 0u;
        }

        [[nodiscard]] rhi::error get_stage_info(std::uint32_t, std::string_view*, std::uint64_t*) const noexcept override {
            return rhi::error::unsupported;
        }
    };

    /// THE PROBE'S ONE RECORDING FACE (abi 14/15/20): `command_list` is DELETED, so this single
    /// statically allocated object is BOTH the frame's borrowed command buffer that `begin_commands()`
    /// hands out for as long as the probe's frame fiction is open, AND the owned buffer
    /// `create_command_buffer()` hands out. It is the object the two TIMING verbs live on, and the
    /// probe's device cannot timestamp - which is why their honest answer is `unsupported` (the
    /// profiler face's story) - while the ORDER rule stays observable: `mark_gpu_timing()` checks the
    /// positional index BEFORE its own capability, so an out-of-order mark is refused with
    /// `invalid_argument` on this device-less list exactly as the contract requires it to be. (The real
    /// backend checks its capability first; both orders are contract-faithful - the two codes say
    /// different things, "you asked wrong" and "I cannot measure" - and the probe's choice is the one
    /// that makes the positional rule testable without a GPU.)
    ///
    /// THE TWO OBJECTS ARE THE SAME TYPE AND DIFFERENT INSTANCES: `commands` is the frame's
    /// (`frame_scoped == true`), `command_buffer` is the owned one (`frame_scoped == false`), which is
    /// how the frame-scoped verbs tell "the frame's" from "the caller's" exactly as the real backend's
    /// `target != VK_NULL_HANDLE` test does.
    struct probe_command_buffer final : rhi::command_buffer {
        impl* owner = nullptr;
        rhi::command_buffer_kind kind_echo = rhi::command_buffer_kind::primary;
        rhi::command_buffer_flags usage_echo = rhi::no_command_buffer_flags;
        std::uint32_t executions = 0;
        bool released = false;

        void release() noexcept override;
        [[nodiscard]] rhi::error begin_recording(rhi::command_buffer_begin_info const& info) override;
        [[nodiscard]] rhi::error end_recording() noexcept override;
        [[nodiscard]] rhi::error execute(rhi::command_buffer& secondary) override;
        /// WHOSE LIST THIS IS (abi 15): the probe has exactly two kinds of list - the FRAME's
        /// (`begin_commands()`, the abi-13/14 object) and one per owned command buffer - and the
        /// frame-scoped verbs below must tell them apart the way the real backend does.
        bool frame_scoped = true;

        /// The probe records no barrier (it has no device) but the call has to ARRIVE, so the pair it
        /// was handed is echoed into this object - the same "make the crossing visible" shape
        /// `probe_buffer::release()` uses for the owned-handle path.
        [[nodiscard]] rhi::error use(rhi::image const&, rhi::image_use const from, rhi::image_use const to) noexcept override {
            if (!this->frame_scoped) {
                return rhi::error::not_ready; // the frame's image is not in this buffer (the frame-scoped rule)
            }
            this->last_use_from = from;
            this->last_use_to = to;
            return rhi::error::ok;
        }

        /// No device, no destination buffer to copy into: the honest answer is `unsupported`, never a
        /// fake copy (the same rule `probe_buffer::mapped()` follows with its empty span).
        [[nodiscard]] rhi::error copy_image_to_buffer(rhi::buffer&, rhi::image const&, rhi::image_copy_region const&) noexcept override {
            return this->frame_scoped ? rhi::error::unsupported : rhi::error::not_ready;
        }

        /// TIER-1 RECORDING VERBS SINCE ABI 26, AND `unsupported` IS THE HONEST ANSWER HERE: this fake records
        /// into no device, so a build it accepted would be a recorded lie - the same rule the copy above
        /// follows. (No handle can reach these calls while `create_acceleration_structure` answers null, which
        /// is why they are answered rather than exercised.)
        [[nodiscard]] rhi::error build_acceleration_structure(rhi::acceleration_structure&) override {
            return rhi::error::unsupported;
        }
        [[nodiscard]] rhi::error refit_acceleration_structure(rhi::acceleration_structure&) override {
            return rhi::error::unsupported;
        }
        /// abi 27: the micromap build, refused for the same reason as the two above (no device records here).
        [[nodiscard]] rhi::error build_micromap(rhi::micromap&) override {
            return rhi::error::unsupported;
        }

        [[nodiscard]] rhi::error begin_gpu_timing() noexcept override {
            if (!this->frame_scoped) {
                return rhi::error::not_ready; // the timing range is the frame slot's, and this is not it
            }
            this->marks = 0;
            return rhi::error::unsupported;
        }

        [[nodiscard]] rhi::error mark_gpu_timing(std::uint32_t const mark_index, std::string_view const stage_name) noexcept override {
            if (!this->frame_scoped) {
                return rhi::error::not_ready;
            }
            if (mark_index != this->marks) {
                return rhi::error::invalid_argument; // positional: the next mark is the only valid index
            }
            this->last_stage_name = stage_name; // the echo: static text stored verbatim, never copied
            return rhi::error::unsupported;
        }

        // ---- the portable record series (abi 20): the ECHO verbs. Each call arrives, is counted and
        // carries its shape into this object for the test to read (the same "make the crossing
        // visible" shape the members below follow); nothing is faked and nothing is dropped, because
        // a silent verb is exactly what the answering rule exists to refuse.
        [[nodiscard]] rhi::error begin_rendering(rhi::rendering_info const& info) noexcept override {
            this->begun_renderings += 1;
            this->last_layer_count = info.layer_count;
            this->last_secondary_contents = info.secondary_contents;
            return rhi::error::ok;
        }

        void end_rendering() noexcept override {
            this->ended_renderings += 1;
        }

        [[nodiscard]] rhi::error bind_pipeline(rhi::pipeline const& handle) noexcept override {
            this->bound_pipelines += 1;
            this->last_bound_pipeline = &handle;
            return rhi::error::ok;
        }

        [[nodiscard]] rhi::error bind_vertex_buffer(rhi::buffer const& handle, std::uint64_t const offset) noexcept override {
            this->last_bound_vertex = &handle;
            this->last_vertex_offset = offset;
            return rhi::error::ok;
        }

        [[nodiscard]] rhi::error bind_index_buffer(rhi::buffer const& handle, std::uint64_t const offset, rhi::index_type const type) noexcept override {
            this->last_bound_index = &handle;
            this->last_index_offset = offset;
            this->last_index_type = type;
            return rhi::error::ok;
        }

        void draw(std::uint32_t const vertex_count, std::uint32_t const instance_count, std::uint32_t const first_vertex, std::uint32_t const first_instance) noexcept override {
            this->last_draw = {vertex_count, instance_count, first_vertex, first_instance};
        }

        void draw_indexed(std::uint32_t const index_count, std::uint32_t const instance_count, std::uint32_t const first_index, std::int32_t const vertex_offset, std::uint32_t const first_instance) noexcept override {
            this->last_indexed_draw = {index_count, instance_count, first_index, vertex_offset, first_instance};
        }

        void dispatch(std::uint32_t const groups_x, std::uint32_t const groups_y, std::uint32_t const groups_z) noexcept override {
            this->last_dispatch = {groups_x, groups_y, groups_z};
        }

        void draw_mesh_tasks(std::uint32_t const groups_x, std::uint32_t const groups_y, std::uint32_t const groups_z) noexcept override {
            this->last_mesh_tasks = {groups_x, groups_y, groups_z};
        }

        [[nodiscard]] rhi::error draw_mesh_tasks_indirect(rhi::buffer const& handle, std::uint64_t const offset, std::uint32_t const count, std::uint32_t const stride) noexcept override {
            this->last_indirect_mesh = {&handle, offset, count, stride};
            return rhi::error::ok;
        }

        // abi 24: the recording face's ray-tracing LAUNCH. A fake backend records nothing; the real one calls the
        // entry point it resolved at startup.
        void trace_rays(rhi::shader_binding_table_region const&, rhi::shader_binding_table_region const&, rhi::shader_binding_table_region const&, rhi::shader_binding_table_region const&,
                        std::uint32_t, std::uint32_t, std::uint32_t) noexcept override {
        }

        void set_viewport(rhi::viewport const& vp) noexcept override {
            this->last_viewport = vp;
        }

        void set_scissor(rhi::rect const& scissor) noexcept override {
            this->last_scissor = scissor;
        }

        void set_cull_mode(rhi::cull_mode const mode) noexcept override {
            this->last_cull_mode = mode;
        }

        void set_depth_write(bool const enable) noexcept override {
            this->last_depth_write = enable;
        }

        void set_depth_bias(float const constant_factor, float const slope_factor, float const clamp) noexcept override {
            this->last_depth_bias = {constant_factor, slope_factor, clamp};
        }

        [[nodiscard]] rhi::error barrier(rhi::barrier_group const& group) noexcept override {
            this->barrier_groups += 1;
            this->last_group_images = group.images.size();
            this->last_group_buffers = group.buffers.size();
            return rhi::error::ok;
        }

        [[nodiscard]] rhi::error barrier(rhi::image_barrier const& one) noexcept override {
            this->single_barriers += 1;
            this->last_single_barrier_to = one.to;
            return rhi::error::ok;
        }

        [[nodiscard]] rhi::error copy_image(rhi::image_copy const& copy) noexcept override {
            this->last_image_copy = &copy;
            return rhi::error::ok;
        }

        [[nodiscard]] rhi::error copy_buffer(rhi::buffer& destination, rhi::buffer const& source, std::uint64_t const size, std::uint64_t const source_offset, std::uint64_t const destination_offset) noexcept override {
            this->last_buffer_copy = {&destination, &source, size, source_offset, destination_offset};
            return rhi::error::ok;
        }

        [[nodiscard]] rhi::error clear_color_image(rhi::image const& target, std::array<float, 4> const& color, rhi::subresource_range const& range) noexcept override {
            this->last_cleared_image = &target;
            this->last_clear_color = color;
            this->last_clear_range = range;
            return rhi::error::ok;
        }

        rhi::image_use last_use_from = rhi::image_use::color_attachment;
        rhi::image_use last_use_to = rhi::image_use::color_attachment;
        std::string_view last_stage_name = {};
        std::uint32_t marks = 0;
        // the record series' echo fields (abi 20)
        std::uint32_t begun_renderings = 0;
        std::uint32_t ended_renderings = 0;
        std::uint32_t last_layer_count = 1;
        bool last_secondary_contents = false;
        std::uint32_t bound_pipelines = 0;
        rhi::pipeline const* last_bound_pipeline = nullptr;
        rhi::buffer const* last_bound_vertex = nullptr;
        std::uint64_t last_vertex_offset = 0;
        rhi::buffer const* last_bound_index = nullptr;
        std::uint64_t last_index_offset = 0;
        rhi::index_type last_index_type = rhi::index_type::uint16;
        struct draw_call {
            std::uint32_t a = 0, b = 0, c = 0, d = 0;
        };
        draw_call last_draw = {};
        struct indexed_draw_call {
            std::uint32_t a = 0, b = 0, c = 0;
            std::int32_t d = 0;
            std::uint32_t e = 0;
        };
        indexed_draw_call last_indexed_draw = {};
        draw_call last_dispatch = {};
        draw_call last_mesh_tasks = {};
        struct indirect_mesh_call {
            rhi::buffer const* buffer = nullptr;
            std::uint64_t offset = 0;
            std::uint32_t count = 0;
            std::uint32_t stride = 0;
        };
        indirect_mesh_call last_indirect_mesh = {};
        rhi::viewport last_viewport = {};
        rhi::rect last_scissor = {};
        rhi::cull_mode last_cull_mode = rhi::cull_mode::none;
        bool last_depth_write = false;
        struct depth_bias_call {
            float constant = 0.0f, slope = 0.0f, clamp = 0.0f;
        };
        depth_bias_call last_depth_bias = {};
        std::uint32_t barrier_groups = 0;
        std::uint32_t last_group_images = 0;
        std::uint32_t last_group_buffers = 0;
        std::uint32_t single_barriers = 0;
        rhi::image_use last_single_barrier_to = rhi::image_use::undefined;
        rhi::image_copy const* last_image_copy = nullptr;
        struct buffer_copy_call {
            rhi::buffer* destination = nullptr;
            rhi::buffer const* source = nullptr;
            std::uint64_t size = 0, source_offset = 0, destination_offset = 0;
        };
        buffer_copy_call last_buffer_copy = {};
        rhi::image const* last_cleared_image = nullptr;
        std::array<float, 4> last_clear_color = {};
        rhi::subresource_range last_clear_range = {};
    };

    /// THE PROBE'S OWNED COMMAND BUFFER (abi 15): the handle `create_command_buffer()` hands out,
    /// statically allocated like `probe_buffer` (a probe has no device to allocate from, and the point
    /// here is the CONTRACT's shape: one reference, a begin/end lifecycle, a borrowed recording view,
    /// and execution of a secondary). Its answers are the echo the test reads:
    ///   - `begin_recording()` records the usage it was handed (the flags cross the boundary),
    ///   - `release()` drops the one reference, and every later verb answers `not_ready` (the same
    ///     "a released handle is dead" shape `probe_buffer::release()` gives through size() == 0),
    ///   - `execute()` accepts only THIS probe's buffer and refuses anything else by name.
    /// The bodies live below `impl` (the execute check reaches back through the owner).

    /// THE PROBE'S PRESENTATION SURFACE (abi 14): a BORROWED view like the walker and the profiler, and
    /// the DESCRIPTOR ECHO reaches through it - the "current size" a rebuild re-derives from the window
    /// it owns is, for a probe with no window, the creation descriptor it was handed
    /// (`window_width` x `window_height`), so a view that never crossed the boundary cannot answer
    /// with those numbers. A zero-sized descriptor is the DEFERRED recreate state (`not_ready`), which
    /// is the contract's rule for a not-yet-sized window. The bodies live below `impl` (they reach its
    /// echo through the owner pointer, which the constructor sets).
    struct probe_swapchain final : rhi::swapchain {
        impl* owner = nullptr;

        /// A BORROWED view: this surface is a static member of the probe, so there is no reference to
        /// drop - the call is counted, and the count is what makes "the call arrived" observable.
        void release() noexcept override;
        [[nodiscard]] rhi::error recreate() override;
        [[nodiscard]] rhi::image_extent extent() const noexcept override;
        [[nodiscard]] rhi::image_format format() const noexcept override {
            return rhi::image_format::bgra8_srgb;
        }

        std::uint32_t releases = 0;
        std::uint32_t recreates = 0;
    };

    /// The probe's api_core. `final` so that a missing override is a compile error
    /// rather than an inherited pure virtual in an abstract class nobody notices.
    // CPU recording resources for the portability path. No driver or graphics library is used.
    struct cpu_buffer final : rhi::buffer {
        std::uint64_t bytes = 0;
        std::vector<std::byte> content;
        explicit cpu_buffer(rhi::buffer_desc const& desc)
            : bytes(desc.size) {
            bool const host = desc.usage == rhi::buffer_usage::uniform_coherent ||
                              desc.usage == rhi::buffer_usage::uniform_cached ||
                              desc.usage == rhi::buffer_usage::storage_coherent ||
                              desc.usage == rhi::buffer_usage::readback_coherent;
            if (host) {
                content.resize(static_cast<std::size_t>(desc.size));
                std::copy(desc.initial_bytes.begin(), desc.initial_bytes.end(), content.begin());
            }
        }
        void release() noexcept override {
            delete this;
        }
        std::uint64_t size() const noexcept override {
            return bytes;
        }
        std::span<std::byte> mapped() noexcept override {
            return content;
        }
    };
    struct cpu_image_view final : rhi::image_view {
        void release() noexcept override {
            delete this;
        }
    };
    struct cpu_image final : rhi::image {
        rhi::image_extent shape;
        rhi::image_format pixel_format;
        std::uint32_t layers, mips;
        explicit cpu_image(rhi::image_desc const& desc)
            : shape(desc.extent)
            , pixel_format(desc.format)
            , layers(desc.array_layers)
            , mips(desc.mip_levels != 0 ? desc.mip_levels : std::bit_width(std::max(desc.extent.width, desc.extent.height))) {
        }
        void release() noexcept override {
            delete this;
        }
        rhi::image_extent extent() const noexcept override {
            return shape;
        }
        rhi::image_format format() const noexcept override {
            return pixel_format;
        }
        std::expected<rhi::image_content, rhi::error> get_content(rhi::image_copy_region const&) const override {
            return std::unexpected(rhi::error::unsupported);
        }
        rhi::image_view* make_view(rhi::image_view_desc const& desc) override {
            if (desc.base_layer >= layers || desc.base_mip >= mips ||
                (desc.layer_count != 0 && desc.layer_count > layers - desc.base_layer) ||
                (desc.mip_count != 0 && desc.mip_count > mips - desc.base_mip))
                return nullptr;
            return new cpu_image_view;
        }
    };
    struct cpu_sampler final : rhi::sampler {
        void release() noexcept override {
            delete this;
        }
    };
    struct cpu_capabilities final : rhi::device_capabilities {
        bool mesh_shader() const noexcept override {
            return false;
        }
        bool ray_query() const noexcept override {
            return false;
        }
        std::uint32_t max_push_constants_size() const noexcept override {
            return 256;
        }
        std::uint32_t graphics_queue_family() const noexcept override {
            return 0;
        }
        rhi::shader_binding_table_properties shader_binding_table() const noexcept override {
            return {};
        }
        std::uint64_t acceleration_structure_scratch_alignment() const noexcept override {
            return 1;
        }
        std::uint64_t max_acceleration_structure_instances() const noexcept override {
            return 0;
        }
    };
    struct cpu_heap final : rhi::descriptor_heap {
        std::uint32_t image_writes = 0, buffer_writes = 0;
        bool ready() const noexcept override {
            return true;
        }
        rhi::descriptor_heap_properties properties() const noexcept override {
            rhi::descriptor_heap_properties info{};
            info.max_push_data = 256;
            info.buffer_descriptor_size = 64;
            info.image_descriptor_size = 64;
            info.sampler_descriptor_size = 32;
            return info;
        }
        rhi::heap_bindings bindings() const noexcept override {
            return {};
        }
        rhi::error write_image(rhi::heap_image_write_info const& info) noexcept override {
            if (info.resource == nullptr || info.view == nullptr || info.offset % 64 != 0)
                return rhi::error::invalid_argument;
            ++image_writes;
            return rhi::error::ok;
        }
        rhi::error write_buffer(rhi::heap_buffer_write_info const& info) noexcept override {
            if (info.address == 0 || info.size == 0 || info.offset % 64 != 0)
                return rhi::error::invalid_argument;
            ++buffer_writes;
            return rhi::error::ok;
        }
        rhi::error bind(rhi::heap_bind_info const& info) const noexcept override {
            return info.commands != nullptr ? rhi::error::ok : rhi::error::invalid_argument;
        }
        rhi::error push_data(rhi::heap_push_info const& info) const noexcept override {
            return info.commands == nullptr ? rhi::error::invalid_argument : rhi::validate_heap_push_range(info.offset, info.data.size(), 256);
        }
    };

    struct impl final : rhi::api_core {
        /// THE CONTRACT'S THIRD HANDSHAKE (abi 19), with a knob: the engine checks this against its
        /// own `rhi::abi_version`, and the injection test needs BOTH answers - the agreeing one and
        /// a deliberately stale one - to prove the check is the thing that fires. `make_core` below
        /// (the test-only factory) is what sets it; the C entry always leaves the default.
        [[nodiscard]] std::uint32_t api_version() const noexcept override {
            return this->reported_api_version;
        }

        [[nodiscard]] rhi::ability_bits abilities() const noexcept override {
            if (portable_mode)
                return rhi::to_bits(rhi::extension_kind::device_address) | rhi::to_bits(rhi::extension_kind::device_capabilities) | rhi::to_bits(rhi::extension_kind::descriptor_heap);
            // TWO ABILITIES SINCE abi 19: the escape joined device_address, because the ENGINE's runtime
            // asks it for the presentation format before any frame exists (`escape()` dereferences what
            // `query_extension<vulkan_escape>()` answers, so an injected root has to serve it - see
            // `probe_escape`). Both bits are announced, and both answer with an object whose kind matches,
            // which is the contract's two-way invariant (the walk in test_dynamic_link checks it).
            return rhi::to_bits(rhi::extension_kind::device_address) | rhi::to_bits(rhi::extension_kind::vulkan_escape);
        }

        [[nodiscard]] rhi::extension* query_extension(rhi::extension_kind kind) noexcept override {
            switch (kind) {
            case rhi::extension_kind::device_address:
                return &this->address;
            case rhi::extension_kind::vulkan_escape:
                return portable_mode ? nullptr : &this->escape;
            case rhi::extension_kind::device_capabilities:
                return portable_mode ? &cpu_caps : nullptr;
            case rhi::extension_kind::descriptor_heap:
                return portable_mode ? &heap : nullptr;
            default:
                return nullptr; // not announced, so not available (§3.6)
            }
        }

        [[nodiscard]] rhi::swapchain* create_swapchain(rhi::swapchain_desc const&) override {
            return nullptr;
        }

        [[nodiscard]] rhi::buffer* create_buffer(rhi::buffer_desc const& desc) override {
            if (portable_mode)
                return desc.initial_bytes.size() <= desc.size ? new cpu_buffer(desc) : nullptr;
            this->buffer.size_bytes = desc.size;
            return &this->buffer;
        }

        [[nodiscard]] rhi::image* create_image(rhi::image_desc const& desc) override {
            return portable_mode && desc.extent.width != 0 && desc.extent.height != 0 && desc.array_layers != 0 && desc.format != rhi::image_format::unknown ? new cpu_image(desc) : nullptr;
        }

        /// TIER-1 SINCE ABI 26 AND NULL HERE, which is this fake's whole stance: it has no device, so an
        /// acceleration structure is exactly what it cannot hand out - the same answer `create_image` gives
        /// (and the answer the retired `ray_tracing` ability got from it: "an engine that asks for ray_tracing
        /// gets a null, not a stub").
        [[nodiscard]] rhi::acceleration_structure* create_acceleration_structure(rhi::acceleration_structure_desc const&) override {
            return nullptr;
        }

        /// ABI 27: A MICROMAP IS NULL HERE FOR THE SAME REASON - no device, so nothing to build one in.
        [[nodiscard]] rhi::micromap* create_micromap(rhi::micromap_desc const&) override {
            return nullptr;
        }

        [[nodiscard]] rhi::sampler* create_sampler(rhi::sampler_desc const& desc) override {
            if (portable_mode)
                return new cpu_sampler;
            // abi 16: the descriptor's new fields have to CROSS, so the probe keeps it and the test reads it
            // back (a nullptr answer would make the crossing untestable - the same reason create_buffer
            // returns its stand-in rather than nothing).
            this->sampler.seen = desc;
            this->sampler.released = false;
            return &this->sampler;
        }

        [[nodiscard]] rhi::shader* create_shader(rhi::shader_desc const&) override {
            return nullptr;
        }

        [[nodiscard]] rhi::pipeline* create_pipeline(rhi::pipeline_desc const&) override {
            return nullptr;
        }

        [[nodiscard]] rhi::query* create_query(rhi::query_desc const&) override {
            return nullptr;
        }

        [[nodiscard]] rhi::command_buffer* begin_commands() override {
            // A BORROWED VIEW, AND ONLY WHILE A FRAME IS OPEN: the real backend answers nullptr when
            // nothing is in flight, and the probe keeps that rule with its own frame fiction
            // (`frame_begin()` / the walker's `wait_and_acquire()` open it, `submit()` closes it).
            // abi 14's timing verbs live ON this list, which is the reason it is handed out at all.
            return this->frame_open ? &this->commands : nullptr;
        }

        [[nodiscard]] rhi::submit_info frame_begin() override {
            rhi::submit_info info{};
            // THE RING IS THE ONE CURSOR (see probe_frame_walker): `frame_begin()` reports the same
            // slot the frame walker's `position()` names.
            info.frame_index = this->current_slot;
            // THE ECHO OF THE CREATION DESCRIPTOR (see deren_make_api_core below): the field the test
            // filled is the field the test reads back - a descriptor that never reached the library
            // cannot produce the number.
            info.image_index = portable_mode ? 0u : this->creation_window_width;
            // ... AND THE FRAME IS OPEN: this is the window `begin_commands()` and the recording verbs
            // answer in, the same window the real backend's acquire opens (see begin_commands()).
            this->frame_open = true;
            this->frame_acquired = true;
            return info;
        }

        /// abi 14: the verb ANSWERS now. A frame that was never acquired has nothing to show, so the
        /// probe refuses BY NAME (`not_ready`) instead of pretending it presented something.
        [[nodiscard]] rhi::error present() override {
            if (!this->frame_acquired) {
                return rhi::error::not_ready;
            }
            ++this->presents;
            return rhi::error::ok;
        }

        void wait_idle() override {
            ++this->waits;
        }

        /// THE FRAME IMAGE ANSWERS NOW (③-D/E item B): the probe has no device and no swapchain, so the
        /// stand-in is a 4x4 fiction - but its SHAPE is the contract's: borrowed from the probe, alive only
        /// while a frame is open (`frame_acquired`), and handing out an owned view from `make_view()`.
        [[nodiscard]] rhi::image* frame_image() noexcept override {
            return this->frame_acquired ? &this->borrowed_frame_image : nullptr;
        }

        [[nodiscard]] rhi::buffer* frame_readback_buffer() noexcept override {
            return nullptr;
        }

        // ---- the frame face (abi 13) ------------------------------------------------------------
        [[nodiscard]] rhi::frame_walker* walk_frames() noexcept override {
            return &this->walker;
        }

        [[nodiscard]] rhi::gpu_profiler* profiler() noexcept override {
            return &this->profiler_view;
        }

        // ---- the frame verbs abi 14 added to the context ----------------------------------------
        /// THE PRESENTATION SURFACE, as a BORROWED view - the same object every call, never a new one
        /// (the walker's and the profiler's rule). Its `extent()` is the creation-descriptor echo.
        [[nodiscard]] rhi::swapchain* frame_swapchain() noexcept override {
            return &this->swapchain_view;
        }

        /// abi 14: the frame verb the contract was missing. The list must be THIS probe's own borrowed
        /// recording view (a foreign list is a caller bug, refused by name, never guessed at), and
        /// there must be a frame to hand over - the same two-way check the real backend makes.
        [[nodiscard]] rhi::error submit(rhi::command_buffer& commands_ref) override {
            if (&commands_ref != static_cast<rhi::command_buffer*>(&this->commands)) {
                return rhi::error::invalid_argument;
            }
            if (!this->frame_open) {
                return rhi::error::not_ready;
            }
            this->frame_open = false; // handed over: no frame is open to record into any more
            ++this->submits;
            return rhi::error::ok;
        }

        /// abi 15: the owned recording handle. The probe has no device, so it answers with its static
        /// stand-in and ECHOES the descriptor into it; a kind outside the two roles the contract names
        /// is the factory's one refusal (`nullptr`, the contract's named-refusal rule).
        /// abi 21: the SAME stand-in, handed over as a shared_ptr. The probe buffer is a STATIC
        /// member (a probe has no device to allocate from), so the control block deleter is a no-op -
        /// release() is what the test observes, and the object itself outlives every reference.
        [[nodiscard]] std::shared_ptr<rhi::command_buffer> make_command_buffer(rhi::command_buffer_desc const& desc) override {
            rhi::command_buffer* const raw = this->create_command_buffer(desc);
            if (raw == nullptr) {
                return {};
            }
            return std::shared_ptr<rhi::command_buffer>(raw, [](rhi::command_buffer*) noexcept {});
        }

        [[nodiscard]] rhi::command_buffer* create_command_buffer(rhi::command_buffer_desc const& desc) override {
            if (desc.kind != rhi::command_buffer_kind::primary && desc.kind != rhi::command_buffer_kind::secondary) {
                return nullptr;
            }
            // RESET ON CREATION, like probe_buffer's size: the one reference the factory hands out starts
            // here, so a released object becomes live again exactly as a fresh one would be.
            this->command_buffer.kind_echo = desc.kind;
            this->command_buffer.usage_echo = rhi::no_command_buffer_flags;
            this->command_buffer.executions = 0;
            this->command_buffer.released = false;
            return &this->command_buffer;
        }

        bool portable_mode = false;
        cpu_capabilities cpu_caps;
        cpu_heap heap;
        probe_buffer buffer{};
        probe_sampler sampler{};                  ///< abi 16: the descriptor echo the sampler test reads
        probe_frame_image borrowed_frame_image{}; ///< abi 16 (item B): the borrowed frame image and its owned view
        probe_device_address address{};
        /// abi 15: the command buffer `create_command_buffer()` hands out. ONE object, reset on every
        /// creation - the same "statically allocated stand-in" shape `buffer` has, and the reason a
        /// second create answers with the same pointer (the test says so where it matters).
        probe_command_buffer command_buffer{};
        /// the ring cursor `frame_begin()` and the frame walker both answer (see the walker's note)
        std::uint32_t current_slot = 0;
        /// the creation descriptor's `window_width`, carried in by deren_make_api_core and echoed out
        /// of frame_begin() and the walker's open; 0 would be an impl nobody filled, which the test's
        /// non-zero fill catches
        probe_escape escape;
        std::uint32_t creation_window_width = 0;
        /// what `api_version()` answers: the contract's number unless a test asked for another
        std::uint32_t reported_api_version = rhi::abi_version;
        /// the creation descriptor's `window_height`, echoed by the swapchain view's `extent()` (the
        /// creation descriptor is the only "window size" a probe has)
        std::uint32_t creation_window_height = 0;
        std::uint32_t presents = 0;
        std::uint32_t waits = 0;
        std::uint32_t submits = 0;
        /// WHETHER A FRAME IS OPEN: the probe's stand-in for the backend's `frame_in_flight`, so
        /// `begin_commands()`/the timing verbs/the submit check answer in the same window the real
        /// backend answers in
        bool frame_open = false;
        /// whether a frame was ever opened, which is what `present()` refuses on (abi 14)
        bool frame_acquired = false;
        probe_frame_walker walker{};
        probe_command_buffer commands{};
        probe_swapchain swapchain_view{};   // not `swapchain`: a type name, not a member name
        probe_gpu_profiler profiler_view{}; // not `profiler`: the accessor above owns that name

        impl() noexcept {
            // THE OWNER IS SET IN THE CONSTRUCTOR - the same lesson the real backend's constructor
            // states: a view handed out with a null owner dereferences null on its first call.
            this->commands.owner = this;
            this->walker.owner = this;
            this->swapchain_view.owner = this;
            this->command_buffer.owner = this;
            // ITS list is NOT the frame's: the frame-scoped verbs must refuse it (abi 15's rule the
            // real backend applies, mirrored here so the test can measure it without a GPU).
            this->command_buffer.frame_scoped = false;
        }
    };

    // The frame walker's bodies, where `impl` is complete (they answer through its cursor).
    std::uint32_t probe_frame_walker::slot_count() const noexcept {
        return 2u;
    }

    std::uint32_t probe_frame_walker::position() const noexcept {
        return this->owner->current_slot;
    }

    rhi::frame_open_info probe_frame_walker::wait_and_acquire() {
        // THE BY-VALUE POD, part two: `frame_open_info` crosses the boundary like `submit_info` does
        // - decision and frame together - and the slot it reports is the cursor's.
        // Opening a frame here IS opening the recording window: `begin_commands()` and abi 14's
        // timing verbs answer in it, exactly as they do after the real backend's acquire.
        this->owner->frame_open = true;
        this->owner->frame_acquired = true;
        return {.frame = {.frame_index = this->owner->current_slot, .image_index = this->owner->portable_mode ? 0u : this->owner->creation_window_width}, .result = {}};
    }

    void probe_frame_walker::walk_to_next() noexcept {
        this->owner->current_slot = (this->owner->current_slot + 1u) % this->slot_count();
    }

    // The swapchain view's bodies, where `impl` and its descriptor echo are complete.
    void probe_swapchain::release() noexcept {
        ++this->releases;
    }

    rhi::error probe_swapchain::recreate() {
        ++this->recreates;
        // A ZERO-SIZED DESCRIPTOR IS THE DEFERRED STATE, not a failure: the probe has no window, so
        // the "current size" it re-derives is the creation descriptor, and a 0 x 0 one answers
        // `not_ready` - the contract's own rule for a window that has not been sized yet.
        return this->owner->creation_window_width != 0u && this->owner->creation_window_height != 0u ? rhi::error::ok : rhi::error::not_ready;
    }

    rhi::image_extent probe_swapchain::extent() const noexcept {
        return {.width = this->owner->creation_window_width, .height = this->owner->creation_window_height, .depth = 1u};
    }

    // The command buffer's bodies, where `impl` is complete (the execute check reaches back to the one
    // object the factory hands out).
    void probe_command_buffer::release() noexcept {
        // THE ONE REFERENCE IS DROPPED: the probe's buffer is a static stand-in, so "freed" can only be
        // OBSERVED, and it is - every later verb answers `not_ready` (the same shape probe_buffer's
        // release gives through size() == 0).
        this->released = true;
    }

    rhi::error probe_command_buffer::begin_recording(rhi::command_buffer_begin_info const& info) {
        if (this->released) {
            return rhi::error::not_ready;
        }
        // THE USAGE CROSSES AND IS ECHOED: the four-flag set the caller composed is what the probe
        // remembers, so the test can tell "the call arrived with these bits" from "a default was used".
        this->usage_echo = info.usage;
        return rhi::error::ok;
    }

    rhi::error probe_command_buffer::end_recording() noexcept {
        return this->released ? rhi::error::not_ready : rhi::error::ok;
    }

    rhi::error probe_command_buffer::execute(rhi::command_buffer& secondary) {
        if (this->released) {
            return rhi::error::not_ready;
        }
        // THE SAME PROVENANCE RULE THE REAL BACKEND APPLIES: a buffer this factory did not hand out is
        // refused by name (the probe has exactly one, so "ours" is an identity comparison).
        if (&secondary != static_cast<rhi::command_buffer*>(&this->owner->command_buffer)) {
            return rhi::error::invalid_argument;
        }
        ++this->executions;
        return rhi::error::ok;
    }

} // namespace

extern "C" DEREN_API_EXPORT std::shared_ptr<rhi::api_core>
deren_make_api_core(std::uint32_t abi_version, deren::promise::rhi::create_info const* desc,
                    deren::promise::rhi::error_info* out_error_info) {
    if (abi_version != deren::promise::rhi::abi_version) {
        if (out_error_info != nullptr) {
            // THE DIAGNOSTIC, WHOLE: code, who produced it, static text and the failure point - the
            // abi 13 channel is the whole story, not a bare number. THE BACKEND'S OWN NUMBER travels in
            // `native_code` since abi 18: `deren_abi_version()` is gone, so this is where a host reads
            // the number it disagrees with.
            *out_error_info = rhi::error_info{.code = rhi::error::abi_mismatch,
                                              .native_code = static_cast<std::int32_t>(rhi::abi_version),
                                              .message = "probe: the caller's abi_version is not this backend's",
                                              .where = std::source_location::current()};
        }
        return {};
    }
    // "no creation parameters" is a caller bug, not a request for the standard context: the contract's
    // own `create_info{}` is how that is spelled (see backend_entry.hpp).
    if (desc == nullptr) {
        if (out_error_info != nullptr) {
            *out_error_info = rhi::error_info{.code = rhi::error::invalid_argument,
                                              .message = "probe: the creation descriptor is null",
                                              .where = std::source_location::current()};
        }
        return {};
    }
    if (out_error_info != nullptr) {
        *out_error_info = rhi::error_info{}; // ok: a zeroed diagnostic says ok - nobody failed
    }
    // THE CREATION DESCRIPTOR IS ECHOED BACK THROUGH AN EXISTING CALL. The probe has no device, so it
    // cannot honour the descriptor's window / vsync / render-scale fields; what it CAN do - and what
    // makes the crossing observable - is carry `window_width` into the by-value POD results the
    // contract returns: `frame_begin()`'s and the frame walker's `image_index`. The test fills the
    // descriptor and reads the number back, so a request that never reached the library cannot pass
    // for one that did.
    //
    // OWNERSHIP: `make_shared` puts the deleter in the control block (abi 18), so the probe object is
    // destroyed by whoever drops the last reference - no deleter entry point exists to resolve.
    auto created = std::make_shared<impl>();
    created->creation_window_width = static_cast<std::uint32_t>(desc->window_width);
    created->creation_window_height = static_cast<std::uint32_t>(desc->window_height);
    created->reported_api_version = rhi::abi_version; // the C entry never lies about the contract
    return created;
}

// ---- THE PROBE AS AN ENGINE-SIDE INJECTION SOURCE (batch ⑥) ----------------------------------
// The runtime's constructor takes a device root, so a test can hand it one and prove the layering
// without a DLL, a loader or a GPU. `reported_api_version` is the knob that makes the CONSUMPTION
// handshake testable: a probe reporting `rhi::abi_version + 1` must make the runtime refuse the
// object. Declared in tests/probe_backend.hpp so the test links the same TU the C entry lives in
// instead of re-compiling the probe (two copies of it would drift).
namespace deren::vk_test {
    std::shared_ptr<deren::promise::rhi::api_core> probe_make_core(deren::promise::rhi::create_info const& desc,
                                                                   std::uint32_t const reported_api_version, bool const portable_mode) {
        auto created = std::make_shared<impl>();
        created->creation_window_width = static_cast<std::uint32_t>(desc.window_width);
        created->creation_window_height = static_cast<std::uint32_t>(desc.window_height);
        created->reported_api_version = reported_api_version;
        created->portable_mode = portable_mode;
        return created;
    }
} // namespace deren::vk_test
