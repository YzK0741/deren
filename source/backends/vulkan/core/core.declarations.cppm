// ============================================================================
// module: deren.vulkan.core:declarations  - the INTERFACE PARTITION of deren.vulkan.core
//
// The declarations live here so the implementation halves can be partitions: a partition does not
// see the primary interface on its own, so every implementation partition imports THIS one.
// The primary (source/backends/vulkan/core/core.cppm) is the three lines that re-export it, and `import deren.vulkan.core;`
// is unaffected anywhere - which is the whole point of paying for the split.
//
// MEASURED BEFORE COMMITTING TO IT: under this project's -Werror, clang 22 REJECTS the textbook
// pattern (the primary importing its implementation partitions) with
// -Wimport-implementation-partition-unit-in-interface-unit. The viable variant, verified on a scratch
// project with these exact flags, is this one: the primary imports ONLY interface partitions, and the
// implementation partitions are listed in CMake (which compiles every unit) without being imported by
// the interface. Their definitions are still archived and still link.
// ============================================================================
// ============================================================================
// module: deren.vulkan.core
// module version: 1.0.0  (independent of the app version in CMakeLists project(VERSION))
//
// GPU scaffolding: instance / device / swapchain / VMA / pipeline / descriptor
// plumbing (core.vma / core.pipeline / core.filter / core.init_utils submodules
// are part of this unit). Standalone Vulkan wrapper; depends on VMA + utility,
// with the struct-fill conventions coming from vulkan.constant_init.
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - independently of the rest of the project.
// ============================================================================
module;
#include "lazy_loading.h"

#include <GLFW/glfw3.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string_view> // the CONCRETE implementation names are static string_views (ABI 30)
#include <unordered_map>
#include <unordered_set>
#include <vulkan/vulkan.h>

export module deren.vulkan.core:declarations;
export import deren.utility.shared;
import deren.utility;
export import deren.vstd;
export import deren.vulkan.core.handles;
export import :vma;
export import :descriptor_heap;

// THE BACKEND IMPLEMENTS THE RHI CONTRACT IN PLACE: `core` IS the `api_core` a host gets back from
// `deren_make_api_core()`, not a wrapper object standing beside it. It owns the instance, the device,
// the swapchain and the frame machinery, so it is the thing the contract describes; a second type in
// front of it would be a second lifetime to keep in step, and `deren_make_api_core()` would have to
// invent an owner for it. With the base here, the entry allocates `this` and hands it out as the
// contract's `shared_object<api_core>`, whose drop is `core::release()` (which deletes it).
//
// THE DEPENDENCY DIRECTION IS THE POINT: backend -> contract, never contract -> backend. The contract
// module imports nothing (source/promise/rhi/*.cppm), so a backend that implements it cannot drag anything
// back into the engine's contract.
import deren.promise.rhi;
import deren.promise.render_operations;
// THE CONCRETE LAYOUT of the contract's opaque `main_context` (abi 39): the SAME interface unit the GUI
// plugin compiles (vulkan_main_context), so the tag and the layout cannot fork between the two images.
export import deren.vulkan.main_context;
import deren.vulkan.command_buffer;
// The error translators and the diagnostic builder: a neutral module both sides link (see the note
// in the purview). RE-EXPORTED, because every backend TU that names `failed`/`*_error` imports THIS
// module and must keep compiling unchanged.
export import deren.vulkan.error_tables;
// The renderer's slot grid (③-D/E batch, ruling D): the values live in a module BOTH halves compile, so
// the engine can name the grid without importing this backend. Re-exported, because `core::heap_slots`
// and the `core::heap_slot_*` constants below are aliases INTO it - a consumer of an alias needs the
// underlying declaration.
export import deren.engine.render_layout;
export import :vma_handles;

/**
 * @file source/backends/vulkan/core/core.declarations.cppm
 * @brief the interface partition of `deren.vulkan.core`: the core type, its views and its helpers.
 */

namespace deren::vulkan {

    /**
     * @defgroup vulkan_core Vulkan Core Objects Manager
     * @brief manages core vulkan objects and windows instance init and destroy.
     * @note
     *      - RAII
     *      - includes VMA decorator, which is defined in ./vma/vma.cppm
     *
     * @warning
     *      - do not call the init or create function, just use the members or other functions
     *      - no thread-safe
     */

    /**
     * @ingroup vulkan_core
     * @brief agreed flat layout of the scene block, shared by every pipeline (see source/shaders/scene/pbr.slang):
     *        set 0 binding 0 = CameraUBO (uniform buffer; one per frame slot, each slot's block
     *              points at its own - static, no per-frame descriptor writes),
     *              binding 1 = sampler2D textures[] (runtime array, partially bound + non-uniform index),
     *              binding 2/3/4 = prefiltered env / irradiance / BRDF LUT (combined image samplers),
     *              binding 5 = Material materials[] (storage buffer: per-material texture indices + factors),
     *              binding 6 = mat4 instance transforms[] (storage buffer, per-instance world matrices),
     *              binding 7 = LightUBO (uniform buffer: directional light view-proj + direction),
     *              binding 8 = shadow map (sampler2DArrayShadow, one array layer per cascade; LINEAR
     *              min/mag with compareEnable = VK_TRUE, so the hardware does the 2x2 comparison and
     *              shading.glsl's calc_shadow_cascade() averages a 3x3 grid of those taps),
     *              binding 9 = mat4 skin matrices[] (storage buffer: identity block + per-skin joints),
     *              binding 10 = float morph data[] (storage buffer: per-primitive morph deltas + weights),
     *              binding 11/12 = uint cluster light counts[] / uint cluster light indices[] (the
     *              clustered-culling result: written by the cluster compute pass, read by the
     *              fragment stage),
     *              binding 13 = mat4 previous world matrices[] (one per motion slot; the vertex stage
     *              reads its own entry so the fragment stage can build TAA's motion vector for a
     *              MOVING object, not only for camera motion)
     * @note hardcoded instead of parsed from SPIR-V: the indexed layout is flat, so pipelines skip
     *       descriptor / push constant parsing and every stage shares this one binding contract, which the
     *       frame's heap carries
     */
    // WHY `inline` AND NOT A PLAIN `constexpr` (this is the dynamic backend's one prerequisite in this
    // file): a namespace-scope constexpr declared in a module purview has MODULE linkage, so a consumer
    // in ANOTHER IMAGE does not fold it - it references the variable and needs the owning image to
    // export DATA for it. Measured on the DLL probe tree: the executable imported exactly one data
    // symbol, `_ZN5deren6vulkanW5derenW6vulkanW4core22scene_texture_capacityE`, and the runtime entry
    // veneer the boundary is built on can carry functions (a naked jump stub is a function) but NOT data.
    // `inline` gives the constant external linkage with a definition every importer may use, so each side
    // keeps its own copy and nothing crosses the boundary as data. The values are unchanged.
    // THE VALUE IS THE ENGINE LAYOUT'S NOW, not a literal copied beside it: `deren.engine.render_layout` is
    // compiled by BOTH halves (it already publishes the slot grid the array lives on), so aliasing its
    // `scene_texture_capacity` is what makes "the texture run between the two slot names is the capacity" a
    // single derivation instead of two numbers. The `inline` above still matters for the reason it always
    // did: each image keeps its own copy of the VALUE and no data symbol crosses the boundary.
    export inline constexpr uint32_t scene_texture_capacity = deren::engine::render_layout::scene_texture_capacity;
    static_assert(scene_texture_capacity == deren::engine::render_layout::heap_slots::materials - deren::engine::render_layout::heap_slots::textures,
                  "the bindless texture array's capacity is the grid's texture run");
    // material_push_constants: 6 uints + aligned mat4 = 96 bytes, see source/engine/scene_tree/scene_tree.cppm
    // MOVED INTO `deren.engine.render_layout` by the import-cleanup batch, for the reason `hdr_format` moved
    // (A1.0): `deren.engine.primitive` needs it for its exported push-block offsets, and it may not import a
    // backend module to get it. The alias plus the assert below are this backend's half of the drift guard.
    export inline constexpr uint32_t scene_push_constant_size = deren::engine::render_layout::scene_push_constant_size;
    static_assert(scene_push_constant_size == 96u, "the scene push block's size moved");
    /**
     * @ingroup vulkan_core
     * @brief offset of the SECOND push constant range of the shared scene layout, right after the
     *        per-primitive material block
     * @note currently one uint: the cascade index the shadow pass is rendering. It is a separate
     *       range rather than extra fields in the material block because that block is exactly 96
     *       bytes and the two together would exceed the 128 bytes the spec guarantees every
     *       implementation provides (the engine does not rely on a vendor's larger limit).
     */
    export inline constexpr uint32_t scene_cascade_push_offset = scene_push_constant_size;
    export inline constexpr uint32_t scene_cascade_push_size = sizeof(uint32_t);

    /**
     * @brief format of the HDR scene target the deferred lighting stage renders into and the post-process
     *        pass samples: the scene color target uses it, and each swapchain image owns one
     *        single-sample resolve target in it (the ENGINE creates that target now - see
     *        runtime::create_render_chain_targets)
     *
     * MOVED INTO `deren.engine.render_layout` (③-D/E item A1, A1.0): the ENGINE creates these targets now,
     * so the formats it creates them WITH have to live in a module both halves compile. The names stay
     * reachable as `deren::vulkan::hdr_format` / `core::hdr_format` through the aliases below, and the
     * frozen numbers are `static_assert`ed against the module - a drift is a COMPILE ERROR rather than a
     * different picture.
     */
    [[nodiscard]] constexpr VkFormat native_render_format(deren::promise::rhi::image_format format) noexcept {
        using image_format = deren::promise::rhi::image_format;
        switch (format) {
        case image_format::rgba8_unorm:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case image_format::r16g16_sfloat:
            return VK_FORMAT_R16G16_SFLOAT;
        case image_format::r16g16b16a16_sfloat:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        default:
            return VK_FORMAT_UNDEFINED;
        }
    }

    export inline constexpr VkFormat hdr_format = native_render_format(deren::engine::render_layout::hdr_format);

    /**
     * @ingroup vulkan_core
     * @brief how many color targets the G-buffer pass writes (see gbuffer_formats)
     */
    export inline constexpr uint32_t gbuffer_target_count = deren::engine::render_layout::gbuffer_target_count;

    /**
     * @ingroup vulkan_core
     * @brief formats of the G-buffer targets, in attachment order (= the fragment output locations
     *        of source/shaders/scene/gbuffer.slang), and the reason the deferred path is cheap to store:
     *        - 0 RGBA8_UNORM: albedo.rgb (base color, linear) + metallic in a
     *        - 1 RGBA16F: world normal.xyz (no encoding - the conservative layout trades 4 bytes per
     *          pixel for not having to reason about octahedral precision) + roughness in a
     *        - 2 RGBA8_UNORM: material_id low/high byte + ambient occlusion + material flags
     *        16 bytes per pixel in total; the depth is the pass's own single-sampled depth image.
     * @note every target is single-sampled (1x) on purpose: a G-buffer cannot be multisampled
     *       without per-sample shading, which is the trade that makes TAA the anti-aliasing
     *       (the anti-aliasing story is TAA/FXAA on the lit image instead).
     * @note the values themselves moved to `deren.engine.render_layout` with `hdr_format` (A1.0).
     */
    export inline constexpr deren::utility::vessel::array<VkFormat, gbuffer_target_count> gbuffer_formats = [] {
        deren::utility::vessel::array<VkFormat, gbuffer_target_count> formats{};
        for (std::size_t i = 0; i < formats.size(); ++i) {
            formats[i] = native_render_format(deren::engine::render_layout::gbuffer_formats[i]);
        }
        return formats;
    }();

    /**
     * @ingroup vulkan_core
     * @brief motion-vector format: the fourth G-buffer target, written by the G-buffer pass and read
     *        by TAA (RG16F because a motion vector is a signed sub-pixel quantity in UV space and
     *        8-bit would quantize it to ~1/255 of the screen - coarser than the jitter TAA exists to
     *        resolve)
     */
    export inline constexpr VkFormat gbuffer_velocity_format = native_render_format(deren::engine::render_layout::gbuffer_velocity_format);

    /// THE DRIFT GUARD for the moved formats (A1.0), the same shape ruling D used for the slot grid: the
    /// numbers this backend was built with are frozen HERE, so a change on either side has to be a
    /// deliberate edit in both places and a layout drift stops the build.
    static_assert(hdr_format == VK_FORMAT_R16G16B16A16_SFLOAT, "the HDR target's format moved");
    static_assert(gbuffer_target_count == 3u, "the G-buffer's target count moved");
    static_assert(gbuffer_formats[0] == VK_FORMAT_R8G8B8A8_UNORM && gbuffer_formats[1] == VK_FORMAT_R16G16B16A16_SFLOAT &&
                      gbuffer_formats[2] == VK_FORMAT_R8G8B8A8_UNORM,
                  "the G-buffer's formats moved");
    static_assert(gbuffer_velocity_format == VK_FORMAT_R16G16_SFLOAT, "the motion-vector format moved");

    /**
     * @ingroup vulkan_core
     * @brief color attachments the G-buffer pass declares: the three surface targets above, the
     *        motion-vector target, and the scene-color target storing the nearest surface's emissive
     * @note emissive is lighting-independent, so it does not belong to the deferred lighting stage -
     *       and it needs the material's emissive texture and the fragment's UVs, neither of which the
     *       G-buffer stores. All five attachments overwrite: otherwise a farther fragment's emission
     *       could survive a nearer non-emissive fragment that passes depth later.
     *       It is CLEARed to zero by the instance, and the deferred lighting stage adds the lighting
     *       (and the sky, where no geometry wrote depth) on top.
     */
    export inline constexpr uint32_t gbuffer_pass_attachment_count = gbuffer_target_count + 2; // + velocity + scene color

    /**
     * @ingroup vulkan_core
     * @brief how many GPU timing marks one frame may write (the query pool is sized
     *        MAX_FRAMES_IN_FLIGHT * this, and each frame slot owns its own contiguous range)
     * @note a mark is one vkCmdWriteTimestamp; the frame's pass boundaries use a handful of them,
     *       and the remaining capacity is headroom for the passes later milestones add. A frame
     *       that records more marks than this silently stops marking (the extra passes are simply
     *       not measured) instead of overflowing into the next slot's range.
     */
    export inline constexpr uint32_t gpu_timing_mark_capacity = 32;

    /**
     * @ingroup vulkan_core
     * @brief GPU durations of one completed frame, in mark order (see core::mark_gpu_timing)
     * @note entry i is the time between mark i and mark i + 1, so a frame that wrote
     *       @p mark_count marks yields mark_count - 1 durations
     */
    export struct gpu_timing_result {
        deren::utility::vessel::array<double, gpu_timing_mark_capacity> milliseconds = {}; // elapsed per consecutive mark pair
        uint32_t mark_count = 0;                                                           // marks the frame wrote (0 = no measurement)
    };

    /**
     * @ingroup vulkan_core
     * @brief whether a field @p size bytes long at @p offset of a contract structure is inside the
     *        @p struct_size bytes the caller says it compiled
     *
     * THE APPEND-ONLY ABI GUARD, written once because both the context's descriptor and the buffer's
     * are read through it (core.constructor.cppm and core.api_core.cppm). A contract structure grows by
     * APPENDING fields and every field carries a default, so a caller that compiled an older, shorter
     * shape is served the defaults it never declared instead of having this build read past its end.
     */
    [[nodiscard]] constexpr bool covered_by(uint32_t const struct_size, size_t const offset, size_t const size) noexcept {
        return static_cast<size_t>(struct_size) >= offset + size;
    }

    /// THE CONCRETE IMPLEMENTATION NAMES THIS BACKEND GIVES ITS HANDLES (the ABI 30 identity), in ONE
    /// place. The adapter constructors below and every validation site in core.api_core.cpp read the
    /// same constants, so a concrete name cannot drift between the producer and the check that guards
    /// a cast. The names themselves are part of the diagnosis (logs, tests) and must stay stable.
    ///
    /// THE OPERATION IS SHARED, NOT PER-BACKEND: `rhi::is_concrete()` / `rhi::do_concrete()` live in
    /// `deren.promise.rhi:contract` - the contract owns "check the concrete name, then cast to the
    /// concrete layout" (and the panic/refusal split), this namespace owns the NAMES that operation is
    /// asked about. A second backend registers its own names with the same calls and no second copy of
    /// the rule exists.
    namespace concrete {
        inline constexpr std::string_view api_core = "deren_api_core_vulkan";
        /// abi 43: the identities live in the SHARED layout (`denen.vulkan.command_buffer`) because the GUI
        /// plugin checks them too; these aliases keep the backend's own spellings pointing at one source.
        inline constexpr std::string_view frame_commands = deren::vulkan::frame_command_buffer_name;
        inline constexpr std::string_view command_buffer = deren::vulkan::owned_command_buffer_name;
        inline constexpr std::string_view mergeable_command_buffer = deren::vulkan::mergeable_command_buffer_name;
        inline constexpr std::string_view frame_image = "deren_frame_image_vulkan";
        inline constexpr std::string_view frame_snapshot = "deren_frame_snapshot_vulkan";
        inline constexpr std::string_view frame_resources = "deren_frame_resources_vulkan";
        inline constexpr std::string_view frame_readback_buffer = "deren_frame_readback_buffer_vulkan";
        inline constexpr std::string_view swapchain = "deren_swapchain_vulkan";
        inline constexpr std::string_view buffer = "deren_buffer_vulkan";
        inline constexpr std::string_view micromap = "deren_micromap_vulkan";
        inline constexpr std::string_view acceleration_structure = "deren_acceleration_structure_vulkan";
        inline constexpr std::string_view image = "deren_image_vulkan";
        inline constexpr std::string_view image_view = "deren_image_view_vulkan";
        inline constexpr std::string_view sampler = "deren_sampler_vulkan";
        inline constexpr std::string_view shader = "deren_shader_vulkan";
        inline constexpr std::string_view pipeline = "deren_pipeline_vulkan";
        inline constexpr std::string_view device_address = "deren_device_address_vulkan";
        inline constexpr std::string_view host_image_copy = "deren_host_image_copy_vulkan";
        inline constexpr std::string_view device_capabilities = "deren_device_capabilities_vulkan";
        inline constexpr std::string_view shader_group_access = "deren_shader_group_access_vulkan";
        inline constexpr std::string_view descriptor_heap = "deren_descriptor_heap_vulkan";
        inline constexpr std::string_view descriptor_space = "deren_descriptor_space_vulkan";
        inline constexpr std::string_view resource_group = "deren_resource_group_vulkan";
        inline constexpr std::string_view texture_group = "deren_texture_group_vulkan";
        inline constexpr std::string_view gpu_sorting = "deren_gpu_sorting_vulkan";
        inline constexpr std::string_view async_presentation = "deren_async_presentation_vulkan";
        inline constexpr std::string_view scoped_synchronization = "deren_scoped_synchronization_vulkan";
        inline constexpr std::string_view radix_sorter = "deren_radix_sorter_vulkan";
    } // namespace concrete

    /**
     * @brief the device, its allocator and every resource created on it - the renderer's lifetime ROOT
     *
     * THE ROOT IS HANDED OUT AS A `shared_object<api_core>` (abi 44): the C entry allocates the core,
     * `attach_self` keeps the control block's weak handle here, and `release()` is the drop the last
     * shared copy performs (for the root that call destroys it). Frame-owned state keeps the root alive by
     * locking `self_owner`, so a resource that outlives the engine's handle still has a live device. The
     * old `enable_shared_from_this`/shared_ptr spelling is gone with that batch.
     */
    export struct core : deren::promise::rhi::api_core, deren::promise::rhi::resource_factory, deren::promise::rhi::command_context, deren::promise::rhi::frame_context, deren::utility::enable_stack_destruct {
        struct owned_buffer;
        struct owned_image;
        // creation options this core was built with (window size, vsync); the window and the
        // swap chain honor them
        deren::promise::rhi::create_info create_options = {};
        /// THE ROOT'S OWN SHARED CONTROL (abi 44): the entry hands the root out as a
        /// `shared_object<api_core>`; frame-owned state keeps the root alive by locking this weak handle.
        deren::promise::rhi::shared_object<deren::promise::rhi::api_core>::weak self_owner;
        /// the entry's hand-off: remember the control block the root was handed out under
        void attach_self(deren::promise::rhi::shared_object<deren::promise::rhi::api_core> const& root) noexcept {
            this->self_owner = root;
        }
        /// DROP THE ROOT (abi 44): the last shared handle destroys the object the entry allocated.
        void release() noexcept override;

        // ---- THE RECORDING SURFACE AND THE FRAME'S VIEWS --------------------------------------------
        //
        // THE CONTRACT SPEAKS IN BORROWED VIEWS AND THESE ARE THE ONES THIS BACKEND ANSWERS. The frame's
        // primary command buffer, the swapchain image the frame draws into, the host-visible read-back
        // slot, and the main-thread context the GUI plugin casts (abi 39). Each is a member of this class,
        // so "the backend owns it, the caller borrows it" is the object graph rather than a rule in a
        // comment.
        //
        // `owner` is raw and non-owning: the views live INSIDE the core they point at, so their
        // lifetime is this object's, and a caller that keeps one past the core's death holds a
        // dangling pointer - the contract says the same thing about every borrowed view.
        //
        // `use()` turns the contract's (from, to) ROLE PAIR into the barrier this renderer's own recipe
        // describes, and `copy_image_to_buffer()` records the copy the screenshot path used to spell
        // out by hand. The compile-time shadow gate in core.api_core.cpp proves, field by field, that
        // the first lands exactly on constant_init's two recipes (plan §8.3, gate A5).

        /// The frame's PRIMARY command buffer, as the contract's recording surface (`begin_commands()`).
        ///
        /// abi 15 GENERALIZED IT: `target` says WHICH buffer this list records into, so one list type
        /// serves both the frame's slot buffer (`target` null - the abi 13/14 meaning, and the object
        /// `begin_commands()` answers) and every `command_buffer` this backend hands out (target set -
        /// `command_buffer::recording()`). The frame-scoped verbs below (`use`,
        /// `copy_image_to_buffer`, the timing pair) are the FRAME's: they answer `not_ready` on a list
        /// that is not the frame's, which is the window their own contract notes already name.
        struct owned_command_buffer;
        struct owned_pipeline;
        /// THE VULKAN CLASS (abi 43) is the shared declaration both images compile; this class derives from
        /// it, so a tag-checked `static_cast` on the GUI side is a real base-class cast, and the native handle
        /// is read through `get()`. The backend publishes it through `set_target()`.
        struct frame_commands : deren::vulkan::command_buffer {
            explicit frame_commands(std::string_view const name = concrete::frame_commands) noexcept
                : deren::vulkan::command_buffer(name) {
            }
            [[nodiscard]] deren::promise::rhi::error push_constants(std::uint32_t, std::span<std::byte const>) noexcept override;
            core* owner = nullptr;
            /// the buffer this list records into: null = the frame's slot buffer, set = an owned one
            VkCommandBuffer target = VK_NULL_HANDLE;
            bool recording = false;
            bool rendering_active = false;
            bool executable = false;
            bool simultaneous_use = false;
            bool one_time_submit = false;
            owned_pipeline const* bound_pipeline = nullptr;
            owned_pipeline const* emitted_pipeline = nullptr;
            deren::promise::rhi::recording::operation_stream operations;
            deren::promise::rhi::recording::operation_stream saved_state;
            deren::utility::vessel::vector<deren::promise::rhi::recording::render_operation const*> selected_batch, restored_selection, saved_selection;
            deren::utility::vessel::vector<std::pair<std::uint64_t, std::uint64_t>> pending_push_ranges;
            deren::promise::rhi::resource_group_ref current_resources;
            deren::promise::rhi::texture_group_ref current_textures;
            deren::promise::rhi::error recording_error = deren::promise::rhi::error::ok;
            deren::utility::vessel::array<deren::promise::rhi::recording::words, 11> native_state{};
            std::uint64_t native_state_mask = 0;
            bool restore_state = false;
            bool rendering_inherited = false;
            bool final_application = false;
            [[nodiscard]] deren::promise::rhi::error apply_operations() noexcept override;
            [[nodiscard]] deren::promise::rhi::error prepare_native() noexcept override {
                return apply_operations();
            }
            void finish_native() noexcept override {
                restore_state = true;
            }
            void reset_operations() noexcept;

            deren::utility::vessel::vector<std::shared_ptr<void>> lazy_refs;
            [[nodiscard]] deren::promise::rhi::error demand(deren::promise::rhi::buffer const&);
            [[nodiscard]] deren::promise::rhi::error demand(deren::promise::rhi::image const&);
            void retain_lazy(owned_buffer const&);
            void retain_lazy(owned_image const&);
            deren::utility::vessel::vector<deren::promise::rhi::resource_group_ref> resource_refs;
            [[nodiscard]] deren::promise::rhi::error load_resource_group(deren::promise::rhi::resource_group_ref const&) override;
            deren::utility::vessel::vector<deren::promise::rhi::texture_group_ref> group_refs;
            deren::utility::vessel::vector<std::shared_ptr<owned_command_buffer>> secondary_refs;
            [[nodiscard]] deren::promise::rhi::error load_texture_group(deren::promise::rhi::texture_group_ref const& group) override;
            /// `use()` ANSWERS with an `error` now (it does not drop a barrier silently); this flag is only
            /// about how often the backend spells out the REASON for a refusal, so a per-frame caller
            /// cannot turn one broken pair into a log flood
            bool unexpected_use_logged = false;
            /// the A5 dump (both sides of each barrier) is emitted once per pair per process
            std::uint32_t shadow_gate_dumped = 0;

            /// the native command buffer this list records into: the owned buffer's own handle (`target`) or
            /// the frame slot's while a frame is in flight. `get()` (the shared Vulkan class) answers the same
            /// handle for callers that cast the contract reference instead of using this backend-internal name.
            [[nodiscard]] VkCommandBuffer native() const noexcept;

            /// THE FRAME'S BUFFER IS THE CORE'S, so the four lifecycle verbs take the BORROWED shape
            /// (frame_image_slot sets the precedent): release() refuses with a ONE-TIME named log, because
            /// a caller that wrapped this view had a bug, and begin/end/execute answer unsupported by name
            /// because the FRAME LOOP owns those steps. The recording series below is the frame's own.
            mutable bool borrowed_lifecycle_logged = false;
            void release() noexcept override;
            [[nodiscard]] deren::promise::rhi::error begin_recording(deren::promise::rhi::command_buffer_begin_info const& info) override;
            [[nodiscard]] deren::promise::rhi::error end_recording() noexcept override;
            /// abi 36: merge a batch of mergeable segments into this recording buffer.
            [[nodiscard]] deren::promise::rhi::error execute(std::span<deren::promise::rhi::mergeable_command_buffer_ref const> batch) override;
            /// abi 36: merge a batch of mergeable segments into this recording buffer.

            [[nodiscard]] deren::promise::rhi::error use(deren::promise::rhi::image const& resource, deren::promise::rhi::image_use from, deren::promise::rhi::image_use to) noexcept override;
            [[nodiscard]] deren::promise::rhi::error copy_image_to_buffer(deren::promise::rhi::buffer& destination,
                                                                          deren::promise::rhi::image const& source,
                                                                          deren::promise::rhi::image_copy_region const& region) noexcept override;
            // ---- the GPU timing recording verbs (abi 14): this list IS the frame's slot, so the
            // timing range and the marks ride the list rather than a raw slot index ----
            [[nodiscard]] deren::promise::rhi::error begin_gpu_timing() noexcept override;
            [[nodiscard]] deren::promise::rhi::error mark_gpu_timing(std::uint32_t mark_index, std::string_view stage_name) noexcept override;
            // ---- the portable record series (abi 20): the recording surface's own vocabulary,
            // translated here (the only place that reads Vulkan structures) and recorded into
            // `native()` - the frame's slot buffer or an owned buffer's, per `target` ----
            [[nodiscard]] deren::promise::rhi::error begin_rendering(deren::promise::rhi::rendering_info const& info) override;
            void end_rendering() noexcept override;
            [[nodiscard]] deren::promise::rhi::error bind_pipeline(deren::promise::rhi::pipeline const& handle) override;
            [[nodiscard]] deren::promise::rhi::error bind_vertex_buffer(deren::promise::rhi::buffer const& handle, std::uint64_t offset) override;
            [[nodiscard]] deren::promise::rhi::error bind_index_buffer(deren::promise::rhi::buffer const& handle, std::uint64_t offset, deren::promise::rhi::index_type type) override;
            void draw(std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t first_vertex, std::uint32_t first_instance) noexcept override;
            void draw_indexed(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index, std::int32_t vertex_offset, std::uint32_t first_instance) noexcept override;
            void dispatch(std::uint32_t groups_x, std::uint32_t groups_y, std::uint32_t groups_z) noexcept override;
            [[nodiscard]] deren::promise::rhi::error dispatch_indirect(deren::promise::rhi::buffer const& arguments, std::uint64_t offset) override;
            void draw_mesh_tasks(std::uint32_t groups_x, std::uint32_t groups_y, std::uint32_t groups_z) noexcept override;
            [[nodiscard]] deren::promise::rhi::error draw_indirect(deren::promise::rhi::buffer const&, std::uint64_t, std::uint32_t, std::uint32_t) override;
            [[nodiscard]] deren::promise::rhi::error draw_mesh_tasks_indirect(deren::promise::rhi::buffer const& argument_buffer, std::uint64_t offset, std::uint32_t count, std::uint32_t stride) override;
            /// abi 24: the ray-tracing LAUNCH, through the entry point resolved once at startup
            /// (`core::ray_trace_launch`). It records nothing when the device published no `vkCmdTraceRaysKHR`.
            /// TIER-1 RECORDING VERBS SINCE ABI 26 (the frame's buffer and a caller-owned one share them, exactly
            /// as `draw`/`trace_rays` are shared): `frame_commands` is the base of `owned_command_buffer`, so one
            /// implementation serves both.
            [[nodiscard]] deren::promise::rhi::error build_acceleration_structure(deren::promise::rhi::acceleration_structure& target) override;
            [[nodiscard]] deren::promise::rhi::error refit_acceleration_structure(deren::promise::rhi::acceleration_structure& target) override;
            [[nodiscard]] deren::promise::rhi::error build_micromap(deren::promise::rhi::micromap& target) override;

            void trace_rays(deren::promise::rhi::shader_binding_table_region const& raygen, deren::promise::rhi::shader_binding_table_region const& miss,
                            deren::promise::rhi::shader_binding_table_region const& hit, deren::promise::rhi::shader_binding_table_region const& callable,
                            std::uint32_t width, std::uint32_t height, std::uint32_t depth) noexcept override;
            void set_viewport(deren::promise::rhi::viewport const& vp) noexcept override;
            void set_scissor(deren::promise::rhi::rect const& scissor) noexcept override;
            void set_cull_mode(deren::promise::rhi::cull_mode mode) noexcept override;
            void set_depth_write(bool enable) noexcept override;
            void set_depth_bias(float constant_factor, float slope_factor, float clamp) noexcept override;
            [[nodiscard]] deren::promise::rhi::error barrier(deren::promise::rhi::barrier_group const& group) override;
            [[nodiscard]] deren::promise::rhi::error barrier(deren::promise::rhi::image_barrier const& one) override;
            [[nodiscard]] deren::promise::rhi::error copy_image(deren::promise::rhi::image_copy const& copy) override;
            [[nodiscard]] deren::promise::rhi::error copy_buffer(deren::promise::rhi::buffer& destination, deren::promise::rhi::buffer const& source, std::uint64_t size, std::uint64_t source_offset, std::uint64_t destination_offset) override;
            [[nodiscard]] deren::promise::rhi::error clear_color_image(deren::promise::rhi::image const& target, deren::utility::vessel::array<float, 4> const& color, deren::promise::rhi::subresource_range const& range) override;
        };

        /// A command buffer the caller OWNS, as the contract's `command_buffer` (abi 15).
        ///
        /// THE HEAP OBJECT THE FACTORY MAKES: it IS a `frame_commands` (one inheritance, not a composed
        /// member) whose `target` is its own `vk_command_buffer`, so the whole record series and the
        /// frame-scoped verbs are the SAME implementation the frame's borrowed buffer uses - one
        /// definition, and the `target` field is the only thing that tells the two apart. `release()` is
        /// the contract's one-reference drop and the matching delete, exactly like `owned_buffer`.
        ///
        /// NO SECOND `owner` MEMBER: the base's `owner` is the one every inherited verb reads
        /// (`use`, `copy_image_to_buffer`, the timing pair, the record series), and a derived member of
        /// the same name would SHADOW it - which is what made `create_command_buffer`'s
        /// `answer->owner = this` set a field the base never looked at, and the next inherited verb
        /// dereference a null core. The factory sets THIS one.
        struct owned_command_buffer final : frame_commands {
            owned_command_buffer() noexcept
                : frame_commands(concrete::command_buffer) {
            }
            /// the buffer and its command pool; the pool dies with this object
            vk_command_buffer buffer;
            deren::promise::rhi::command_buffer_kind kind = deren::promise::rhi::command_buffer_kind::primary;
            std::atomic<std::uint32_t> references{1};
            std::atomic<std::uint32_t> recording_users{0};
            std::atomic<std::uint32_t> pending{0};

            /// one log per buffer, not one per call: a begin that repeats a refusal must not flood the log
            bool refused_chain_logged = false;

            void release() noexcept override;
            [[nodiscard]] deren::promise::rhi::error begin_recording(deren::promise::rhi::command_buffer_begin_info const& info) override;
            [[nodiscard]] deren::promise::rhi::error end_recording() noexcept override;
            /// abi 36: the owned primary is a merge target too (only a primary); the base implementation records
            /// the batch, this override adds the kind guard the API requires.
            [[nodiscard]] deren::promise::rhi::error execute(std::span<deren::promise::rhi::mergeable_command_buffer_ref const> batch) override;
        };

        /// THE BATCH-ONLY COMMAND BUFFER (contract: `mergeable_command_buffer`). It reuses the owned
        /// secondary's ENTIRE recording implementation by composition: `inner` is a real `owned_command_buffer`
        /// created at the secondary level and held through a `shared_ptr`, so a batch submission can retain it
        /// after the caller drops the mergeable. Every recording verb forwards to it.
        ///
        /// THE BATCH (`core::submit(std::span<...>)`) records these secondaries into a backend-owned primary and
        /// hands that primary to the EXISTING owned-primary submission path: retention, fences, pending
        /// counters and the one-shot rules are therefore the same machinery a caller-owned primary already
        /// uses, and nothing about the frame path moves.
        struct owned_mergeable_command_buffer final : deren::vulkan::mergeable_command_buffer {
            using rhi_error = deren::promise::rhi::error;
            using rhi_begin_info = deren::promise::rhi::command_buffer_begin_info;
            using rhi_image = deren::promise::rhi::image;
            using rhi_image_use = deren::promise::rhi::image_use;
            using rhi_buffer = deren::promise::rhi::buffer;
            using rhi_image_copy_region = deren::promise::rhi::image_copy_region;
            using rhi_rendering_info = deren::promise::rhi::rendering_info;
            using rhi_pipeline = deren::promise::rhi::pipeline;
            using rhi_index_type = deren::promise::rhi::index_type;
            using rhi_sbt_region = deren::promise::rhi::shader_binding_table_region;
            using rhi_acceleration_structure = deren::promise::rhi::acceleration_structure;
            using rhi_micromap = deren::promise::rhi::micromap;
            using rhi_viewport = deren::promise::rhi::viewport;
            using rhi_rect = deren::promise::rhi::rect;
            using rhi_cull_mode = deren::promise::rhi::cull_mode;
            using rhi_barrier_group = deren::promise::rhi::barrier_group;
            using rhi_image_barrier = deren::promise::rhi::image_barrier;
            using rhi_image_copy = deren::promise::rhi::image_copy;
            using rhi_texture_group_ref = deren::promise::rhi::texture_group_ref;
            using rhi_subresource_range = deren::promise::rhi::subresource_range;
            using rhi_array4f = deren::utility::vessel::array<float, 4>;

            owned_mergeable_command_buffer() noexcept
                : deren::vulkan::mergeable_command_buffer(deren::vulkan::mergeable_command_buffer_name) {
            }
            core* owner = nullptr;
            /// the secondary this mergeable records into (shared, so the batch can retain it)
            std::shared_ptr<owned_command_buffer> inner;

            void release() noexcept override;
            [[nodiscard]] rhi_error begin_recording(rhi_begin_info const& info) override {
                return inner != nullptr ? inner->begin_recording(info) : rhi_error::not_ready;
            }
            [[nodiscard]] rhi_error end_recording() noexcept override {
                return inner != nullptr ? inner->end_recording() : rhi_error::not_ready;
            }
            [[nodiscard]] rhi_error apply_operations() noexcept override {
                return inner ? inner->apply_operations() : rhi_error::not_ready;
            }
            [[nodiscard]] rhi_error prepare_native() noexcept override {
                return inner ? inner->prepare_native() : rhi_error::not_ready;
            }
            void finish_native() noexcept override {
                if (inner)
                    inner->finish_native();
            }
            [[nodiscard]] rhi_error use(rhi_image const& resource, rhi_image_use from, rhi_image_use to) noexcept override {
                return inner->use(resource, from, to);
            }
            [[nodiscard]] rhi_error copy_image_to_buffer(rhi_buffer& destination, rhi_image const& source, rhi_image_copy_region const& region) noexcept override {
                return inner->copy_image_to_buffer(destination, source, region);
            }
            [[nodiscard]] rhi_error begin_gpu_timing() noexcept override {
                return inner->begin_gpu_timing();
            }
            [[nodiscard]] rhi_error mark_gpu_timing(std::uint32_t mark_index, std::string_view stage_name) noexcept override {
                return inner->mark_gpu_timing(mark_index, stage_name);
            }
            [[nodiscard]] rhi_error begin_rendering(rhi_rendering_info const& info) override {
                return inner->begin_rendering(info);
            }
            void end_rendering() noexcept override {
                inner->end_rendering();
            }
            [[nodiscard]] rhi_error bind_pipeline(rhi_pipeline const& handle) override {
                return inner->bind_pipeline(handle);
            }
            [[nodiscard]] rhi_error bind_vertex_buffer(rhi_buffer const& handle, std::uint64_t offset) override {
                return inner->bind_vertex_buffer(handle, offset);
            }
            [[nodiscard]] rhi_error bind_index_buffer(rhi_buffer const& handle, std::uint64_t offset, rhi_index_type type) override {
                return inner->bind_index_buffer(handle, offset, type);
            }
            void draw(std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t first_vertex, std::uint32_t first_instance) noexcept override {
                inner->draw(vertex_count, instance_count, first_vertex, first_instance);
            }
            void draw_indexed(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index, std::int32_t vertex_offset, std::uint32_t first_instance) noexcept override {
                inner->draw_indexed(index_count, instance_count, first_index, vertex_offset, first_instance);
            }
            [[nodiscard]] rhi_error draw_indirect(rhi_buffer const& arguments, std::uint64_t offset, std::uint32_t count, std::uint32_t stride) override {
                return inner->draw_indirect(arguments, offset, count, stride);
            }
            [[nodiscard]] rhi_error dispatch_indirect(rhi_buffer const& arguments, std::uint64_t offset) override {
                return inner->dispatch_indirect(arguments, offset);
            }
            void dispatch(std::uint32_t groups_x, std::uint32_t groups_y, std::uint32_t groups_z) noexcept override {
                inner->dispatch(groups_x, groups_y, groups_z);
            }
            void draw_mesh_tasks(std::uint32_t groups_x, std::uint32_t groups_y, std::uint32_t groups_z) noexcept override {
                inner->draw_mesh_tasks(groups_x, groups_y, groups_z);
            }
            [[nodiscard]] rhi_error draw_mesh_tasks_indirect(rhi_buffer const& argument_buffer, std::uint64_t offset, std::uint32_t count, std::uint32_t stride) override {
                return inner->draw_mesh_tasks_indirect(argument_buffer, offset, count, stride);
            }
            void trace_rays(rhi_sbt_region const& raygen, rhi_sbt_region const& miss, rhi_sbt_region const& hit,
                            rhi_sbt_region const& callable, std::uint32_t width, std::uint32_t height, std::uint32_t depth) noexcept override {
                inner->trace_rays(raygen, miss, hit, callable, width, height, depth);
            }
            [[nodiscard]] rhi_error build_acceleration_structure(rhi_acceleration_structure& target) override {
                return inner->build_acceleration_structure(target);
            }
            [[nodiscard]] rhi_error refit_acceleration_structure(rhi_acceleration_structure& target) override {
                return inner->refit_acceleration_structure(target);
            }
            [[nodiscard]] rhi_error build_micromap(rhi_micromap& target) override {
                return inner->build_micromap(target);
            }
            void set_viewport(rhi_viewport const& vp) noexcept override {
                inner->set_viewport(vp);
            }
            void set_scissor(rhi_rect const& scissor) noexcept override {
                inner->set_scissor(scissor);
            }
            void set_cull_mode(rhi_cull_mode mode) noexcept override {
                inner->set_cull_mode(mode);
            }
            void set_depth_write(bool enable) noexcept override {
                inner->set_depth_write(enable);
            }
            void set_depth_bias(float constant_factor, float slope_factor, float clamp) noexcept override {
                inner->set_depth_bias(constant_factor, slope_factor, clamp);
            }
            [[nodiscard]] rhi_error barrier(rhi_barrier_group const& group) override {
                return inner->barrier(group);
            }
            [[nodiscard]] rhi_error barrier(rhi_image_barrier const& one) override {
                return inner->barrier(one);
            }
            [[nodiscard]] rhi_error copy_image(rhi_image_copy const& copy) override {
                return inner->copy_image(copy);
            }
            [[nodiscard]] rhi_error copy_buffer(rhi_buffer& destination, rhi_buffer const& source, std::uint64_t size, std::uint64_t source_offset, std::uint64_t destination_offset) override {
                return inner->copy_buffer(destination, source, size, source_offset, destination_offset);
            }
            [[nodiscard]] rhi_error clear_color_image(rhi_image const& target, rhi_array4f const& color, rhi_subresource_range const& range) override {
                return inner->clear_color_image(target, color, range);
            }
            [[nodiscard]] rhi_error push_constants(std::uint32_t offset, std::span<std::byte const> data) noexcept override {
                return inner->push_constants(offset, data);
            }
            [[nodiscard]] rhi_error load_resource_group(deren::promise::rhi::resource_group_ref const& group) override {
                return inner->load_resource_group(group);
            }
            [[nodiscard]] rhi_error load_texture_group(rhi_texture_group_ref const& group) override {
                return inner->load_texture_group(group);
            }
        };

        /// The swapchain image the frame in flight draws into, as the contract's `image`.
        ///
        /// A BORROWED VIEW, NOT AN OWNED HANDLE: this object is a member of the core, the core owns the
        /// swapchain image behind it, and the contract's `release()` must never drop a reference for it
        /// (the caller never held one). That is why the override below PANICS instead of doing nothing
        /// (rhi.api_core.cppm's ownership note): a caller that wrapped a borrowed view in `unique_object`
        /// has a bug, and a lifecycle precondition violation terminates loudly rather than being logged.
        struct frame_image_slot final : deren::promise::rhi::image {
            frame_image_slot() noexcept
                : deren::promise::rhi::image(concrete::frame_image) {
            }
            core* owner = nullptr;

            [[nodiscard]] ::deren::promise::rhi::image_extent extent() const noexcept override;
            [[nodiscard]] deren::promise::rhi::image_format format() const noexcept override;
            /// abi 7's make_view on a BORROWED image: what is borrowed is the IMAGE - the VIEW is a new
            /// owned backend object (abi 16), returned as the factory product the caller releases. It
            /// refuses (nullptr, with a named log) when no image has been acquired yet, or when the range
            /// is not this single-layer/single-mip image, and the caller must release it before the
            /// swapchain is recreated.
            [[nodiscard]] deren::promise::rhi::unique_object<deren::promise::rhi::image_view> make_view(deren::promise::rhi::image_view_desc const& desc) override;
            /// BORROWED: a wrapped borrowed view is a precondition violation and PANICS (see the note above)
            void release() noexcept override;
            /// abi 25's read-back on a BORROWED view: a swapchain image can only be host-copied when the
            /// surface listed `HOST_TRANSFER` in its `supportedUsageFlags`, which this one does not (measured
            /// and logged at swapchain creation) - so this answers the contract's named `unsupported` rather
            /// than faking content. A caller that wants pixels reads an image this backend CREATED.
            [[nodiscard]] std::expected<deren::promise::rhi::image_content, deren::promise::rhi::error> get_content(
                deren::promise::rhi::image_copy_region const& region) const override;
            /// the raw handle the backend's own recording needs (never carried across the boundary)
            [[nodiscard]] VkImage handle() const noexcept;
        };

        // A snapshot keeps the ROOT alive (keep_alive, a shared copy), while the core only caches a weak
        // reference to the snapshot.
        struct frame_snapshot final : deren::promise::rhi::image {
            frame_snapshot() noexcept
                : deren::promise::rhi::image(concrete::frame_snapshot) {
            }
            /// the concrete core (its frame state is what `current()` asks about)...
            core* owner = nullptr;
            /// ...kept alive by the root handle this snapshot was made under
            deren::promise::rhi::shared_object<deren::promise::rhi::api_core> keep_alive;
            /// this snapshot's own control block, so `share()` hands out the SAME one
            deren::promise::rhi::shared_object<frame_snapshot>::weak self;
            VkImage native_handle = VK_NULL_HANDLE;
            deren::promise::rhi::image_extent captured_extent{};
            deren::promise::rhi::image_format captured_format = deren::promise::rhi::image_format::unknown;
            std::uint64_t epoch = 0;
            ~frame_snapshot() noexcept override;
            [[nodiscard]] bool current() const noexcept;
            [[nodiscard]] deren::promise::rhi::image_extent extent() const noexcept override {
                return captured_extent;
            }
            [[nodiscard]] deren::promise::rhi::image_format format() const noexcept override {
                return captured_format;
            }
            [[nodiscard]] deren::promise::rhi::unique_object<deren::promise::rhi::image_view> make_view(deren::promise::rhi::image_view_desc const&) override;
            [[nodiscard]] deren::promise::rhi::shared_object<deren::promise::rhi::image> share() override {
                return this->self.lock();
            }
            [[nodiscard]] std::expected<deren::promise::rhi::image_content, deren::promise::rhi::error> get_content(deren::promise::rhi::image_copy_region const&) const override;
            void release() noexcept override;
        };
        struct frame_resources_view final : deren::promise::rhi::frame_resources {
            frame_resources_view() noexcept
                : deren::promise::rhi::frame_resources(concrete::frame_resources) {
            }
            core* owner = nullptr;
            [[nodiscard]] deren::promise::rhi::shared_object<deren::promise::rhi::image> retain_image() noexcept override;
        };
        frame_resources_view frame_resources;
        deren::promise::rhi::shared_object<frame_snapshot>::weak frame_snapshot_cache;
        deren::utility::vessel::unordered_set<deren::promise::rhi::image const*> frame_snapshots;
        std::uint64_t frame_acquire_epoch = 0;

        /// The backend's host-visible read-back slot, as the contract's `buffer`.
        ///
        /// A borrowed view for exactly the reasons `frame_image_slot` is; see its note.
        struct frame_readback_slot final : deren::promise::rhi::buffer {
            frame_readback_slot() noexcept
                : deren::promise::rhi::buffer(concrete::frame_readback_buffer) {
            }
            core* owner = nullptr;

            [[nodiscard]] std::uint64_t size() const noexcept override;
            [[nodiscard]] std::span<std::byte> mapped() noexcept override;
            /// BORROWED: a wrapped borrowed buffer is a precondition violation and PANICS (see frame_image_slot)
            void release() noexcept override;
            /// the raw handle `copy_image_to_buffer()` records into
            [[nodiscard]] VkBuffer handle() const noexcept;
        };

        /// The frame ring's cursor, as the contract's `frame_walker` (abi 13).
        ///
        /// A BORROWED VIEW like the two above - a member of the core, handed out by `walk_frames()`,
        /// never released by the caller (the type has no `release()`, so `unique_object` refuses it).
        /// `wait_and_acquire()` composes the frame prologue this class already owned: wait the slot's
        /// timeline, LATCH the slot's last completed frame's GPU timings (the step between wait and
        /// acquire, so an OUT_OF_DATE-skipped frame is still collected - today's order, now the
        /// backend's), acquire, and report the decision through `frame_open_info` - which is where
        /// today's two information losses are repaired: the acquire's OUT_OF_DATE stops being
        /// squashed into a zeroed `submit_info`, and the timeline wait's `vkWaitSemaphores` result
        /// stops being dropped. The existing methods keep their spellings and become what the face
        /// is spelled FROM.
        struct frame_walker_view final : deren::promise::rhi::frame_walker {
            core* owner = nullptr;

            [[nodiscard]] std::uint32_t slot_count() const noexcept override;
            [[nodiscard]] std::uint32_t position() const noexcept override;
            [[nodiscard]] deren::promise::rhi::frame_open_info wait_and_acquire() override;
            void walk_to_next() noexcept override;
        };

        /// The presentation surface, as the contract's `swapchain`: a BORROWED view like the frame
        /// views above (a `release()` on it logs once and drops no reference). `recreate()` and
        /// `extent()` are abi 14's verbs - the rebuild is this object's own behaviour, re-derived
        /// from the window it owns, and the extent answers what presentation shows.
        struct swapchain_view final : deren::promise::rhi::swapchain {
            swapchain_view() noexcept
                : deren::promise::rhi::swapchain(concrete::swapchain) {
            }
            core* owner = nullptr;
            mutable bool borrowed_release_logged = false;

            /// BORROWED: logs once and drops no reference (see frame_image_slot's note)
            void release() noexcept override;
            [[nodiscard]] deren::promise::rhi::error recreate() override;
            [[nodiscard]] ::deren::promise::rhi::image_extent extent() const noexcept override;
            [[nodiscard]] deren::promise::rhi::image_format format() const noexcept override;
        };

        /// The last completed frame's GPU timing report, as the contract's `gpu_profiler` (abi 13).
        ///
        /// A borrowed view like `frame_walker_view`. The LATCHED state below is the report's one
        /// source: `frame_walker_view::wait_and_acquire()` calls `latch()` for the slot it just
        /// waited - the moment its previous submission is guaranteed complete - and the getters
        /// answer that snapshot until the next latch replaces it. The snapshot keeps RAW ticks and
        /// the mark names (static text the engine passed at mark time, held as a view - the
        /// `window_title` rule); durations are converted to nanoseconds on read.
        struct gpu_profiler_view final : deren::promise::rhi::gpu_profiler {
            core* owner = nullptr;

            /// the latched frame: its mark count, the raw timestamp ticks, and the static names the
            /// engine's marks carried. `latch_failed` reports a failed timestamp read-back (the count
            /// is still host-known, so the getters can name the failure instead of reporting silence).
            std::uint32_t latched_mark_count = 0;
            bool latch_failed = false;
            deren::utility::vessel::array<std::uint64_t, gpu_timing_mark_capacity> latched_ticks = {};
            deren::utility::vessel::array<std::string_view, gpu_timing_mark_capacity> latched_names = {};

            /// latch what THIS slot's last completed submission measured (no new submission since the
            /// previous latch => the snapshot stays as it was). Called between the slot's timeline
            /// wait and the image acquire - the position that gives the timing line its meaning.
            void latch(std::uint32_t slot) noexcept;

            [[nodiscard]] std::uint32_t stage_count() const noexcept override;
            [[nodiscard]] deren::promise::rhi::error get_stage_info(std::uint32_t index, std::string_view* name,
                                                                    std::uint64_t* duration_ns) const noexcept override;
        };

        /// AN OWNED BUFFER: what `create_buffer()` hands the caller.
        ///
        /// THE OPPOSITE OF THE TWO VIEWS ABOVE, and the difference is the whole ownership model:
        /// this object is HEAP-ALLOCATED BY THE FACTORY, so the caller's one `release()` can give the
        /// reference back to the allocator that made it - `release()` is `delete this`, and the
        /// destructor resets the `vk_buffer` RAII owner, which IS the allocator's reference-count
        /// decrement (rhi.api_core.cppm's ownership note: release, not necessarily destruction).
        ///
        /// It caches the size and the mapped pointer because the allocator's detail lookup is an
        /// UNLOCKED BORROW of its internal map (source/backends/vulkan/core/vma/vma.cppm's `get_buffer_detail`): the
        /// pointer into that map must not be kept, the VALUES read out of it may be. That is the rule
        /// the ownership analysis says every contract query has to follow.
        struct owned_buffer final : deren::promise::rhi::lazy_buffer {
            owned_buffer() noexcept
                : deren::promise::rhi::lazy_buffer(concrete::buffer) {
            }
            std::atomic<std::uint32_t> references{1};
            deren::promise::rhi::lazy_data source;
            detail::lazy_loading<deren::promise::rhi::lazy_state, deren::promise::rhi::error> loading;
            deren::promise::rhi::buffer_desc description;
            deren::utility::vessel::string debug_name;
            [[nodiscard]] deren::promise::rhi::error unload() noexcept override;
            [[nodiscard]] deren::promise::rhi::lazy_state state() const noexcept override {
                return loading.state();
            }
            deren::vulkan::vk_buffer owned = {};
            /// the Vulkan handle, cached at creation: `vk_buffer::handle()` is the ALLOCATOR's registry
            /// key (a uint64), not the `VkBuffer`, and reaching the real handle means a detail lookup -
            /// an unlocked borrow whose VALUES may be copied but whose pointer must not be kept.
            std::uint64_t identity = 0;
            VkBuffer native = VK_NULL_HANDLE;
            std::uint64_t size_bytes = 0;
            void* mapped_bytes = nullptr;
            /// whether the descriptor asked for `buffer_flag::device_address`; the ability answers 0
            /// when it did not, because Vulkan only gives an address to a buffer created with the usage
            /// - asking for one this object never declared is the caller's bug, not a backend guess.
            bool addressable = false;
            bool readback = false;
            deren::promise::rhi::buffer_usage declared_usage = deren::promise::rhi::buffer_usage::vertex;
            core* owner = nullptr;
            /// the capability bits the descriptor declared, kept for the diagnostic a failed address
            /// query needs (nothing else reads it)
            deren::promise::rhi::buffer_flags declared_flags = deren::promise::rhi::no_buffer_flags;

            [[nodiscard]] std::uint64_t size() const noexcept override;
            [[nodiscard]] std::span<std::byte> mapped() noexcept override;
            /// give the reference back: `delete this`, whose destructor resets `owned`
            void release() noexcept override;
        };

        /// AN OPACITY MICROMAP THIS BACKEND OWNS (tier-1 since abi 27, plan S1's P4): the structure itself, the
        /// memory it lives in, the scratch a build needs, and THE SETUP BUFFERS THE BUILD READS.
        ///
        /// THE SETUP BUFFERS ARE THE POINT OF THIS OBJECT EXISTING: the API requires the `data` and
        /// `triangleArray` addresses to be 256-BYTE ALIGNED (a requirement on an ADDRESS, which the engine used
        /// to satisfy by hand: allocate with slack, read the address, compute the first aligned offset inside).
        /// A backend that owns both the allocation and the address cannot get that wrong, and a caller that
        /// hands over spans never has to know it exists.
        ///
        /// THE ATTACHMENT THE TRAVERSAL READS carries the index array, so THAT buffer lives here too (one
        /// element per micro-triangle, which is what makes the engine's `micromap_resource` shrink to a handle).
        struct owned_micromap final : deren::promise::rhi::micromap {
            owned_micromap() noexcept
                : deren::promise::rhi::micromap(concrete::micromap) {
            }
            core* owner = nullptr;
            VkMicromapEXT native = VK_NULL_HANDLE;
            deren::promise::rhi::buffer* storage = nullptr; ///< the micromap's own memory (MICROMAP_STORAGE)
            deren::promise::rhi::buffer* scratch = nullptr; ///< the build's scratch (only its address is read)
            std::uintptr_t scratch_address = 0;
            deren::promise::rhi::buffer* data = nullptr; ///< the attributes, 256-aligned inside it
            std::uintptr_t data_address = 0;
            deren::promise::rhi::buffer* triangles = nullptr; ///< one record per micro-triangle
            std::uintptr_t triangles_address = 0;
            deren::promise::rhi::buffer* indices = nullptr; ///< the index array the ATTACHMENT points at
            std::uintptr_t indices_address = 0;
            VkDeviceSize triangle_array_stride = 0;
            std::uint32_t index_stride = 0;
            std::uint32_t triangle_count = 0;
            VkMicromapUsageEXT usage = {}; ///< the one usage record the build declares

            void release() noexcept override;
            ~owned_micromap() noexcept;
        };

        /// AN ACCELERATION STRUCTURE THIS BACKEND OWNS (tier-1 since abi 26): the memory it lives in, the
        /// scratch a build needs, and - for a top-level one - the instance records the CALLER writes through
        /// `write_instances()`.        ///
        /// WHAT IT REPLACES, AND WHY THE SHAPE IS THIS ONE: the engine's own `source/backends/vulkan/acceleration_structure`
        /// module used to do all of this itself (a size query, a storage buffer, one shared scratch with
        /// aligned per-geometry ranges, the instance buffers, the recording). All of it moves here, so a caller
        /// of the tier-1 interface never sees a size, an alignment or a scratch offset - which is what makes
        /// the interface portable to a backend that has no such objects at all.
        ///
        /// THE DESTRUCTOR ORDER IS THE ONE THING WORTH STATING: the structure is destroyed BEFORE the memory it
        /// lives in (`vkDestroyAccelerationStructureKHR` first, then the buffers are released), because
        /// destroying the buffer under a live structure is a validation error and, on some drivers, worse.
        struct owned_acceleration_structure final : deren::promise::rhi::acceleration_structure {
            owned_acceleration_structure() noexcept
                : deren::promise::rhi::acceleration_structure(concrete::acceleration_structure) {
            }
            core* owner = nullptr;
            VkAccelerationStructureKHR native = VK_NULL_HANDLE;
            /// the memory the structure lives in: the BACKEND's reference, never handed to the caller
            deren::promise::rhi::buffer* storage = nullptr;
            /// the build's scratch, sized ONCE at creation for the worst case (a bottom level's geometries are
            /// fixed, a top level is sized for its capacity), so recording a build never allocates
            deren::promise::rhi::buffer* scratch = nullptr;
            std::uintptr_t scratch_address = 0;
            VkDeviceSize scratch_size = 0;
            /// what `device_address()` and `size_bytes()` answer, cached at creation
            std::uintptr_t address = 0;
            VkDeviceSize structure_size = 0;
            /// WHAT IT WAS CREATED AS: the geometry the driver's build reads, or the instances the caller writes
            bool top_level = false;
            bool refittable = false;
            deren::utility::vessel::vector<VkAccelerationStructureGeometryKHR> geometries = {};
            /// THE MICROMAP ATTACHMENTS, one per geometry that consults one. `reserve()`d to the geometry count
            /// before the geometries are built, because a geometry's `triangles.pNext` points INTO this vector and
            /// the build reads it later, at record time: a reallocation would leave that pointer dangling, and the
            /// failure mode is a traversal consulting freed memory rather than a compile error.
            deren::utility::vessel::vector<VkAccelerationStructureTrianglesOpacityMicromapEXT> micromap_attachments = {};
            /// One range per geometry, in the same order as geometries.
            deren::utility::vessel::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges = {};
            /// TOP LEVEL: the instance records, in the backend's own host-visible buffer
            deren::promise::rhi::buffer* instances = nullptr;
            void* instances_mapped = nullptr;
            std::uint32_t instance_capacity = 0;
            std::uint32_t instance_count = 0;

            [[nodiscard]] std::uint64_t device_address() const noexcept override;
            [[nodiscard]] std::uint64_t size_bytes() const noexcept override;
            [[nodiscard]] deren::promise::rhi::error write_instances(std::span<deren::promise::rhi::acceleration_structure_instance const> records) override;
            /// give the reference back: `delete this`, whose destructor destroys the structure and THEN
            /// releases the memory (see the note above)
            void release() noexcept override;
            ~owned_acceleration_structure() noexcept;
        };

        /// AN OWNED IMAGE: what `create_image()` hands the caller (abi 7's image face, §17's design).
        ///
        /// THE SAME SHAPE AS `owned_buffer`: heap-allocated by the factory, `release()` is `delete this`,
        /// and the destructor gives the allocator reference back through the `vk_image` RAII owner - which
        /// is the allocator's reference-count decrement, so a content-deduplicated image survives while
        /// any view or handle still refers to it. The escape's `native_image()` (deleted in abi 42) was the
        /// last external reader of the allocation's registry key; the backend reads its members directly.
        struct owned_image final : deren::promise::rhi::lazy_image {
            owned_image() noexcept
                : deren::promise::rhi::lazy_image(concrete::image) {
            }
            std::atomic<std::uint32_t> references{1}; // factory ownership + shared control blocks
            std::mutex shared_owner_mutex;
            deren::promise::rhi::shared_object<deren::promise::rhi::image>::weak shared_owner;
            /// the core that created this image: `make_view()` needs the device, and every owned
            /// object that calls back into the backend carries its owner (the shape `buffer_address_view`
            /// uses)
            core* owner = nullptr;
            deren::promise::rhi::lazy_data source;
            detail::lazy_loading<deren::promise::rhi::lazy_state, deren::promise::rhi::error> loading;
            deren::promise::rhi::image_desc description;
            deren::utility::vessel::string debug_name;
            [[nodiscard]] deren::promise::rhi::error unload() noexcept override;
            [[nodiscard]] deren::promise::rhi::lazy_state state() const noexcept override {
                return loading.state();
            }
            deren::vulkan::vk_image owned = {};
            /// the `VkImage`, copied out of the allocator's detail map AT CREATION (an unlocked borrow:
            /// the pointer into the map must not be kept, the values read out of it may - the rule
            /// `owned_buffer`'s comment states)
            VkImage native_handle = VK_NULL_HANDLE;
            /// the RESOLVED format: the descriptor's `depth` role became the device's concrete depth
            /// format at creation, and a view of this image needs that concrete format, not the role
            VkFormat resolved_format = VK_FORMAT_UNDEFINED;
            /// the shape the descriptor declared, cached: `extent()` answers from these rather than
            /// re-borrowing the allocator's map on every query
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint32_t mip_levels = 1;
            std::uint32_t array_layers = 1;
            /// whether the descriptor declared `cube_compatible`: the only case a six-layer image
            /// views as a CUBE rather than a 2D array
            bool cube_compatible = false;
            deren::promise::rhi::image_flags declared_flags = deren::promise::rhi::no_image_flags;
            /// the contract format the descriptor named (the `depth` role stays the role here; the
            /// concrete spelling lives in `resolved_format`)
            deren::promise::rhi::image_format declared_format = deren::promise::rhi::image_format::unknown;

            [[nodiscard]] ::deren::promise::rhi::image_extent extent() const noexcept override;
            [[nodiscard]] deren::promise::rhi::image_format format() const noexcept override;
            [[nodiscard]] deren::promise::rhi::unique_object<deren::promise::rhi::image_view> make_view(deren::promise::rhi::image_view_desc const& desc) override;
            [[nodiscard]] deren::promise::rhi::shared_object<deren::promise::rhi::image> share() override;
            /// abi 25's read-back: the image's CONTENT in host memory, performed by the IMPLEMENTATION
            /// (`vkCopyImageToMemoryEXT`) - no staging buffer, no copy command, no submission. It refuses with
            /// the contract's named `unsupported` when the descriptor did not declare
            /// `image_flag::host_transfer` (the API requires the bit on the source) or the device's host-copy
            /// ability is absent, and with `invalid_argument` when this is not an owned image.
            [[nodiscard]] std::expected<deren::promise::rhi::image_content, deren::promise::rhi::error> get_content(
                deren::promise::rhi::image_copy_region const& region) const override;
            /// Drop one retained reference; the final release resets `owned`.
            void release() noexcept override;
        };

        /// An owned view retains its parent until after destroying the native view.
        /// Views of borrowed swapchain images carry no parent owner.
        struct owned_image_view final : deren::promise::rhi::image_view {
            owned_image_view() noexcept
                : deren::promise::rhi::image_view(concrete::image_view) {
            }
            deren::promise::rhi::shared_object<deren::promise::rhi::image> parent_image;
            VkImageView native_view = VK_NULL_HANDLE;
            std::shared_ptr<void> lazy_reference;
            deren::promise::rhi::image_view_desc description;
            std::mutex loading_mutex;
            /// the device the view was created on; the destructor needs it and the object must not
            /// outlive the core it came from (the core's own cleanup order guarantees that)
            VkDevice device = VK_NULL_HANDLE;

            /// the `VkImageView` is a raw handle with no RAII owner of its own, so the view's
            /// destruction is this object's destructor
            ~owned_image_view() noexcept override;
            [[nodiscard]] deren::promise::rhi::shared_object<deren::promise::rhi::image> get_image() const noexcept override {
                return parent_image;
            }
            /// give the view back: `delete this`, whose destructor destroys the `VkImageView`
            void release() noexcept override;
        };

        /// AN OWNED SAMPLER: what `create_sampler()` hands the caller, the same shape as the view.
        struct owned_sampler final : deren::promise::rhi::sampler {
            owned_sampler() noexcept
                : deren::promise::rhi::sampler(concrete::sampler) {
            }
            core* owner = nullptr;
            VkSamplerCreateInfo info{};
            VkSampler native_sampler_handle = VK_NULL_HANDLE;
            VkDevice device = VK_NULL_HANDLE;

            /// same rule as the view: the raw handle dies with this object
            ~owned_sampler() noexcept override;
            /// `delete this`, whose destructor destroys the `VkSampler`
            void release() noexcept override;
        };

        /// AN OWNED SHADER: what `create_shader()` hands the caller (abi 8's pipeline face). The engine
        /// builds its pipelines from SPIR-V through `create_pipeline()` (`pipeline_desc`), so no native
        /// module crosses the contract - the escape's borrow was deleted in abi 42 - and the factory
        /// remains for a caller that wants the module as a contract handle.
        struct owned_shader final : deren::promise::rhi::shader {
            owned_shader() noexcept
                : deren::promise::rhi::shader(concrete::shader) {
            }
            /// optional, not a member: the RAII types have no default state (a wrapper IS a live
            /// handle), so the factory emplaces on success
            std::optional<deren::vulkan::vk_shader_module> owned;

            /// `delete this`, whose destructor resets the RAII owner
            void release() noexcept override;
        };

        /// AN OWNED PIPELINE: what `create_pipeline()` hands the caller - the backend's
        /// `make_pipeline` result (vertex input derived from the SPIR-V, the dynamic viewport/scissor
        /// state, the heap-native layout-less creation) wrapped in the contract's ownership.
        struct owned_pipeline final : deren::promise::rhi::pipeline {
            deren::promise::rhi::texture_group_binding group_binding{};
            deren::utility::vessel::vector<deren::promise::rhi::resource_group_binding> resource_layout;
            owned_pipeline() noexcept
                : deren::promise::rhi::pipeline(concrete::pipeline) {
            }
            core* owner = nullptr;
            std::uint32_t group_count = 0;
            /// same optional rule as owned_shader
            std::optional<deren::vulkan::vk_pipeline> owned;
            /// the `VkPipeline`, cached at creation: `bind_pipeline` records with it and the
            /// `shader_group_access` view queries it (the escape's borrow that used to read it is deleted)
            std::uint64_t identity = 0;
            VkPipeline native_handle = VK_NULL_HANDLE;
            /// graphics or compute: the bind point `bind_pipeline` (abi 20) needs, decided at
            /// creation from the descriptor's shape and never re-guessed per bind
            VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;

            /// `delete this`, whose destructor destroys the pipeline (RAII owner)
            void release() noexcept override;
        };

        /// tier-2 `device_address`: a buffer's device address.
        ///
        /// ANNOUNCED ONLY NOW, AND THE REASON IS THE ABILITY'S OWN RULE rather than a change of heart:
        /// it used to declare an acceleration-structure address as well, and no backend could produce an
        /// acceleration structure, so the bit would have promised a service nothing could perform
        /// (rhi.extension.cppm's "every operation the ability declares must be performable"). With that
        /// operand moved to `ray_tracing` (abi 5) and buffers producible since the buffer batch, the
        /// bit is servable in the strict sense: `query_extension(device_address)` answers, and
        /// `buffer_address()` is real.
        struct buffer_address_view final : deren::promise::rhi::device_address {
            buffer_address_view() noexcept
                : deren::promise::rhi::device_address(concrete::device_address) {
            }
            core* owner = nullptr;

            [[nodiscard]] std::uint64_t buffer_address(deren::promise::rhi::buffer const& resource, std::uint64_t offset) const noexcept override;
        };

        // `frame_escape` STOOD HERE AND IS DELETED in abi 43: the GUI overlay received the RHI command_buffer
        // and cast it to the shared `denen.vulkan.command_buffer` head (see `frame_commands` above), so the
        // backend no longer implements a tier-2 escape interface at all. Gui abi 5 supersedes that hand-off:
        // the overlay now owns two mergeable segments (`owned_mergeable_command_buffer`), see the GUI contract.

        /// tier-2 `host_image_copy`: the implementation-side copy out of an image into the app's own
        /// memory (`VK_EXT_host_image_copy`), announced ONLY when the device actually has it
        /// (`host_image_copy_available`, which requires the extension, its feature AND GENERAL among the
        /// copy source layouts) - a set bit is a promise about service, and this view serves it on the
        /// images `create_image()` handed out.
        ///
        /// WIRED UP BY THE CONTRACT-ONLY RUNTIME (③-D/E step 2): the two heap probes used to call the
        /// device entry point directly off `core::copy_image_to_memory`; they now go through this, so no
        /// engine file names a Vulkan entry point the backend resolved. It is the FIRST consumer of an
        /// ability that had been declared in the contract since abi 1 and served by nobody.
        struct frame_host_copy final : deren::promise::rhi::host_image_copy {
            frame_host_copy() noexcept
                : deren::promise::rhi::host_image_copy(concrete::host_image_copy) {
            }
            core* owner = nullptr;

            [[nodiscard]] deren::promise::rhi::error copy_image_to_memory(deren::promise::rhi::image const& source,
                                                                          std::span<std::byte> destination,
                                                                          deren::promise::rhi::image_copy_region const& region) noexcept override;
        };

        /// tier-2 `device_capabilities`: WHAT THIS DEVICE CAN DO, answered from the values the constructor
        /// already QUERIED to decide what to enable (`ray_query_available`, `mesh_shader_available`,
        /// `device_properties`, `acceleration_structure_properties`, `ray_tracing_pipeline_properties`,
        /// `graphics_queue_family_index`). Not one of these methods queries anything: the engine used to make
        /// these queries itself - `vkGetPhysicalDeviceFeatures2` / `...Properties` / `...Properties2` and a
        /// queue-family walk - to re-derive facts this object already holds.
        ///
        /// ANNOUNCED UNCONDITIONALLY, because every method is answerable the moment the device exists (a
        /// device with no mesh shader answers `false`, it is not UNSERVED), so "a set bit is a promise about
        /// service" is kept in the strict sense.
        struct frame_device_capabilities final : deren::promise::rhi::device_capabilities {
            frame_device_capabilities() noexcept
                : deren::promise::rhi::device_capabilities(concrete::device_capabilities) {
            }
            core* owner = nullptr;

            [[nodiscard]] bool mesh_shader() const noexcept override;
            [[nodiscard]] bool buffer_int64_atomics() const noexcept override;
            [[nodiscard]] bool compute_indirect() const noexcept override;
            [[nodiscard]] bool ray_query() const noexcept override;
            [[nodiscard]] std::uint32_t max_push_constants_size() const noexcept override;
            [[nodiscard]] std::uint32_t graphics_queue_family() const noexcept override;
            [[nodiscard]] deren::promise::rhi::shader_binding_table_properties shader_binding_table() const noexcept override;
            [[nodiscard]] std::uint64_t acceleration_structure_scratch_alignment() const noexcept override;
            [[nodiscard]] std::uint64_t max_acceleration_structure_instances() const noexcept override;
        };

        struct frame_shader_group_access final : deren::promise::rhi::shader_group_access {
            frame_shader_group_access() noexcept
                : deren::promise::rhi::shader_group_access(concrete::shader_group_access) {
            }
            core* owner = nullptr;
            [[nodiscard]] deren::promise::rhi::error read(deren::promise::rhi::pipeline const& resource,
                                                          std::uint32_t first_group, std::uint32_t group_count, std::span<std::uint8_t> out) const noexcept override;
        };

        struct frame_heap final : deren::promise::rhi::descriptor_heap {
            frame_heap() noexcept
                : deren::promise::rhi::descriptor_heap(concrete::descriptor_heap) {
            }
            core* owner = nullptr;
            [[nodiscard]] bool ready() const noexcept override;
            [[nodiscard]] deren::promise::rhi::descriptor_heap_properties properties() const noexcept override;
            [[nodiscard]] deren::promise::rhi::heap_bindings bindings() const noexcept override;
            [[nodiscard]] deren::promise::rhi::error write_image(deren::promise::rhi::heap_image_write_info const&) noexcept override;
            [[nodiscard]] deren::promise::rhi::error write_image_native(deren::promise::rhi::heap_image_write_info const&, bool tracked) noexcept;
            [[nodiscard]] deren::promise::rhi::error write_buffer(deren::promise::rhi::heap_buffer_write_info const&) noexcept override;
            [[nodiscard]] deren::promise::rhi::error bind(deren::promise::rhi::heap_bind_info const&) const noexcept override;
        };

        /// THE SERVABLE FACE OF THE DESCRIPTOR SPACE (see `rhi::descriptor_space`): positions and portable
        /// writes for immutable texture groups. It lives beside the heap view rather than inside it because
        /// the heap speaks the DEVICE's numbers (bytes, alignments) while this speaks the ENGINE's
        /// vocabulary (a group and where it is); the group table and sampled-slot pool behind both are the
        /// same backend state (`texture_groups`), which is why one write helper serves both callers.
        struct frame_descriptor_space final : deren::promise::rhi::descriptor_space {
            frame_descriptor_space() noexcept
                : deren::promise::rhi::descriptor_space(concrete::descriptor_space) {
            }
            core* owner = nullptr;
            [[nodiscard]] bool ready() const noexcept override;
            [[nodiscard]] deren::promise::rhi::descriptor_space_properties properties() const noexcept override;
            [[nodiscard]] deren::promise::rhi::descriptor_position position(deren::promise::rhi::texture_group const& group) const noexcept override;
            [[nodiscard]] deren::promise::rhi::error write(deren::promise::rhi::descriptor_write const& info) noexcept override;
            [[nodiscard]] deren::promise::rhi::descriptor_slot_range fixed_slots() const noexcept override;
            [[nodiscard]] deren::promise::rhi::error write_image(deren::promise::rhi::descriptor_image_write const&) noexcept override;
            [[nodiscard]] deren::promise::rhi::error write_buffer(deren::promise::rhi::descriptor_buffer_write const&) noexcept override;
            [[nodiscard]] deren::promise::rhi::error write_acceleration_structure(deren::promise::rhi::descriptor_acceleration_structure_write const&) noexcept override;
            [[nodiscard]] deren::promise::rhi::error bind(deren::promise::rhi::descriptor_bind_info const&) noexcept override;
        };

        // ---- WHAT THE RECORDING SURFACE OWNS --------------------------------------------------------
        //
        // THE COMMAND BUFFERS MOVED HERE FROM THE RUNTIME. They are RAII device objects and
        // `begin_commands()` has to hand out the frame's list, so the type that owns the device owns
        // them. WHAT DID NOT MOVE: the frame's SHAPE (one primary per frame slot, allocated once at
        // construction) and its LIFECYCLE - the engine still begins, ends and submits them, and still
        // decides the present recipe (runtime.frames.cppm). The runtime borrows the container as a span.
        //
        // RELEASED BY A CLEANUP, NOT BY THE MEMBER DESTRUCTORS: a `vk_command_buffer` frees itself
        // through the device and its pool, and a `vk_buffer` through VMA's allocator - both are gone by
        // the time member destructors run, because do_cleanup() destroys them from the destructor BODY.
        // A cleanup registered last runs first (do_cleanup is LIFO) and empties these two.
        deren::utility::vessel::vector<vk_command_buffer> frame_command_buffers;
        /// the recording view `begin_commands()` answers with (one object, reused every frame)
        frame_commands commands_view;
        /// the two frame-domain views `frame_image()` / `frame_readback_buffer()` answer with
        frame_image_slot frame_image_view;
        frame_readback_slot readback_slot_view;
        /// the frame face's two views (abi 13): `walk_frames()` / `profiler()` answer with these, the
        /// same object every call. THE VIEWS' `owner` IS SET IN THE CONSTRUCTOR - a view handed out
        /// with a null owner is a null-dereference on first call (the `address_view` lesson: the
        /// spike caught it as 2 FAILs before anyone looked at a frame).
        frame_walker_view frames_view;
        gpu_profiler_view profiler_view;
        /// the presentation surface `frame_swapchain()` answers with (abi 14), a borrowed view
        swapchain_view presentation_surface;
        /// abi 39: THE MAIN-THREAD CONTEXT the GUI plugin casts (see deren.vulkan.main_context). Backend-owned
        /// and borrowed; filled on each `main_context()` call because the handles it names are stable members.
        deren::vulkan::main_context main_context_view;
        frame_heap heap_view;
        frame_descriptor_space descriptor_space_view;
        struct lazy_descriptor {
            bool image = false;
            bool published = false;
            deren::promise::rhi::descriptor_image_write image_write;
            deren::promise::rhi::image_view_desc view;
            deren::promise::rhi::descriptor_buffer_write buffer_write;
            std::shared_ptr<void> reference;
        };
        std::mutex lazy_descriptors_mutex;
        deren::utility::vessel::map<std::uint32_t, lazy_descriptor> lazy_descriptors;
        [[nodiscard]] deren::promise::rhi::error prepare_lazy_descriptors(frame_commands&) noexcept;
        std::mutex contract_images_mutex;
        std::mutex contract_structures_mutex;
        deren::utility::vessel::unordered_set<deren::promise::rhi::acceleration_structure const*> contract_structures;
        deren::utility::vessel::unordered_set<deren::promise::rhi::image const*> contract_images;
        struct owned_texture_group; // the position map below holds it by pointer; defined after the record
        struct texture_group_state {
            static constexpr std::uint32_t table_slot = 32768;
            static constexpr std::uint32_t texture_base = 32769;
            /// the descriptor space's answers (see `rhi::descriptor_space_properties`): the group table and
            /// the sampled-slot pool this state owns, with the strides the shader-facing record layout uses.
            static constexpr std::uint32_t group_capacity = 1024;
            static constexpr std::uint32_t texture_capacity = 2048;
            static constexpr std::uint32_t group_record_bytes = 80;
            static constexpr std::uint32_t texture_slot_bytes = 64;
            std::mutex mutex;
            bool gpu_ready = false;
            /// live groups BY TOKEN (`allocation + 1`, the shader-facing identity a `descriptor_position`
            /// carries). The token is what a position holds, so a write from a position has to reach the
            /// group without a reverse scan of the 1024-slot table; the generation stored in each group is
            /// what tells a stale position (token reused) from a live one.
            deren::utility::vessel::unordered_map<std::uint32_t, owned_texture_group*> groups_by_token;
            /// the reverse answer, and the reason a caller pointer is never cast before it is known to be
            /// this backend's: the map turns a live `texture_group const*` into its token without the
            /// `static_cast<owned_texture_group>` that a foreign object would make undefined behaviour.
            deren::utility::vessel::unordered_map<deren::promise::rhi::texture_group const*, std::uint32_t> tokens_by_group;
            /// bumped for every group registration; 0 is "no group", so a default position is never valid.
            std::uint32_t next_generation = 0;
            VkDeviceSize reserved_offset = VK_WHOLE_SIZE;
            deren::promise::rhi::unique_object<deren::promise::rhi::buffer> records;
            deren::promise::rhi::unique_object<deren::promise::rhi::image> dummy_image;
            deren::promise::rhi::unique_object<deren::promise::rhi::image_view> dummy_view;
            deren::utility::vessel::array<bool, group_capacity> groups{};
            // Slot 0 is reserved for the dummy descriptor.
            deren::utility::vessel::array<bool, texture_capacity> views{};
            struct sampled_view_lease {
                texture_group_state* state = nullptr;
                std::uint32_t slot = 0;
                deren::promise::rhi::unique_object<deren::promise::rhi::image_view> view;
                ~sampled_view_lease() noexcept;
            };
            deren::utility::vessel::unordered_map<deren::promise::rhi::image const*, std::weak_ptr<sampled_view_lease>> cache;
        };
        struct texture_group_record {
            deren::utility::vessel::array<std::uint32_t, 16> indices{};
            std::uint32_t present_mask = 0;
            deren::utility::vessel::array<std::uint32_t, 3> padding{};
        };
        static_assert(sizeof(texture_group_record) == 80);
        static_assert(offsetof(texture_group_record, present_mask) == 64);
        struct owned_resource_group final : deren::promise::rhi::resource_group {
            owned_resource_group() noexcept
                : deren::promise::rhi::resource_group(concrete::resource_group) {
            }
            core* owner = nullptr;
            std::mutex loading_mutex;
            bool deferred = false;
            deren::promise::rhi::resource_group_ref physical;
            deren::utility::vessel::vector<std::shared_ptr<void>> lazy_references;
            deren::utility::vessel::vector<deren::promise::rhi::resource_group_entry> entries;
            deren::utility::vessel::vector<std::uint32_t> values;
            struct allocation {
                VkDeviceSize offset;
                bool sampler;
            };
            deren::utility::vessel::vector<allocation> allocations;
            void release() noexcept override {
                delete this;
            }
            ~owned_resource_group() noexcept override;
        };
        std::mutex resource_groups_mutex;
        deren::utility::vessel::unordered_set<deren::promise::rhi::resource_group const*> resource_groups;
        deren::utility::vessel::vector<VkDeviceSize> resource_slots;
        deren::utility::vessel::vector<VkDeviceSize> sampler_slots;
        std::uint32_t next_sampler = 0;
        std::mutex group_members_mutex;
        deren::utility::vessel::unordered_set<deren::promise::rhi::object const*> group_members;
        [[nodiscard]] std::expected<deren::promise::rhi::resource_group_ref, deren::promise::rhi::error>
        create_resource_group(deren::promise::rhi::resource_group_desc const&) override;
        [[nodiscard]] deren::promise::rhi::error validate_resource_group_pipeline(deren::promise::rhi::pipeline_desc const&);
        struct owned_texture_group final : deren::promise::rhi::texture_group {
            owned_texture_group() noexcept
                : deren::promise::rhi::texture_group(concrete::texture_group) {
            }
            core* owner = nullptr;
            deren::utility::vessel::array<std::shared_ptr<texture_group_state::sampled_view_lease>, 16> views{};
            detail::lazy_loading<deren::promise::rhi::lazy_state, deren::promise::rhi::error> loading;
            deren::utility::vessel::array<deren::promise::rhi::shared_object<deren::promise::rhi::image>, 16> deferred_images{};
            deren::utility::vessel::array<std::shared_ptr<void>, 16> lazy_references{};
            texture_group_record record{};
            std::uint32_t allocation = 1024;
            /// the position's generation (see `texture_group_state::next_generation`): 0 means this group
            /// was never registered, which is also the empty `descriptor_position`'s generation.
            std::uint32_t generation = 0;
            ~owned_texture_group() noexcept override;
            void release() noexcept override {
                delete this;
            }
        };
        texture_group_state texture_groups;
        [[nodiscard]] deren::promise::rhi::error initialize_texture_groups();
        [[nodiscard]] deren::promise::rhi::error validate_texture_group_pipeline(deren::promise::rhi::pipeline_desc const& desc);
        /// One image is a legal immutable-group source: this backend's, single-layer, colour, sampled.
        /// Requires `contract_images_mutex` held (the caller has already established provenance).
        [[nodiscard]] deren::promise::rhi::error validate_texture_group_image(owned_image const& image) const noexcept;
        /// THE ONE SAMPLED-DESCRIPTOR WRITE PATH: registration (`assign_texture_group`) and
        /// `descriptor_space::write()` both come through here, so "how a group slot is published" exists
        /// once. Requires `texture_groups.mutex` held and `index < texture_group_capacity`.
        [[nodiscard]] deren::promise::rhi::error write_group_texture_locked(owned_texture_group& group, std::uint32_t index,
                                                                            deren::promise::rhi::image& texture) noexcept;
        struct command_submission_view final : deren::promise::rhi::command_submission {
            command_submission_view() noexcept
                : command_submission("deren_command_submission_vulkan") {
            }
            core* owner = nullptr;
            [[nodiscard]] std::expected<deren::promise::rhi::submission_token, deren::promise::rhi::error> submit(deren::promise::rhi::command_buffer&) override;
            [[nodiscard]] std::expected<deren::promise::rhi::submission_token, deren::promise::rhi::error> submit(std::span<deren::promise::rhi::mergeable_command_buffer_ref const>) override;
            [[nodiscard]] deren::promise::rhi::error poll(deren::promise::rhi::submission_token) noexcept override;
            [[nodiscard]] deren::promise::rhi::error wait(deren::promise::rhi::submission_token, std::uint64_t) noexcept override;
        } completion_view;
        struct frame_checkpoint_view final : deren::promise::rhi::frame_checkpoint {
            frame_checkpoint_view() noexcept
                : deren::promise::rhi::frame_checkpoint("deren.vulkan.frame_checkpoint") {
            }
            core* owner = nullptr;
            [[nodiscard]] deren::promise::rhi::error checkpoint_and_wait() override;
        } checkpoint_view;
        bool frame_acquire_consumed = false;
        std::uint64_t const completion_device_id = deren::utility::allocate_identity();
        VkSemaphore completion_timeline = VK_NULL_HANDLE;
        std::uint64_t completion_sequence = 0;
        deren::promise::rhi::error completion_failure = deren::promise::rhi::error::ok;
        [[nodiscard]] deren::promise::rhi::error submit_commands(deren::promise::rhi::command_buffer&, deren::promise::rhi::submission_token*);
        [[nodiscard]] deren::promise::rhi::error submit_batch(std::span<deren::promise::rhi::mergeable_command_buffer_ref const>, deren::promise::rhi::submission_token*);
        [[nodiscard]] deren::promise::rhi::error check_completion(deren::promise::rhi::submission_token, bool wait, std::uint64_t timeout_ns) noexcept;
        struct pending_submission {
            core* owner = nullptr;
            VkFence fence = VK_NULL_HANDLE;
            VkSemaphore timeline = VK_NULL_HANDLE;
            std::uint64_t value = 0;
            std::shared_ptr<owned_command_buffer> primary;
            deren::utility::vessel::vector<deren::promise::rhi::texture_group_ref> groups;
            deren::utility::vessel::vector<deren::promise::rhi::resource_group_ref> resources;
            deren::utility::vessel::vector<std::shared_ptr<void>> lazy_refs;
            deren::utility::vessel::vector<std::shared_ptr<owned_command_buffer>> secondaries;
            bool submitted = false;
            ~pending_submission() noexcept;
        };
        mutable std::mutex submissions_mutex;
        mutable deren::utility::vessel::vector<std::unique_ptr<pending_submission>> pending_submissions;
        void poll_submissions(bool all = false) const noexcept;
        [[nodiscard]] std::shared_ptr<owned_command_buffer> retain_command(owned_command_buffer& command, bool recorded_secondary);
        /// THE COMMAND BUFFERS `create_command_buffer()` HANDED OUT (abi 15), the same registry shape
        /// `contract_images` uses and for the same reason: the batch merge (`execute(span)` / `submit(span)`)
        /// and the one-shot submit must tell "a buffer this backend made" from "some pointer a caller has"
        /// WITHOUT casting the caller's pointer (the cast is only defined once provenance is known). Erased
        /// by `owned_command_buffer::release()`.
        [[nodiscard]] frame_commands* resolve_recorder(deren::promise::rhi::command_recorder* commands, deren::promise::rhi::error& result) noexcept;
        std::mutex contract_command_buffers_mutex;
        deren::utility::vessel::unordered_set<deren::promise::rhi::command_recorder const*> contract_command_buffers;
        /// THE BATCH-ONLY HANDLES `make_mergeable_command_buffer()` HANDED OUT (abi 35): the same provenance
        /// rule as the command-buffer registry above - the batch submission refuses a pointer this backend
        /// did not hand out BEFORE it casts anything. Erased by `owned_mergeable_command_buffer::release()`.
        std::mutex contract_mergeable_buffers_mutex;
        deren::utility::vessel::unordered_set<deren::promise::rhi::command_recorder const*> contract_mergeable_buffers;
        /// the tier-2 address object `query_extension(device_address)` answers with
        struct frame_gpu_sorting final : deren::promise::rhi::gpu_sorting {
            frame_gpu_sorting() noexcept
                : deren::promise::rhi::gpu_sorting(concrete::gpu_sorting) {
            }
            core* owner = nullptr;
            [[nodiscard]] deren::promise::rhi::radix_sorter* create_radix_sorter() override;
        };
        bool gpu_sorting_available = false;
        frame_gpu_sorting sorting_view;
        struct frame_async_presentation final : deren::promise::rhi::async_presentation {
            frame_async_presentation() noexcept
                : deren::promise::rhi::async_presentation(concrete::async_presentation) {
            }
            core* owner = nullptr;
            std::shared_future<deren::promise::rhi::error> begin() override;
        };
        frame_async_presentation async_presentation_view;
        std::unique_ptr<deren::utility::thread_pool> present_worker;
        std::shared_future<deren::promise::rhi::error> present_pending;
        bool async_present_queued = false;
        // Acquired before the worker enters presentation, published only by the next frame open.
        mutable uint32_t prefetched_slot = UINT32_MAX;
        uint32_t prefetched_image = 0;
        struct scoped_sync_view final : deren::promise::rhi::scoped_synchronization {
            scoped_sync_view() noexcept
                : deren::promise::rhi::scoped_synchronization(concrete::scoped_synchronization) {
            }
            core* owner = nullptr;
            [[nodiscard]] deren::promise::rhi::synchronization::stage supported_stages() const noexcept override;
            [[nodiscard]] deren::promise::rhi::error describe(deren::promise::rhi::image const&, deren::promise::rhi::scoped_image_info&) const noexcept override;
            [[nodiscard]] deren::promise::rhi::error describe(deren::promise::rhi::buffer const&, deren::promise::rhi::scoped_buffer_info&) const noexcept override;
            [[nodiscard]] deren::promise::rhi::error validate(std::span<deren::promise::rhi::scoped_image_barrier const>, std::span<deren::promise::rhi::scoped_buffer_barrier const>) const noexcept override;
            [[nodiscard]] deren::promise::rhi::error record(deren::promise::rhi::command_recorder&, std::span<deren::promise::rhi::scoped_image_barrier const>, std::span<deren::promise::rhi::scoped_buffer_barrier const>) const noexcept override;
        };
        scoped_sync_view scoped_sync;
        VkQueueFlags synchronization_queue_flags = 0;
        std::mutex contract_buffers_mutex;
        deren::utility::vessel::unordered_set<deren::promise::rhi::buffer const*> contract_buffers;
        buffer_address_view address_view;
        /// the tier-2 host-copy object `query_extension(host_image_copy)` answers with (③-D/E step 2) -
        /// the first thing that serves that ability, which the contract had announced from the start
        frame_host_copy host_copy_view;
        /// the tier-2 `device_capabilities` object `query_extension(device_capabilities)` answers with, and it
        /// is announced UNCONDITIONALLY because every method of it is answerable once the device exists
        frame_device_capabilities capabilities_view;
        frame_shader_group_access shader_groups_view;
        std::mutex contract_pipelines_mutex;
        deren::utility::vessel::unordered_set<deren::promise::rhi::pipeline const*> contract_pipelines;
        /// the read-back slot's allocation and its cached handle / mapping / capacity: host-visible,
        /// host-coherent and TRANSFER_DST, grown on demand (see frame_readback_buffer())
        vk_buffer readback_slot_buffer;
        VkBuffer readback_slot_handle = VK_NULL_HANDLE;
        void* readback_slot_mapped = nullptr;
        VkDeviceSize readback_slot_size = 0;
        /// true between the ACQUIRE of a frame and the SUBMIT that hands it to the queue: it is the window
        /// `begin_commands()`, `use()` and `copy_image_to_buffer()` answer in (nullptr, or
        /// `error::not_ready`, outside it)
        deren::utility::vessel::array<std::uint64_t, 33> operation_requests{};
        void report_operation_counts() noexcept;
        std::uint64_t deferred_requested = 0;
        std::uint64_t deferred_retained = 0;
        std::uint64_t emitted_setters = 0;
        std::uint64_t emitted_work = 0;
        std::atomic<std::uint64_t> resource_identity{1};
        bool frame_in_flight = false;
        /// whether an acquire has EVER succeeded. `frame_image()` answers nullptr before the first one:
        /// `acquired_image_index` defaults to 0, which is a REAL image (the wrong one), so the index alone
        /// cannot tell "no frame yet" from "frame 0"
        bool frame_acquired = false;

        /// the VkCommandBuffer of the frame slot in flight (VK_NULL_HANDLE when there is none)
        [[nodiscard]] VkCommandBuffer frame_command_buffer() const noexcept;

        VkInstance instance = VK_NULL_HANDLE;
        // called logical_device, not device: the structured binding of that name in core.constructor.cppm
        // would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        VkDevice logical_device = VK_NULL_HANDLE;
        VkPhysicalDevice physical_device = VK_NULL_HANDLE;
        // properties of the picked physical device (VkPhysicalDeviceProperties: limits such as
        // timestampPeriod, bufferImageGranularity, maxPushConstantsSize + the device name).
        // Filled in init_device_and_queue() from the capabilities query it already runs - no
        // second vkGetPhysicalDeviceProperties round trip.
        VkPhysicalDeviceProperties device_properties = {};
        // Ray tracing is optional and comes from the device, not from a build option: when this is
        // false the extensions were not enabled (see device_capabilities) and every ray-traced path
        // skips itself. The properties carry the two limits the AS builder needs - the scratch
        // buffer's required address alignment and the per-level instance/geometry caps - and are a
        // plain data holder like device_properties above (assigned from the query, never passed to
        // Vulkan), so they are zero-initialized rather than carrying a fixed sType.
        bool ray_query_available = false;
        /// @brief whether the RT pipeline (traceRaysEXT + a shader binding table) can be used at all:
        ///        the extension set AND the feature, enabled together at device creation
        bool ray_tracing_pipeline_available = false;
        /// @brief whether VK_EXT_opacity_micromap is enabled: the micromap state that makes an
        ///        alphaMode MASK surface opaque/transparent per microtriangle. Needs the RT pipeline.
        bool opacity_micromap_available = false;
        VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_structure_properties = {};
        /// @brief the micromap subdivision levels the device allows (queried with its feature)
        VkPhysicalDeviceOpacityMicromapPropertiesEXT opacity_micromap_properties = {};
        /// @brief the SBT numbers (handle size, region base alignment, handle alignment, recursion depth)
        ///        a ray-tracing pipeline's shader binding table has to be built against
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR ray_tracing_pipeline_properties = {};
        /**
         * @brief VK_EXT_mesh_shader: whether the device runs a MESH pipeline at all, and the dispatch command
         *        that goes with it (docs/mesh_shaders.md)
         * @note the command is fetched through `vkGetDeviceProcAddr` because `vkCmdDrawMeshTasksEXT` is an
         *       EXTENSION command that the loader's import library does not export - calling it directly is a
         *       link error (`undefined symbol: vkCmdDrawMeshTasksEXT`), which is how this was learned. It is
         *       resolved once here rather than per use, and it is null on a device without the extension.
         * @note this is the FEATURE half only (extension + `meshShader`); whether a pass may use it also depends
         *       on the push budget the stage block needs, and that decision is the RUNTIME's (see
         *       runtime::mesh_shaders), because a pass is not the only thing that could want it.
         */
        bool mesh_shader_available = false;
        bool buffer_int64_atomics_available = false;
        bool task_shader_available = false;
        PFN_vkCmdDrawMeshTasksEXT mesh_dispatch = nullptr;
        /// ... and the INDIRECT form (`vkCmdDrawMeshTasksIndirectEXT`), resolved the same way and null on the same
        /// devices: it reads the three group counts out of a BUFFER instead of taking them as arguments, which is
        /// the seam a COMPUTE culling pass needs - the counts are then decided on the GPU, after culling, rather
        /// than by the host that recorded the draw (see runtime::draw_mesh_tasks_indirect)
        PFN_vkCmdDrawMeshTasksIndirectEXT mesh_dispatch_indirect = nullptr;
        /// `vkCmdTraceRaysKHR`, THE SAME SHAPE AS THE MESH COMMANDS (abi 24): the ray-tracing launch is an
        /// extension entry point the loader's import library does not export, so it is resolved once here and is
        /// null on a device without the ray-tracing pipeline extension. `frame_commands::trace_rays` is the
        /// recording face's verb (a launch is an ordered recording command like `draw`/`dispatch`/
        /// `draw_mesh_tasks`), and it records NOTHING where this is null - a pass that launches rays is only built
        /// when its pipeline could be created, which needs that extension.
        PFN_vkCmdTraceRaysKHR ray_trace_launch = nullptr;
        /// ---- THE ACCELERATION-STRUCTURE ENTRY POINTS (tier-1 since abi 26) ------------------------------
        ///
        /// The same rule as the mesh commands and the launch: the loader's import library does not export them,
        /// so they are resolved once through `vkGetDeviceProcAddr`, and every one is null on a device without
        /// `VK_KHR_acceleration_structure`. `create_acceleration_structure` refuses by name while they are
        /// null, and the two recording verbs answer `unsupported` - a build that cannot be recorded must not
        /// look like one that was.
        PFN_vkCreateAccelerationStructureKHR acceleration_structure_create = nullptr;
        PFN_vkDestroyAccelerationStructureKHR acceleration_structure_destroy = nullptr;
        PFN_vkGetAccelerationStructureBuildSizesKHR acceleration_structure_build_sizes = nullptr;
        PFN_vkGetAccelerationStructureDeviceAddressKHR acceleration_structure_address = nullptr;
        PFN_vkCmdBuildAccelerationStructuresKHR acceleration_structure_build = nullptr;
        /// ---- THE MICROMAP ENTRY POINTS (tier-1 since abi 27, plan S1's P4): the same rule and the same shape
        ///      again, resolved once at startup when the device has VK_EXT_opacity_micromap. All four are null
        ///      otherwise, `create_micromap` refuses by name, and `build_micromap` answers `unsupported`.
        PFN_vkCreateMicromapEXT micromap_create = nullptr;
        PFN_vkDestroyMicromapEXT micromap_destroy = nullptr;
        PFN_vkGetMicromapBuildSizesEXT micromap_build_sizes = nullptr;
        PFN_vkCmdBuildMicromapsEXT micromap_build = nullptr;
        /// VK_EXT_host_image_copy (REQUIRED - see host_image_copy_available below): the copy between an
        /// image and HOST memory that the IMPLEMENTATION performs - no command buffer, no staging buffer,
        /// no submission. Resolved through vkGetDeviceProcAddr like the mesh commands above, because the
        /// loader's import library does not export it; the startup check panics when it cannot be resolved.
        PFN_vkCopyImageToMemoryEXT copy_image_to_memory = nullptr;
        /// the other direction of the same extension: HOST memory -> image. The image upload path uses it
        /// (instead of staging buffer + vkCmdCopyBufferToImage); resolved here and checked at startup for
        /// the same reason as its twin above.
        PFN_vkCopyMemoryToImageEXT copy_memory_to_image = nullptr;
        /// whether the host image copy is CONFIRMED USABLE on this device: the extension, its
        /// hostImageCopy feature, BOTH entry points resolving, and GENERAL present in the device's
        /// copy-source AND copy-destination layout lists - every one of them checked at construction, and
        /// a missing one is a NAMED STARTUP FAILURE (core.constructor.cppm), not a downgrade. In a running
        /// process this member is therefore true: it exists so the paths that use the extension can state
        /// their precondition, not to choose between implementations.
        bool host_image_copy_available = false;
        /// one line for the tier-1 acceleration-structure refusal (see that definition): a caller that reads a
        /// `nullptr` has to be able to tell "not served yet" from "this device cannot"
        bool acceleration_structure_refusal_logged = false;
        /// ... and one for the micromap factory (tier-1 since abi 27), for the same reason
        bool micromap_refusal_logged = false;
        // called graphics_queue_family_index, not graphics_family_index: the structured binding of that name
        // in core.constructor.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        uint32_t graphics_queue_family_index = 0;
        // called present_queue_family_index, not present_family_index: the structured binding of that name
        // in core.constructor.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        uint32_t present_queue_family_index = 0;
        VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;

        /// THE WINDOW THIS CORE OWNS, and ONLY when it created one: with a caller-provided window this
        /// stays null, because since the SHARED flip the caller hands over a NATIVE HANDLE (an HWND) rather
        /// than a `GLFWwindow*` made by another image's GLFW - see init_surface in core.constructor.cppm.
        GLFWwindow* window = nullptr;

        // ---- facade operations (keep raw Vulkan / GLFW calls out of the caller) ----
        void wait_idle() const noexcept; // vkDeviceWaitIdle
        // (no set_window_title: the title is the APPLICATION's - it names the window at creation
        // through create_info.window_title and GLFW keeps it; a window-title facade here only
        // dragged a backend symbol onto the boundary for a call the engine never made)

        // ---- deren.promise.rhi::api_core: THE RHI CONTRACT, IMPLEMENTED IN PLACE -------------------
        //
        // Declared here, defined in source/backends/vulkan/core/core.api_core.cpp. The rule for every one of them is
        // "answer for the device that was really created, never in the abstract": `abilities()` probes
        // the physical device below, the frame calls run the acquire/submit/present machinery on THIS
        // swapchain, and a factory that cannot honour a descriptor answers nullptr instead of a
        // half-built object (the plan's no-throwing-path rule, §4.2).
        //
        // `wait_idle()` below is the non-const OVERLOAD of the const facade call above - the contract's
        // virtual is not const, and both spellings answer `vkDeviceWaitIdle`.
        //
        // THE CONTRACT'S THIRD HANDSHAKE (abi 19): this object's own `api_version()`, answered with the
        // number THIS image compiled. The engine checks it before it uses the object - the consumption side
        // of the same fact the entry's first argument guards on the creation side - and it is a contract
        // virtual, so it emits no symbol of its own and the boundary's measured count stays at zero.
        [[nodiscard]] std::uint32_t api_version() const noexcept override;
        [[nodiscard]] deren::promise::rhi::texture_group_ref assign_texture_group(
            deren::promise::rhi::texture_group_info const& info, deren::promise::rhi::error* result = nullptr) override;
        [[nodiscard]] deren::promise::rhi::ability_bits abilities() const noexcept override;
        [[nodiscard]] deren::promise::rhi::extension* query_extension(deren::promise::rhi::extension_kind kind) noexcept override;

        [[nodiscard]] std::expected<deren::promise::rhi::unique_object<deren::promise::rhi::lazy_buffer>, deren::promise::rhi::error> create_lazy_buffer(deren::promise::rhi::lazy_buffer_desc const&) override;
        [[nodiscard]] std::expected<deren::promise::rhi::unique_object<deren::promise::rhi::lazy_image>, deren::promise::rhi::error> create_lazy_image(deren::promise::rhi::lazy_image_desc const&) override;
        [[nodiscard]] deren::promise::rhi::resource_memory_statistics memory_statistics() const noexcept override;
        [[nodiscard]] deren::promise::rhi::error materialize(owned_buffer&) noexcept;
        [[nodiscard]] deren::promise::rhi::error materialize(owned_image&) noexcept;
        [[nodiscard]] deren::promise::rhi::error materialize(owned_image_view&) noexcept;
        [[nodiscard]] deren::promise::rhi::error materialize(owned_resource_group&) noexcept;
        [[nodiscard]] deren::promise::rhi::error materialize(owned_texture_group&) noexcept;
        [[nodiscard]] std::shared_ptr<void> depend_on(owned_buffer const&);
        [[nodiscard]] std::shared_ptr<void> depend_on(owned_image const&);
        [[nodiscard]] VkFormat resolve_format(deren::promise::rhi::image_format) const noexcept;
        [[nodiscard]] std::uint64_t internal_address(owned_buffer const&, std::uint64_t offset = 0) const noexcept;
        [[nodiscard]] deren::promise::rhi::unique_object<deren::promise::rhi::buffer> create_buffer(deren::promise::rhi::buffer_desc const& desc) override;
        /// THE BACKEND'S OWN SPELLING of `create_buffer`: the factories hand CALLERS an unique_object, but the
        /// backend's storage fields keep the raw handle and release it in their own teardown (it is the owner
        /// either way). Not part of the override surface.
        [[nodiscard]] deren::promise::rhi::buffer* create_buffer_raw(deren::promise::rhi::buffer_desc const& desc);
        [[nodiscard]] deren::promise::rhi::unique_object<deren::promise::rhi::image> create_image(deren::promise::rhi::image_desc const& desc) override;
        /// TIER-1 SINCE ABI 26 AND REFUSED BY NAME BY THIS BACKEND UNTIL PLAN S1's P1b: the shape exists (see
        /// the definition's note for what landing it takes), so a caller gets a logged `nullptr` - the same
        /// named refusal for an unsupported resource.
        [[nodiscard]] deren::promise::rhi::unique_object<deren::promise::rhi::acceleration_structure> create_acceleration_structure(deren::promise::rhi::acceleration_structure_desc const& desc) override;
        /// TIER-1 SINCE ABI 27 (plan S1's P4), refused by name on a device without VK_EXT_opacity_micromap
        [[nodiscard]] deren::promise::rhi::unique_object<deren::promise::rhi::micromap> create_micromap(deren::promise::rhi::micromap_desc const& desc) override;
        [[nodiscard]] deren::promise::rhi::unique_object<deren::promise::rhi::sampler> create_sampler(deren::promise::rhi::sampler_desc const& desc) override;
        [[nodiscard]] deren::promise::rhi::unique_object<deren::promise::rhi::shader> create_shader(deren::promise::rhi::shader_desc const& desc) override;
        [[nodiscard]] deren::promise::rhi::unique_object<deren::promise::rhi::pipeline> create_pipeline(deren::promise::rhi::pipeline_desc const& desc) override;
        /**
         * THE COMPUTE SPELLING OF `create_pipeline` (abi 21), split out because it shares NOTHING with the
         * graphics path but the descriptor: one stage from `compute_code`, a NULL layout with the heap flag,
         * and `owned_pipeline::bind_point = COMPUTE` - the bind point `bind_pipeline` reads, which the graphics
         * path never has to set. See the definition for why the branch is not inline there.
         */
        [[nodiscard]] deren::promise::rhi::pipeline* create_compute_pipeline(deren::promise::rhi::pipeline_desc const& desc, char const* what);
        /**
         * THE RAY-TRACING SPELLING OF `create_pipeline` (abi 21): several named entry points, one module each,
         * a group table whose indices are positions in the stage list, and the entry point resolved through
         * `vkGetDeviceProcAddr` (the loader exports no extension command). See the definition for the
         * `bind_point` it sets - the ray-tracing one, which is what makes `bind_pipeline` work for it.
         */
        [[nodiscard]] deren::promise::rhi::pipeline* create_ray_tracing_pipeline(deren::promise::rhi::pipeline_desc const& desc, char const* what);
        [[nodiscard]] deren::promise::rhi::command_buffer* begin_commands() override;
        // ---- S2 batch 2: the recording surface's frame-domain views --------------------------------
        // `begin_commands()` now hands out a REAL list (the frame's primary command buffer, which this
        // class owns - see command_buffers below), so the tier-2 verbs that take a `command_buffer&`
        // (push_constants / draw_mesh_tasks / build_acceleration_structure) become reachable, and the
        // read-back's copy can be recorded into the frame it belongs to.
        [[nodiscard]] deren::promise::rhi::image* frame_image() noexcept override;
        [[nodiscard]] deren::promise::rhi::buffer* frame_readback_buffer() noexcept override;
        [[nodiscard]] deren::promise::rhi::submit_info frame_begin() override;
        [[nodiscard]] deren::promise::rhi::error present() override;
        void wait_idle() override;
        // ---- the frame face (abi 13, grown in abi 14): appended, after every virtual the older
        // engines dispatch ----
        [[nodiscard]] deren::promise::rhi::frame_walker* walk_frames() noexcept override;
        [[nodiscard]] deren::promise::rhi::gpu_profiler* profiler() noexcept override;
        [[nodiscard]] deren::promise::rhi::swapchain* frame_swapchain() noexcept override;
        [[nodiscard]] deren::promise::rhi::error submit() override;
        [[nodiscard]] deren::promise::rhi::error submit(deren::promise::rhi::command_buffer& commands) override;
        /// abi 39: the OPAQUE main-thread context the GUI plugin casts to its Vulkan layout
        /// (source/backends/vulkan/main_context/main_context.cppm). Borrowed; nullptr only if this core has
        /// nothing to hand out (this backend always does).
        [[nodiscard]] deren::promise::rhi::main_context* main_context() noexcept override;
        /// abi 35: the batch submission (see `mergeable_command_buffer` and `core::submit(span)`).
        [[nodiscard]] deren::promise::rhi::error submit(std::span<deren::promise::rhi::mergeable_command_buffer_ref const> batch) override;
        /// abi 35: the batch-only recording handle. The returned handle carries one reference, dropped by
        /// `release()`; the batch submission shares the underlying secondary through the owned registry.
        [[nodiscard]] deren::promise::rhi::mergeable_command_buffer_ref make_mergeable_command_buffer(deren::promise::rhi::command_buffer_desc const& desc) override;
        /// abi 15: the owned recording handle. The result is heap-allocated here and destroyed by the
        /// contract's `release()` (which is also what removes it from the registry below).
        [[nodiscard]] deren::promise::rhi::unique_object<deren::promise::rhi::command_buffer> create_command_buffer(deren::promise::rhi::command_buffer_desc const& desc) override;
        /// THE OWNED-BUFFER ALLOCATION, without the contract kind: the contract factory only makes a
        /// PRIMARY now (abi 37 retired the secondary kind), and `make_mergeable_command_buffer` calls this
        /// directly to allocate the secondary it executes in its batches. `level` is the API level, not a
        /// contract vocabulary: no caller can name it.
        [[nodiscard]] owned_command_buffer* allocate_owned_command_buffer(VkCommandBufferLevel level);
        /// abi 21/44: THE SAME OWNED HANDLE, handed over as the contract's `shared_object` - its control
        /// block's last copy calls `release()`, so no call site spells it. Empty = the same refusal
        /// `create_command_buffer()` reports as `nullptr` (logged there, once per call site).
        [[nodiscard]] deren::promise::rhi::shared_object<deren::promise::rhi::command_buffer> make_command_buffer(deren::promise::rhi::command_buffer_desc const& desc) override;

        VkSurfaceKHR surface = VK_NULL_HANDLE;

        // called graphics_queue_handle, not graphics_queue: the structured binding of that name in
        // core.constructor.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        VkQueue graphics_queue_handle = VK_NULL_HANDLE;
        // called present_queue_handle, not present_queue: the structured binding of that name in
        // core.constructor.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        VkQueue present_queue_handle = VK_NULL_HANDLE;
        uint32_t graphics_queue_family = VK_QUEUE_FAMILY_IGNORED;

        VkSwapchainKHR swap_chain = {};
        deren::utility::vessel::vector<VkImage> swap_chain_images = {};
        VkFormat swap_chain_image_format = {};
        ::deren::promise::rhi::image_extent swap_chain_extent = {};
        /// the swapchain image the contract's frame_begin() acquired (vkAcquireNextImageKHR), i.e. what
        /// the contract's present() hands to the presentation engine. The runtime's own pacing path
        /// keeps its own `current_image_index` until it migrates onto the contract (S3).
        uint32_t acquired_image_index = 0;
        /**
         * @brief the extent the RENDER chain runs at: `swap_chain_extent` scaled by `render_scale`
         *
         * This is the ONE definition of "the frame's resolution": `runtime::pass_frame` hands it to every
         * pass (so a `full` or `half` `extent_rule` is relative to it), and every render target this core
         * creates is created with it. At `render_scale == 1.0` it returns `swap_chain_extent` exactly, so a
         * frame at the default scale is the frame this renderer always produced - which is what makes the
         * decoupling verifiable rather than merely plausible.
         *
         * ROUNDED to nearest rather than truncated: at half scale the floor would take a pixel off an odd
         * output width ON TOP of the halving, and both the images and the passes have to agree on the
         * number, so there is one formula and it is this one.
         */
        [[nodiscard]] ::deren::promise::rhi::image_extent render_extent() const noexcept {
            auto const scaled = [this](uint32_t const axis) {
                uint32_t const value = static_cast<uint32_t>(static_cast<float>(axis) * this->render_scale + 0.5f);
                return value == 0u ? 1u : value; // a zero extent is not a small frame, it is an invalid one
            };
            return ::deren::promise::rhi::image_extent{scaled(this->swap_chain_extent.width), scaled(this->swap_chain_extent.height)};
        }
        /// THE SAME SIZE, IN THE API'S OWN STRUCT (plan X5 B3.2): `VkRect2D`'s extent and the swapchain create
        /// info still take a `VkExtent2D`, so this is the ONE place that conversion lives. The engine's own
        /// vocabulary for a size is `rhi::image_extent` now; a Vulkan struct is only built where Vulkan is.
        [[nodiscard]] VkExtent2D render_extent_2d() const noexcept {
            ::deren::promise::rhi::image_extent const extent = this->render_extent();
            return VkExtent2D{.width = extent.width, .height = extent.height};
        }
        /// @brief the render scale `render_extent()` applies; clamped to (0, 1] at construction
        float render_scale = 1.0f;
        // Whether the swapchain images were created with VK_IMAGE_USAGE_TRANSFER_SRC_BIT (i.e. the
        // surface supports it). The screenshot read-back copies from a swapchain image and is only
        // legal when this is true - see init_swap_chain().
        bool swapchain_transfer_src_supported = false;
        // NOTE: there is deliberately NO `swapchain_host_transfer_supported` beside it. A swapchain
        // image's usage has to be a subset of the surface's supportedUsageFlags
        // (VUID-VkSwapchainCreateInfoKHR-imageUsage-01276), and the surfaces this renderer runs on do not
        // list VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT - so `vkCopyImageToMemoryEXT` can never read a
        // swapchain image here, and the read-back has exactly one mechanism (the copy command). A member
        // whose only purpose was to choose between two mechanisms is a liability, not a capability.

        // MSAA used to live here. It is gone with the forward path that was its only consumer: a
        // G-buffer cannot be multisampled without per-sample shading, so the scene has always
        // rendered at 1x, and with no second path there is nothing left for the setting to select.
        // The anti-aliasing story is TAA (and FXAA) on the shaded image instead.
        deren::utility::vessel::vector<VkImageView> swap_chain_image_views = {};

        VkFormat color_format = VK_FORMAT_UNDEFINED;
        // ---- THE RENDER CHAIN'S TARGETS ARE THE ENGINE'S (③-D/E A1.1-A1.7) ---------------------------
        // Every per-image and per-frame-slot target this backend used to create, own and destroy stood
        // here: the HDR target and the display-referred (LDR) target, the G-buffer cluster (the three
        // stored surface targets, the pass's own depth image, the motion-vector target and the scene-colour
        // TAA working image), the TAA history pair, the stochastic punctual lighting trio, the four bloom
        // levels, the ray-traced visibility pair and the furnace environment cube. The ENGINE creates and
        // holds all of them now - `runtime::create_render_chain_targets()`, through the contract, releasing
        // the old set before it creates the new one - which is why the members, the cleanup registration
        // that destroyed them and the raw `create_target_image*` helpers beside it are gone.
        // WHAT STAYS IN THIS CLASS IS WHAT THE BACKEND ITSELF OWNS: the swapchain images and their views
        // (above), the forward path's depth image (below), the samplers, the command buffers, the
        // allocator and the device.

        // called depth_attachment_format, not depth_format: make_depth_pipeline keeps a parameter named
        // depth_format, which would hide the member of that name and MSVC /W4 reports C4458 (an error under /WX).
        VkFormat depth_attachment_format = {};
        deren::utility::vessel::vector<VkImage> depth_images = {};
        deren::utility::vessel::vector<VkDeviceMemory> depth_image_memories = {};
        deren::utility::vessel::vector<VkImageView> depth_image_views = {};

        // NOTE: there is deliberately no `command_pool` member beside the frame command buffers below.
        // Each `vk_command_buffer` OWNS ITS OWN pool (handles/handles.cppm): the shared pool was the
        // last reason a caller had to keep a second object alive next to a command buffer, and it was
        // also the one pool every recording thread shared - which a VkCommandPool does not permit.

        /** @brief create the shared samplers above: device-level, reference-counted by nobody, destroyed with core */

        // ---- the SHARED samplers, created once with the device (see create_samplers) ----
        //
        // THEY LIVE HERE because a sampler is a device-level object with no per-frame state and no owner among the
        // passes: a pass DECLARES one by hint (see render_resource::shared::sampler_set) and the renderer hands over
        // the handles, so the object's owner has to be the device root - the same argument every image in this class
        // answers. Before this they were scattered across the runtime's scene setup, a pipeline builder and two
        // ensure_* functions - the "naming accident" this comment records.
        //
        // `env_sampler` is deliberately NOT here: its max_lod is the app's environment mip count, not a device fact.
        vk_sampler texture_sampler = {};      // the bindless texture array: REPEAT, and all its mip levels
        vk_sampler gbuffer_sampler = {};      // the G-buffer's stored surface: NEAREST, clamp (exact texel centres)
        vk_sampler taa_sampler = {};          // the TAA resolve: LINEAR magnification, NEAREST minification
        vk_sampler post_sampler = {};         // the post chain and the FXAA filter: LINEAR, clamp
        vk_sampler post_nearest_sampler = {}; // the composite's GI upsample: NEAREST, clamp (depths are not colours)
        vk_sampler shadow_sampler = {};       // the cascaded map: depth-compare + LINEAR (hardware PCF), clamp
        /**
         * @brief the CREATE INFO of @ref texture_sampler, kept because the descriptor heap needs it as an
         *        EMBEDDED SAMPLER: a heap binding for a combined image sampler takes its sampler from a
         *        VkSamplerCreateInfo (the driver creates one), not from a VkSampler, and it has to be the SAME
         *        sampler the descriptor-set path uses - a different max_lod alone changes which mip is read.
         * @note filled where the sampler is created (create_samplers), so the two cannot drift.
         */
        VkSamplerCreateInfo texture_sampler_info = {};
        /**
         * @brief the CREATE INFO of every shared sampler, in the order source/shaders/heap_slots.glsl names them, because
         *        a heap descriptor for a sampler IS a create info - and the heap is created LATER in the
         *        constructor than these samplers are (create_samplers runs first), so the infos have to be kept
         *        here to be written onto the sampler grid once it exists.
         * @note index 0 texture (linear, repeat mips), 1 post (linear, clamp, one mip), 2 gbuffer (nearest, clamp),
         *       3 post-nearest (nearest, clamp), 4 taa (linear mag / nearest min, clamp), 5 shadow (depth compare).
         */
        deren::utility::vessel::array<VkSamplerCreateInfo, 6> shared_sampler_infos = {};

        vma_allocator vma = {};
        /// the device-wide descriptor heap (see source/backends/vulkan/core/descriptor_heap): one resource heap and one
        /// sampler heap, created right after the device so every pass can be written against it
        descriptor_heap descriptor_heaps = {};
        /// the heap's limits, copied out of the capability query (see core::init_device_and_queue) because the
        /// heap itself is created in the constructor, after vma.init() - the capabilities are not in scope there
        heap_limits descriptor_heap_limits = {};
        /**
         * @brief the reserved heap blocks, in bytes, or VK_WHOLE_SIZE when the heap is not in use
         *
         * @note THE LAYOUT IS OWNED HERE, and that is not tidiness: the heap's contents are written by the RUNTIME
         *       (it is what knows the textures and the material table) while the SLOT NUMBERS that point the
         *       shaders at them are named in `source/shaders/heap_slots.glsl`. Both sides have to use the same number, and
         *       a mismatch - a write at one offset, a shader reading another - is invisible to validation and shows
         *       up only as a wrong picture. So the blocks are reserved once, here, and published; the startup log
         *       prints the whole table, which is how a drift between the two sides is seen.
         */
        VkDeviceSize heap_texture_array_offset = VK_WHOLE_SIZE;
        VkDeviceSize heap_material_table_offset = VK_WHOLE_SIZE;

        // ==========================================================================================
        // THE SLOT GRID LIVES IN ITS OWN MODULE (③-D/E batch, ruling D): `deren.engine.render_layout`
        // is compiled by BOTH halves, because the grid is the agreement between the host's writes and
        // the shader's baked `array[index]` indices - and the ENGINE names it in ~95 places, which it
        // could not do through a backend class member without importing the backend.
        //
        // WHAT STAYS HERE: the backend's own spellings, as ALIASES, so the ~20 backend call sites are
        // untouched while the engine's call sites migrate to the module. The frozen numbers below are
        // the guard the ruling asked for: the module is the single source of truth, and if its grid ever
        // moves, the static_asserts fire and the backend stops compiling - a layout drift is then a
        // COMPILE ERROR instead of a silently wrong picture (validation cannot see it, and the captures
        // would only show it as a different image).
        // ==========================================================================================
        using heap_slots = deren::engine::render_layout::heap_slots;
        static constexpr VkDeviceSize heap_slot_stride = deren::engine::render_layout::heap_slot_stride;
        static constexpr uint32_t heap_slot_base = deren::engine::render_layout::heap_slot_base;
        static constexpr uint32_t heap_slot_count = deren::engine::render_layout::heap_slot_count;
        static constexpr uint32_t heap_image_capacity = deren::engine::render_layout::heap_image_capacity;
        static constexpr uint32_t heap_sampler_base = deren::engine::render_layout::heap_sampler_base;
        static constexpr VkDeviceSize heap_sampler_stride = deren::engine::render_layout::heap_sampler_stride;
        [[nodiscard]] static constexpr VkDeviceSize heap_slot_offset(uint32_t const slot) noexcept {
            return deren::engine::render_layout::heap_slot_offset(slot);
        }
        // THE GUARD: the numbers this backend was built with, written out. They are what the shader's
        // baked indices and the captures were made against, so a change on either side must be a
        // deliberate edit HERE as well.
        static_assert(deren::engine::render_layout::heap_slot_base == 16384u, "the slot grid's base moved");
        static_assert(deren::engine::render_layout::heap_slot_count == 3072u, "the slot grid's size moved");
        static_assert(deren::engine::render_layout::heap_slot_stride == 64u, "the slot grid's stride moved");
        static_assert(deren::engine::render_layout::heap_image_capacity == 8u, "the per-image array capacity moved");
        static_assert(deren::engine::render_layout::heap_sampler_base == 2048u, "the sampler grid's base moved");
        // THE SOFTWARE RASTER'S SURFACE (M4 Task 2) is part of the same grid, and these are this backend's half of the
        // mirror test_render_resources.cpp runs between the host grid and the shaders' baked constants. They are
        // asserted by RELATION rather than by repeating the numbers - the numbers live in deren.engine.render_layout
        // only. Sampled and storage views have distinct per-image runs, with the tile-list frame lanes between them.
        static_assert(deren::engine::render_layout::heap_slots::software_depth + deren::engine::render_layout::heap_image_capacity <=
                          deren::engine::render_layout::heap_slots::software_id,
                      "the software depth family must end before the software id family");
        static_assert(deren::engine::render_layout::heap_slots::software_tiles ==
                          deren::engine::render_layout::heap_slots::software_id + deren::engine::render_layout::heap_image_capacity,
                      "the software tile-list lane must follow the software id family");
        static_assert(deren::engine::render_layout::heap_slots::software_depth_storage == deren::engine::render_layout::heap_slots::software_tiles + 2u,
                      "software storage descriptors must follow both tile-list frame lanes");
        static_assert(deren::engine::render_layout::heap_slots::software_id_storage ==
                          deren::engine::render_layout::heap_slots::software_depth_storage + deren::engine::render_layout::heap_image_capacity,
                      "the software id storage run must follow the software depth storage run");
        static_assert(deren::engine::render_layout::heap_slots::software_route_arguments + deren::engine::render_layout::heap_image_capacity <=
                          deren::engine::render_layout::heap_slot_base + deren::engine::render_layout::heap_slot_count,
                      "the software storage image runs must stay inside the grid");
        static_assert(deren::engine::render_layout::heap_sampler_stride == 32u, "the sampler grid's stride moved");
        /// @brief the byte offset of slot 0 (see @ref heap_slots), or VK_WHOLE_SIZE when the heap is not in use
        VkDeviceSize heap_grid_offset = VK_WHOLE_SIZE;

        // ---- frame synchronization (timeline semaphores; see create_sync_objects) ----
        // vkAcquireNextImageKHR and vkQueuePresentKHR both require BINARY semaphores:
        //   - image_available_semaphores: binary, per frame slot (acquire signal)
        //   - present_ready_semaphores: binary, ONE PER SWAPCHAIN IMAGE — present may run on a
        //     separate queue, so a per-slot binary could be re-signaled before the previous
        //     present consumed it; a swapchain image is only re-acquired after its present
        //     finished, which keeps this per-image gate safe across queues
        //   - frame_done_semaphores / frame_done_values: TIMELINE, per frame slot, counting
        //     submissions — submit() signals it (GPU completion), wait_frame_slot() is the host
        //     pacing wait that used to be vkWaitForFences
        deren::utility::vessel::vector<VkSemaphore> image_available_semaphores = {}; // binary, per frame slot (acquire)
        deren::utility::vessel::vector<VkSemaphore> present_ready_semaphores = {};   // binary, per swapchain image (present wait)
        deren::utility::vessel::vector<VkSemaphore> frame_done_semaphores = {};      // timeline, per frame slot
        deren::utility::vessel::vector<uint64_t> frame_done_values = {};             // last signaled value per slot (host bookkeeping)

        size_t current_frame = 0;

        void to_next_frame() noexcept;
        void wait_frame_slot(uint32_t slot) const; // host wait until this slot's last submission completed
        /// the RESULT-BEARING primitive the wait above is spelled from: `wait_frame_slot` drops the
        /// VkResult (its one remaining caller is the transitional engine path), while
        /// `frame_walker_view::wait_and_acquire()` reports it - the `vkWaitSemaphores` failure that
        /// used to vanish arrives in `frame_open_info::result`.
        [[nodiscard]] VkResult wait_frame_slot_result(uint32_t slot) const noexcept;

        static constexpr int32_t MAX_FRAMES_IN_FLIGHT = 2;
        /// THE RING DEPTH IS PINNED TO THE CONTRACT (③-D/E batch): the engine sizes per-slot C++ arrays
        /// with `rhi::max_frames_in_flight`, so a backend ring of a different depth would overrun them.
        /// The static_assert makes "the two drifted" a COMPILE ERROR instead of a silent overrun, and
        /// the engine repeats the check at startup against the runtime truth
        /// (`frame_walker::slot_count()`) because a backend built from a different revision would have
        /// compiled against its own copy of the contract.
        static_assert(MAX_FRAMES_IN_FLIGHT == static_cast<int32_t>(deren::promise::rhi::max_frames_in_flight),
                      "the backend's frame ring must be the depth rhi::max_frames_in_flight promises");
        /// THE PER-IMAGE ARRAY BOUND, PINNED FROM THE BACKEND'S SIDE (③-D/E item A1): this backend requests
        /// at most 4 swapchain images (3-4 in practice), and `rhi::max_swapchain_images` is the number the
        /// ENGINE sizes its per-image arrays by. `init_swap_chain()`'s refusal is the runtime half of the same
        /// promise; this is the compile-time half - a constant that grew past what this backend accounts for
        /// stops the build instead of quietly oversizing every caller's arrays.
        static constexpr uint32_t max_swapchain_images_requested = 4;
        static_assert(deren::promise::rhi::max_swapchain_images <= max_swapchain_images_requested,
                      "rhi::max_swapchain_images must not promise more images than this backend accounts for");

        // ---- GPU pass timing (VK_QUERY_TYPE_TIMESTAMP) ----
        // A timestamp pool with one contiguous range of gpu_timing_mark_capacity queries per frame
        // slot: the frame records vkCmdResetQueryPool + one vkCmdWriteTimestamp per pass boundary
        // and the reader converts consecutive marks into milliseconds after the slot's submission
        // completed (see mark_gpu_timing / read_gpu_timings). Timestamps need no feature bit, but a
        // queue family that cannot write them reports timestampValidBits == 0, and the tick length
        // comes from the device limits - either missing means gpu_timing_supported stays false and
        // every timing call is a no-op, so callers do not have to check the device themselves.
        VkQueryPool timestamp_query_pool = VK_NULL_HANDLE;
        float timestamp_period_ns = 0.0f;  // ns per tick (VkPhysicalDeviceLimits::timestampPeriod)
        uint32_t timestamp_valid_bits = 0; // graphics family counter width (0 = cannot timestamp)
        bool gpu_timing_supported = false;
        // marks the CURRENT recording of each slot has written (reset by begin_gpu_timing). Also
        // read back as "how many queries to fetch" for the submission that just completed, because
        // a slot is only read after it was paced and before it is recorded again.
        deren::utility::vessel::array<uint32_t, MAX_FRAMES_IN_FLIGHT> gpu_timing_marks = {};
        // the static names the engine's marks carry (the frame face's name channel, abi 13): mark i
        // of slot s stores the caller's text here, and the profiler's latched snapshot reports it
        // verbatim. THE TEXT MUST OUTLIVE THE FRAME - a literal (the same rule as `window_title`);
        // the backend only stores the view, never the characters.
        deren::utility::vessel::array<deren::utility::vessel::array<std::string_view, gpu_timing_mark_capacity>, MAX_FRAMES_IN_FLIGHT> gpu_timing_names = {};
        // frame_done value each slot's timings were last read for: a slot is read at most once per
        // submission, so a frame that hits an early return cannot fetch the same results twice
        deren::utility::vessel::array<uint64_t, MAX_FRAMES_IN_FLIGHT> gpu_timing_read_value = {};

        /**
         * @ingroup vulkan_core
         * @brief open this frame's timing range: reset the slot's queries and forget the previous
         *        frame's marks
         * @param command_buffer the frame's command buffer (the reset is recorded on the GPU
         *        timeline, which keeps it off the host/GPU race the pool would otherwise have)
         * @param slot the frame slot being recorded
         * @note call once per frame, before any mark and outside a dynamic rendering instance;
         *       a no-op when the device cannot timestamp
         */
        void begin_gpu_timing(VkCommandBuffer command_buffer, uint32_t slot) noexcept;

        /**
         * @ingroup vulkan_core
         * @brief write one timing mark into this frame's range
         * @param command_buffer the frame's command buffer
         * @param slot the frame slot being recorded
         * @param stage pipeline stage the mark resolves at: callers use TOP_OF_PIPE for the first
         *        mark of the frame and BOTTOM_OF_PIPE for every pass boundary, so mark i + 1 minus
         *        mark i is exactly how long pass i took
         * @param stage_name the mark's semantic name, AS STATIC TEXT (a literal): stored as a view
         *        and reported verbatim by the contract's `gpu_profiler` face - the same rule as
         *        `window_title`, and the reason no stage-id enum exists in the contract (the engine's
         *        stage vocabulary stays the engine's)
         * @note a no-op when the device cannot timestamp or the frame already wrote
         *       gpu_timing_mark_capacity marks
         */
        void mark_gpu_timing(VkCommandBuffer command_buffer, uint32_t slot, VkPipelineStageFlagBits stage,
                             std::string_view stage_name = {}) noexcept;

        /**
         * @ingroup vulkan_core
         * @brief convert a completed submission's marks into milliseconds
         * @param slot the frame slot to read (its last submission must have completed - pace the
         *        slot first, see wait_frame_slot)
         * @return the durations between consecutive marks, or a zero mark_count when timings are
         *         unavailable, the slot never submitted, or this submission was already read
         * @note never waits on the GPU and never blocks: vkGetQueryPoolResults is called without
         *       VK_QUERY_RESULT_WAIT_BIT, and an unavailable result reports "no measurement"
         *       instead of stalling
         */
        gpu_timing_result read_gpu_timings(uint32_t slot);

        /** @brief whether this device can measure GPU pass timings (see begin_gpu_timing) */
        [[nodiscard]] bool gpu_timing_available() const noexcept {
            return this->gpu_timing_supported;
        }

        core();
        /**
         * @ingroup vulkan_core
         * @brief THE construction: the contract's creation structure, taken directly
         * @param options the creation descriptor, `deren::promise::rhi::create_info`
         *        (source/promise/rhi/rhi.core_desc.cppm): title, size, render scale, vsync, validation layers,
         *        window visibility, and the optional native_window the CALLER owns
         *
         * THE CONTRACT'S STRUCTURE IS THE ONLY ONE. `vulkan::core_create_info` and the
         * `to_backend_create_info()` translation that stood between the two spellings are gone: the
         * initialisation run below reads the boundary's own fields, so there is one structure to add
         * a field to instead of two plus a mapping to keep in step. The one thing a translation still
         * does is the ABI GUARD: `options.struct_size` is compared once and a field whose whole extent
         * is not inside the bytes the caller declares keeps this build's default (see
         * core.constructor.cppm's `sanitize_create_info`), per the append-only rule in
         * rhi.core_desc.cppm.
         *
         * `options.native_window` non-null means BIND THE CALLER'S WINDOW: no window is created, none
         * is destroyed and none is shown or hidden - the caller keeps it alive for as long as this
         * core lives (the size/title/visibility fields are ignored in that mode). The caller also owns
         * GLFW's lifetime in that mode: it initialised GLFW before this core was constructed and it
         * terminates it after this core and its surface are gone.
         *
         * `options.window_title` is BORROWED until this call returns (GLFW copies it into the window);
         * the other fields are copied into `create_options` and read for the core's whole life, which
         * is why nothing here stores the title pointer.
         */
        explicit core(deren::promise::rhi::create_info const& options);
        ~core();

        /// @brief allocate a PRIMARY command buffer THAT OWNS ITS OWN COMMAND POOL
        /// @note one pool per buffer is what makes the wrapper self-contained (see
        ///       deren::vulkan::vk_command_buffer): no shared pool exists any more, which is also what
        ///       a recording thread needs - a VkCommandPool is not thread safe.
        vk_command_buffer make_command_buffer() const;
        /** @brief allocate a SECONDARY command buffer that owns its own command pool
         *         (recorded inside a dynamic rendering instance, executed there via vkCmdExecuteCommands) */
        vk_command_buffer make_secondary_command_buffer() const;

        std::optional<vk_shader_module> make_shader_module(std::span<uint8_t> shader) const noexcept;

        /**
         * @ingroup vulkan_core
         * @brief create an image view covering the whole image (all mip levels and layers)
         * @param image the image to view
         * @param format the view format
         * @param type view type (VK_IMAGE_VIEW_TYPE_2D / VK_IMAGE_VIEW_TYPE_CUBE ...)
         * @return raii vk_image_view owning the created view
         */
        vk_image_view make_image_view(VkImage image, VkFormat format, VkImageViewType type) const;

        /**
         * @ingroup vulkan_core
         * @brief create a 2D depth image view (DEPTH aspect) over the whole image
         * @param image the image to view
         * @param format the view format (a depth format)
         * @return raii vk_image_view owning the created view
         * @note the regular make_image_view uses the COLOR aspect; depth images (e.g. the shadow
         *       map) need the DEPTH aspect to be sampled as depth
         */
        vk_image_view make_depth_image_view(VkImage image, VkFormat format) const;

        /**
         * @ingroup vulkan_core
         * @brief create a 2D ARRAY depth image view covering every layer (DEPTH aspect)
         * @param image the layered depth image
         * @param format the view format (a depth format)
         * @return raii vk_image_view owning the created view
         * @note the cascaded shadow map is ONE layered image sampled as an array: a fragment shader
         *       picks its cascade per pixel, and dynamic indexing of a sampler array would require
         *       dynamically uniform indices, while a texture-array layer is just a coordinate
         */
        vk_image_view make_depth_array_view(VkImage image, VkFormat format) const;

        /**
         * @ingroup vulkan_core
         * @brief create a 2D depth image view of ONE layer (DEPTH aspect), for rendering into it
         * @param image the layered depth image
         * @param format the view format (a depth format)
         * @param layer the array layer to view
         * @return raii vk_image_view owning the created view
         */
        vk_image_view make_depth_layer_view(VkImage image, VkFormat format, uint32_t layer) const;

        /**
         * @ingroup vulkan_core
         * @brief create a linear/min-linear sampler with the given wrap mode
         * @param address_mode wrap mode applied to all three axes
         * @param max_lod maximum mip level the sampler may access
         * @return raii vk_sampler owning the created sampler
         */
        vk_sampler make_sampler(VkSamplerAddressMode address_mode, float max_lod) const;

        /**
         * @ingroup vulkan_core
         * @brief create the shadow map sampling sampler (NEAREST + clamp-to-edge)
         * @return raii vk_sampler owning the created sampler
         * @note pbr.frag does manual percentage-closer filtering: it fetches the stored depth
         *       with this NEAREST sampler at a few neighbor texels and averages the comparisons,
         *       so no depth-comparison/linear-filter format feature is required
         */
        vk_sampler make_shadow_sampler() const;

        /**
         * @ingroup vulkan_core
         * @brief create a depth-only graphics pipeline (no color attachment, single sample),
         *        used by the shadow pass to render depth into the shadow map
         * @param vertex_shader_code raw SPIR-V binary of the vertex shader
         * @param fragment_shader_code raw SPIR-V binary of the fragment shader
         * @param depth_format depth attachment format (dynamic rendering only)
         * @param depth_bias_constant_factor constant rasterization depth bias added to depth
         * @param depth_bias_slope_factor slope-scaled depth bias (removes shadow acne on angled surfaces)
         * @param depth_bias_clamp maximum depth bias magnitude (0 = no clamp)
         * @return vk_pipeline on success, error message on failure
         */
        std::expected<vk_pipeline, std::string_view> make_depth_pipeline(
            std::span<uint8_t const> vertex_shader_code,
            std::span<uint8_t const> fragment_shader_code,
            VkFormat depth_format,
            float depth_bias_constant_factor = 0.0f,
            float depth_bias_slope_factor = 0.0f,
            float depth_bias_clamp = 0.0f) const;

        // The clustered-light-culling compute pipeline is NOT here any more: the CLUSTER PASS owns it
        // (deren.engine.pass.cluster builds it through vulkan.pipelines::build_cluster from the shared scene block
        // layout). `core::make_cluster_pipeline` built it against the core's own scene pipeline layout, which a
        // pass cannot own - and a pipeline only that pass names is that pass's to build and to release.

        /**
         * @ingroup vulkan_core
         * @brief rebuild the swapchain and every per-generation target
         * @return true when a NEW generation was actually built; false when the recreation was
         *         DEFERRED because the window has no drawable size (a minimized window reports a 0x0
         *         currentExtent, and vkCreateSwapchainKHR rejects that).
         *
         * THE RETURN VALUE IS NOT DECORATION. A caller that treats a deferred call as a rebuild
         * invalidates all the per-image state - the temporal histories, the descriptor families, the
         * layout flags - for a generation that still exists, and pays a full re-convergence for a
         * non-event: a minimize/restore dropped the GI and TAA history twice, once for the deferred
         * recreate and once for the real one.
         */
        [[nodiscard]] bool recreate_swap_chain();
        // one-time log for the "recreation deferred because the window has no drawable size" case
        // (see recreate_swap_chain); reset as soon as a recreation actually runs
        bool zero_extent_recreation_logged = false;

        /**
         * @ingroup vulkan_core
         * @brief submit the recorded command buffer for the current frame slot
         * @param command_buffer the command buffer to submit
         * @param image_index the acquired swapchain image index: it selects the per-image binary
         *        semaphore this submit signals for present to wait on
         * @return the result of vkQueueSubmit
         *
         * Signals two semaphores. This slot's TIMELINE (GPU completion, and the host pacing that
         * wait_frame_slot() blocks on) and present_ready_semaphores[image_index]. The second is a
         * BINARY semaphore per swapchain IMAGE rather than per frame slot because vkQueuePresentKHR
         * cannot wait a timeline semaphore, and present may run on a separate queue - keying the gate
         * on the image means an image is only re-acquired after its own present finished, which keeps
         * a re-signal from racing across queues.
         */
        /// @brief acquire the next swapchain image (vkAcquireNextImageKHR) for the frame slot in progress
        ///
        /// THE ACQUIRE BELONGS TO WHOEVER OWNS THE DEVICE AND THE SWAPCHAIN, which is this object: it
        /// is the primitive behind the contract's `frame_begin()` AND behind the runtime's own pacing
        /// path, so both go through one implementation instead of two copies.
        /// The POLICY for the result stays with the caller: the runtime maps OUT_OF_DATE to a
        /// swapchain rebuild plus `skipped`, while the contract's `frame_begin()` collapses anything
        /// but success into a zeroed `submit_info` (tier-1 has no error channel, plan §3.3).
        [[nodiscard]] VkResult acquire_next_image(uint32_t& image_index);
        /// the SUBMISSION PRIMITIVE behind the contract's `submit(command_buffer&)` (abi 14): signals
        /// this slot's completion timeline and the recorded image's present-ready semaphore. It
        /// changed its name from `submit` when the contract's one-argument verb arrived - the two
        /// coexisting would hide the virtual under -Woverloaded-virtual, and the primitive is an
        /// implementation detail the engine no longer names.
        VkResult submit_frame(VkCommandBuffer command_buffer, uint32_t image_index);

        /**
         * @ingroup vulkan_core
         * @brief present the rendered swapchain image, waiting on that image's binary present-ready
         *        semaphore (signaled by submit())
         * @param image_index the swapchain image index to present
         * @return the PRESENT call site's translation (present_error): SUBOPTIMAL is out_of_date
         *         here - the frame showed, but the surface is one resize from gone - and the caller
         *         rebuilds. The raw VkResult never leaves the backend: the engine classifies on the
         *         contract's error vocabulary, not on Vulkan's.
         */
        [[nodiscard]] deren::promise::rhi::error present(uint32_t image_index) const;

        /**
         * @ingroup vulkan_core
         * @brief create the G-buffer pipeline: the shared scene layout, the three gbuffer_formats
         *        color targets and a single-sampled depth attachment
         * @param vertex_shader_code raw SPIR-V of the vertex stage (pbr.vert: instancing / skinning /
         *        morphing are identical to what the forward path did)
         * @param fragment_shader_code raw SPIR-V of the fragment stage (gbuffer.frag: writes the
         *        three targets and shades nothing)
         * @param first_stage the stage that emits the geometry: VERTEX for the input-assembler path (the default),
         *        MESH for a MESH entry that fetches its own vertices (docs/mesh_shaders.md step 2) - the fragment
         *        stage is the same shader either way, which is what makes the two pipelines comparable
         * @return vk_pipeline on success, error message on failure
         * @note single-sampled on purpose (a G-buffer cannot be multisampled without per-sample
         *       shading), so this pipeline may NOT be recorded into an instance whose attachments are
         *       the HDR target: its own attachments are the core::gbuffer_* targets and the pass that owns them
         */
        std::expected<vk_pipeline, std::string_view> make_gbuffer_pipeline(
            std::span<uint8_t const> vertex_shader_code,
            std::span<uint8_t const> fragment_shader_code,
            VkShaderStageFlagBits first_stage = VK_SHADER_STAGE_VERTEX_BIT) const;

        /**
         * @brief the CHARACTER-FORWARD pipeline: the toon shading stage that OVERWRITES the deferred-lit
         *        character pixels, ONE HDR colour target, no blending, depth test on, depth compare EQUAL
         *
         * WHAT MAKES IT DIFFERENT FROM THE OTHER FORWARD BUILDER, and each difference is forced:
         *  - ONE target, and it is `hdr_format`, not the swapchain format. The named forward pipelines
         *    (`runtime::make_pipeline`: `unlit`, `pbr`, `pbr_premult`) are built for the SWAPCHAIN image
         *    because that is what a forward session's default target was; this pass runs INSIDE the HDR
         *    chain, before the resolve, so that the grade and the tonemap stay the post chain's job. A
         *    toon stage that wrote the swapchain would be tonemapping a second time.
         *  - BLENDING OFF (`make_color_blend_attachment_opaque`), because the pass OVERWRITES. The named
         *    forward pipelines blend src-alpha, which is right for coverage and wrong here: the toon result
         *    is a finished colour, not a layer.
         *  - DEPTH COMPARE `EQUAL`. See make_depth_stencil_state's second parameter: this is the only
         *    caller in the renderer that needs an operator other than LESS_OR_EQUAL.
         *
         * DEPTH WRITE IS NOT SET HERE because it is a dynamic state: the pass turns it off per draw, the
         * same way the outline pass does. Leaving it at the builder's default (on) would let a toon
         * fragment write depth over the surface the lighting stage already committed.
         *
         * @param vertex_shader_code the MESH stage that emits the geometry (docs/mesh_shaders.md step 4:
         *        there is no vertex geometry path left, so this is the pbr mesh entry, exactly as the
         *        named forward pipelines use)
         * @param fragment_shader_code the toon fragment stage
         * @param first_stage MESH for the mesh entry, MESH for the meshlet entry
         * @return vk_pipeline on success, error message on failure
         */
        std::expected<vk_pipeline, std::string_view> make_character_forward_pipeline(
            std::span<uint8_t const> vertex_shader_code,
            std::span<uint8_t const> fragment_shader_code,
            VkShaderStageFlagBits first_stage = VK_SHADER_STAGE_MESH_BIT_EXT) const;

        /**
         * @ingroup vulkan_core
         * @brief build the OVERLAY pipeline: the article's two framebuffer multiplies (`MyZmdEyeDarkShader`,
         *        `MyZmdHairShadowShader`), which darken an already shaded character by a mask
         *
         * WHAT MAKES IT AN OVERLAY, and each of the four facts is forced:
         *  - ONE target, and it is `hdr_format`: the multiply has to read the TOON result, which lives in the
         *    HDR chain before the resolve - the same reason `make_character_forward_pipeline` takes this format.
         *  - BLEND `dst = src * dst` (`make_color_blend_attachment_multiply`). This is the article's
         *    `BlendOp Multiply` / `Blend 5,1` expressed in core Vulkan factors, so no blend extension is needed;
         *    see that function for the derivation and for why the alpha channel is left alone.
         *  - DEPTH TEST ON, compare `LESS_OR_EQUAL` (and NOT the toon stage's `EQUAL`): an overlay is a different
         *    mesh whose quads sit slightly in FRONT of the surface they darken, so `EQUAL` would reject it.
         *  - DEPTH WRITE is not set here because it is a DYNAMIC state: the character-forward pass has already
         *    turned it off and LOCKED it for the whole instance (see `render_environment::depth_write_locked`),
         *    which is also what keeps the mask from occluding anything.
         *
         * WHAT IS NOT REPRODUCED IS THE ARTICLE'S STENCIL on the hair shadow (`Stencil { Ref 1 Comp Equal Pass
         * Keep }`), and it is unreachable rather than omitted: this renderer's dynamic rendering info declares
         * `stencilAttachmentFormat = VK_FORMAT_UNDEFINED` (see source/backends/vulkan/constant_init), so no pass in the chain has
         * a stencil plane to test against. The substitute is the geometry's own coverage - the mask meshes are
         * shaped to the features they darken - which is why the difference is recorded here rather than hidden.
         *
         * @param vertex_shader_code the MESH stage that emits the geometry (docs/mesh_shaders.md step 4: the
         *        geometry path is mesh-only, so this is `pbr.mesh.spv`, exactly as the toon stage uses)
         * @param fragment_shader_code the overlay fragment stage (`overlay.frag.spv`)
         * @param first_stage MESH for the mesh entry, MESH for the meshlet entry
         * @return vk_pipeline on success, error message on failure
         */
        std::expected<vk_pipeline, std::string_view> make_overlay_pipeline(
            std::span<uint8_t const> vertex_shader_code,
            std::span<uint8_t const> fragment_shader_code,
            VkShaderStageFlagBits first_stage = VK_SHADER_STAGE_MESH_BIT_EXT) const;
        /**
         * @brief the ARTICLE'S ① 描边 pipeline: the inverted hull, drawn over the toon result
         *
         * THREE STATES, and each is the article's rather than a preference: ONE HDR colour target (the hull is part
         * of the character stage, so it lands where that stage wrote and the tonemap stays the post chain's), an
         * OPAQUE blend (`MyZmdOutlineShader` states no `Blend`), and `LESS_OR_EQUAL` - the article's own `ZTest`
         * default, which is also the only operator that can keep a hull's outer ring, since those fragments are
         * not the surface the G-buffer recorded (see the definition in core.cpp).
         *
         * CULL FRONT IS NOT A PARAMETER HERE: the rasterization state is dynamic, so the front-face culling that
         * makes a hull an outline is `render_environment::forced_cull_front`, set by the character-forward pass
         * around the outline group only.
         */
        std::expected<vk_pipeline, std::string_view> make_outline_pipeline(
            std::span<uint8_t const> vertex_shader_code,
            std::span<uint8_t const> fragment_shader_code,
            VkShaderStageFlagBits first_stage = VK_SHADER_STAGE_MESH_BIT_EXT) const;

    private:
        // ---- THE INITIALIZATION STEPS, and they are private because the constructor is their only caller:
        //      the order in which they run IS the initialization order (see the constructor in core.cpp), and a
        //      caller that could run one on its own would be able to build half a core. The exception is the
        //      swapchain recreation path, which is a member of this class and re-runs three of them. What stays
        //      PUBLIC is the state they produce (the device, the queues, the samplers, the image views and the
        //      layouts) plus the facade operations (wait_idle, set_window_title).
        void init_instance() noexcept;
        void init_surface() noexcept;
        void init_device_and_queue() noexcept;
        void init_swap_chain() noexcept;
        void init_image_views() noexcept;
        void create_depth_image(VkImage& image, VkDeviceMemory& image_memory, VkImageView& image_view) const noexcept;
        void create_depth_resources() noexcept;
        void create_color_resources();
        void create_samplers();
        void create_sync_objects();
        void create_timestamp_query_pool() noexcept;
    };

    // ---------------------------------------------------------------------------
    // The error mechanism's backend half (the error-mechanism batch, DYNAMIC_LINK
    // handoff §4.3/§4.5): the producer and the per-call-site translators. These are
    // The backend's own vocabulary - nothing here is on the C ABI; the contract
    // carries the types, this namespace produces and fills them. The declarations
    // are exported so the tables' unit test (source/tests/test_error_mapping.cpp) feeds
    // the REAL translators; the engine never calls them - its error channel lands
    // with the frame face.
    // ---------------------------------------------------------------------------

    // THE THREE TRANSLATORS AND `failed` MOVED TO `deren.vulkan.error_tables` (the SHARED flip),
    // which this module RE-EXPORTS below, so the backend's own call sites (core.api_core.cpp,
    // core.cpp, core.entry.cpp - which import this module) name them exactly as before. They had to
    // move because a module attachment is a link-time fact: a DLL can serve a module's symbols only
    // by exporting its module surface, and `deren_vulkan.dll` exports exactly one name by design.
    // The neutral module is owned by `vulkan_error_tables`, which BOTH this backend and the tables'
    // unit test link, so the test drives the real translators without importing a backend module.
} // namespace deren::vulkan
