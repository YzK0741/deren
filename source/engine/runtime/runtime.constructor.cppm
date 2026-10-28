// ============================================================================
// module: deren.engine.runtime:constructor  - construction, initialisation and resource creation
//
// The two contiguous runs of runtime.cpp that BUILD a runtime rather than drive one: the constructors
// and the resource initialization (init_scene_resources through prepare_scene_resources), then
// write_light_and_shadow_bindings, set_ibl and register_material. What stayed behind runs per frame or
// is a diagnostic (the two heap probes, which get their own partition).
//
// THE GLFW CALLBACKS CAME WITH THE CONSTRUCTOR because the constructor is what registers them; the
// window lookup they use is the same three-line helper runtime.cpp keeps. The heap-write helpers that
// sit inside the init run (logical descriptor helpers, write_heap_grid_image, write_heap_scene_buffer) came too,
// and runtime.cpp keeps a copy of the ones its own frame path still calls. Duplication of small
// file-local helpers across two anonymous namespaces is legal but it IS duplication: the follow-up is to
// publish the heap three from deren.vulkan.core:descriptor_heap, where heap plumbing belongs.
//
// Imports are NOT transitive: this partition imports what the moved code calls, and uses the shared vessel allocation service.
// ============================================================================
module;
// Pure CPU filtering and bounded residency policy shared with the standalone tests.
#include "../visibility/progressive_texture.h"

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32 // glfwGetWin32Window: the engine's window, as the native handle the
#include <GLFW/glfw3native.h>    // backend's own GLFW image can understand (see to_rhi_create_info)
#include <algorithm>             // std::min in the resource publication
#include <array>                 // the HZB cull table publication's three-entry table
#include <bit>                   // std::bit_cast for the caster world-matrix hash
#include <chrono>
#include <cstddef>    // std::ptrdiff_t in the phase-B table zeroing
#include <cstdint>    // the HZB table layout constants' arithmetic
#include <cstdlib>    // std::getenv: the DEREN_HZB_BUILD gate (read once, at construction)
#include <cstring>    // std::memcpy, for composing a pass's push block
#include <filesystem> // the backend DLL's path: executable_directory() / "deren_vulkan.dll"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>      // std::shared_ptr: the device root this half builds and owns
#include <span>        // std::as_bytes for the init_utils calls (the bytes behind a UBO or a zeroed table)
#include <string>      // the panic messages below build their text through std::format
#include <string_view> // the enabled-extension lookup in runtime_detail (see device_extension_enabled)
#include <thread>      // std::this_thread::yield in the frame limiter

module deren.engine.runtime:constructor;
import deren.utility.shared;

import :declarations;
import deren.engine.profiling;
import deren.engine.pipelines;
import deren.engine.render_resource;
import deren.engine.render_resource.shared;
import deren.engine.pass.buffer_storage;
import deren.engine.pass.hzb_build; // the runtime-owned HZB build pass (see ensure_hzb_resources)
import deren.engine.pass.hzb_cull;  // ... and its phase-B GPU consumer (the three fixed-size tables + the cull pass)
import deren.engine.pass.software_visibility;
import deren.engine.pass.software_merge;
import deren.engine.resource_cluster; // the cluster create_hzb_cull_tables creates the phase-B tables in
import deren.engine.visibility.hzb;   // the pyramid layout the HZB buffers are sized with (§4.1's one formula)

import deren.utility;
import deren.engine.frame_constants; // one frame's shared constants (see update_frame_constants)
import deren.engine.meshlet;         // the meshlet table's record layout and capacity (docs/mesh_shaders.md step 3)
import deren.promise.rhi;            // the RHI creation contract: the type the new constructor below takes

namespace {
    deren::engine::runtime* runtime_from_window(GLFWwindow* window) {
        return static_cast<deren::engine::runtime*>(glfwGetWindowUserPointer(window));
    }

    void mouse_button_callback(GLFWwindow* window, int32_t const button, int32_t const action, [[maybe_unused]] int32_t const mods) {
        auto* runtime = runtime_from_window(window);
        if (button != GLFW_MOUSE_BUTTON_LEFT) {
            return;
        }
        // the overlay owns the mouse while the cursor is over a panel / a widget is being
        // dragged: starting an orbit there would fight the ui (see gui_content::wants_mouse)
        if (runtime->debug_gui_wants_mouse()) {
            return;
        }
        if (action == GLFW_PRESS) {
            runtime->camera.dragging = true;
            glfwGetCursorPos(window, &runtime->camera.last_x, &runtime->camera.last_y);
        } else if (action == GLFW_RELEASE) {
            runtime->camera.dragging = false;
        }
    }

    void cursor_pos_callback(GLFWwindow* window, double const x, double const y) {
        auto& camera = runtime_from_window(window)->camera;
        if (!camera.dragging) {
            return;
        }
        if (runtime_from_window(window)->debug_gui_wants_mouse()) {
            // the drag left the scene and landed on the overlay: keep the camera still but keep
            // tracking the cursor so leaving the panel does not jump the view
            glfwGetCursorPos(window, &camera.last_x, &camera.last_y);
            return;
        }
        constexpr float sensitivity = 0.005f;
        float const dx = static_cast<float>(x - camera.last_x);
        float const dy = static_cast<float>(y - camera.last_y);
        camera.last_x = x;
        camera.last_y = y;
        camera.yaw += dx * sensitivity; // drag direction matches the primitive rotation
        camera.pitch -= dy * sensitivity;
        camera.pitch = std::clamp(camera.pitch, -1.5f, 1.5f); // avoid flipping
    }

    void scroll_callback(GLFWwindow* window, [[maybe_unused]] double const xoffset, double const yoffset) {
        if (runtime_from_window(window)->debug_gui_wants_mouse()) {
            return; // scrolling inside an overlay panel must not zoom the camera
        }
        auto& camera = runtime_from_window(window)->camera;
        // zoom: wheel up pulls in, wheel down pulls out. The upper bound is generous (scene
        // sizes vary from the tiny default model to e.g. the Fox rig, whose framing distance
        // is ~240). The projection far plane always covers the scene (see make_orbit_camera_ubo),
        // so zooming in never clips the far side.
        camera.distance *= std::pow(0.9f, static_cast<float>(yoffset));
        camera.distance = std::clamp(camera.distance, 0.5f, 5000.0f);
    }
} // namespace

namespace deren::engine {
    // Run a batch of tasks on the shared pool and wait for exactly this stage's group: the
    // frame phases are synchronous (the paced slot is read right after animation sampling),
    // so run_tasks blocks until every task in the batch finished. The enum tier is mapped
    // onto the pool's integer priority (see task_priority in runtime.cppm).
    void runtime::run_tasks(std::span<std::function<void()>> const tasks, task_priority const priority) {
        int32_t const pool_priority = static_cast<int32_t>(priority);
        if (tasks.empty() || !this->task_pool.post_batch(tasks, pool_priority)) {
            return; // empty batch, or the pool is shut down (never in the running demo)
        }
        this->task_pool.wait_until_priority_done(pool_priority);
    }

    bool contract_descriptor_ready(rhi::api_core& face) noexcept {
        auto* space = rhi::query_extension<rhi::descriptor_space>(face);
        if (!space || !space->ready())
            return false;
        auto range = space->fixed_slots();
        auto base = deren::engine::render_layout::heap_slot_base;
        auto count = deren::engine::render_layout::heap_slot_count;
        return range.base <= base && base - range.base <= range.count && count <= range.count - (base - range.base);
    }
    bool contract_write_descriptor_image(rhi::api_core& face, std::uint32_t slot, rhi::image const& resource,
                                         rhi::image_view_desc const& view, rhi::descriptor_type type) noexcept {
        auto* space = rhi::query_extension<rhi::descriptor_space>(face);
        return space && space->write_image({.slot = slot, .resource = &resource, .view = &view, .type = type}) == rhi::error::ok;
    }
    bool contract_write_descriptor_buffer(rhi::api_core& face, std::uint32_t slot, rhi::buffer const& resource,
                                          std::uint64_t size, rhi::descriptor_type type) noexcept {
        auto* space = rhi::query_extension<rhi::descriptor_space>(face);
        return space && space->write_buffer({.slot = slot, .resource = &resource, .size = size, .type = type}) == rhi::error::ok;
    }
    bool contract_write_descriptor_structure(rhi::api_core& face, std::uint32_t slot, rhi::acceleration_structure const& resource) noexcept {
        auto* space = rhi::query_extension<rhi::descriptor_space>(face);
        return space && space->write_acceleration_structure({.slot = slot, .resource = &resource}) == rhi::error::ok;
    }
    void contract_record_descriptor_bind(rhi::api_core& face, rhi::command_recorder& commands) noexcept {
        auto* space = rhi::query_extension<rhi::descriptor_space>(face);
        if (space) {
            [[maybe_unused]] auto result = space->bind({.commands = &commands});
        }
    }

    // (THE HEAP-BIND HOOK IS GONE - plan X5 B3.5: it mirrored the contract's `heap_bindings()` into the two raw
    //  `VkBindHeapInfoEXT` a secondary used to inherit. The pass layer stopped calling it at abi 20 and the
    //  BACKEND derives that inheritance from its own bound heaps now, so this was a second truth with no reader.)

    // ---- runtime_detail: THE ONE DERIVATION OF EVERY BACKEND FACT THE ENGINE READS (③-D/E step 2) ----
    //
    // DEFINED HERE, in a partition, rather than in the declarations interface, and that is the same
    // measured rule the `contract_*` helpers above state: a definition in `:declarations` would let a
    // call site inline the concrete backend's `query_extension` and emit a backend symbol into this half
    // - which is what the boundary meter counts. The declaration is in `:declarations` so every partition
    // can call them; the definitions are here, in the one partition every runtime already links.
    namespace runtime_detail {
        // `physical_properties_of` STOOD HERE, and the push-constant limit was the only field anyone read out of
        // it - so it is `max_push_constants_of` below now: one integer, from the ability that already holds the
        // query's answer, instead of a whole `VkPhysicalDeviceProperties` fetched per call site.

        deren::promise::rhi::shader_binding_table_properties ray_tracing_properties_of(rhi::api_core& face) noexcept {
            // THE SHADER-BINDING-TABLE NUMBERS, from the device-capability ability: three integers a pass cannot
            // query itself (it has no physical device). This used to build a `VkPhysicalDeviceProperties2` chain,
            // ask the device and copy the three fields out - and the BACKEND made exactly that query at startup
            // to decide whether ray tracing was available at all, so it already holds the answer. Asking it is
            // both shorter and the only shape a second backend can serve. Zeroed on a device without the
            // extension, which the pass reads as "build no table".
            rhi::device_capabilities* const capabilities = rhi::query_extension<rhi::device_capabilities>(face);
            return capabilities == nullptr ? deren::promise::rhi::shader_binding_table_properties{} : capabilities->shader_binding_table();
        }

        bool ray_query_available_of(rhi::api_core& face) noexcept {
            // THE DEVICE'S OWN ANSWER, ASKED THROUGH THE ABILITY THAT EXISTS FOR IT (see
            // `rhi::device_capabilities`). This used to be TWO engine-side halves - "is VK_KHR_ray_query in the
            // ESCAPE's enabled list" and "does `vkGetPhysicalDeviceFeatures2` report the feature" - which is
            // the same pair the backend enables the extension under, re-derived every call (this helper has
            // SIX call sites, several per frame). The backend recorded that decision when it made it; asking it
            // cannot disagree with the enabling, and it removes both the entry point and the `VK_KHR_` spelling
            // from this half.
            rhi::device_capabilities* const capabilities = rhi::query_extension<rhi::device_capabilities>(face);
            return capabilities != nullptr && capabilities->ray_query();
        }

        bool mesh_shader_available_of(rhi::api_core& face) noexcept {
            // the same question for the mesh stage (see `ray_query_available_of`)
            rhi::device_capabilities* const capabilities = rhi::query_extension<rhi::device_capabilities>(face);
            return capabilities != nullptr && capabilities->mesh_shader();
        }

        std::uint32_t max_push_constants_of(rhi::api_core& face) noexcept {
            // THE STAGE BLOCK'S OWN LIMIT, from the same ability: it used to be
            // `physical_properties_of(face).limits.maxPushConstantsSize`, a whole `VkPhysicalDeviceProperties`
            // fetched (and re-fetched) to read ONE integer out of it.
            rhi::device_capabilities* const capabilities = rhi::query_extension<rhi::device_capabilities>(face);
            return capabilities == nullptr ? 0u : capabilities->max_push_constants_size();
        }
    } // namespace runtime_detail

    namespace {
        /**
         * @brief THE DEVICE ROOT, CHECKED, OR A PANIC - construction's FIRST act (abi 19, batch ⑥).
         *
         * THE ENGINE NO LONGER ACQUIRES ANYTHING (that is `deren.engine.backend_loader`, called by the
         * application), so this is what replaces the two functions that used to live here: a root is HANDED
         * OVER, and this half checks the two things it can still be wrong about.
         *
         * TWO REFUSALS, BOTH FATAL, AND THE ENGINE IS THE ONE THAT EXECUTES THEM (the startup rule this path
         * already follows - the backend reports, the engine decides):
         *
         *   1. AN EMPTY `shared_ptr`. `vulkan_core` is a REFERENCE, not a maybe-root, because everything below
         *      is written against a device that EXISTS; "no root" cannot be represented here and must not be
         *      papered over with a null dereference somewhere in the frame loop. The loader has already
         *      reported WHY (its own named diagnosis), so this panic is the consumption side saying "and the
         *      renderer will not start without one".
         *   2. `api_core::api_version()` DISAGREEING WITH `rhi::abi_version` - the CONSUMPTION-side handshake,
         *      and it covers what the entry's first argument cannot: an object this image did not create (a
         *      probe, a wrapper, a backend some other loader produced). A different number means the two halves
         *      disagree about this vtable and about the layout of every structure the contract passes by
         *      value, so continuing is undefined behaviour rather than a degraded mode. THE PANIC NAMES BOTH
         *      NUMBERS: which side is stale is the first thing a reader needs.
         *
         * IT RUNS IN THE MEMBER-INITIALISER LIST, before the constructor body and before any member that is
         * built FROM the root (`vulkan_core` itself, the pass filter, every resource): a check that ran later
         * would already have used it.
         */
        [[nodiscard]] rhi::api_core& require_core(rhi::shared_object<rhi::api_core> const& core) {
            if (core == nullptr) {
                deren::utility::panic(std::source_location::current(),
                                      "runtime: no device root was handed over (an empty shared_ptr<api_core>) - the "
                                      "loader (deren.engine.backend_loader::load_api_core) reports why it failed");
            }
            runtime_detail::verify_contract_version(*core);
            return *core;
        }
    } // namespace

    void runtime_detail::verify_contract_version(rhi::api_core const& core) noexcept {
        // THE CONSUMPTION-SIDE HANDSHAKE (abi 19): the object's OWN number against the one this image
        // compiled. It covers what the entry's first argument cannot - an `api_core` this image did not
        // create (a probe, a wrapper, a backend some other loader produced) - and a mismatch is FATAL
        // rather than recoverable: the two halves then disagree about this vtable and about the layout of
        // every structure the contract passes by value, so no call on the object is safe. BOTH NUMBERS
        // ARE IN THE MESSAGE, because which side is stale is the first thing a reader needs.
        //
        // IT IS A NAMED function rather than a few lines inside `require_core` so the engine-side
        // injection test can drive the AGREEING outcome in-process and the REFUSAL in a child process
        // (a panic cannot be observed from inside the process it kills).
        std::uint32_t const reported = core.api_version();
        if (reported != rhi::abi_version) {
            deren::utility::panic(std::source_location::current(),
                                  "runtime: the device root reports abi {} but this executable compiled abi {} - the two "
                                  "halves disagree about the contract, so no call on it can be made safely",
                                  reported, rhi::abi_version);
        }
    }

    rhi::api_core& runtime::rhi_face() const noexcept {
        // §18's rule in one line: the interface reference of the SAME object. Before the flip the
        // vtable points into this archive; after it, into the DLL - the call sites cannot tell.
        return this->vulkan_core;
    }

    // THE ONE CONSTRUCTION ENTRY (batch ⑥): the APPLICATION acquires the device root
    // (`deren.engine.backend_loader::load_api_core`, called by main.cpp and by the scaffold test) and
    // hands it over together with the SAME `create_info` that produced it - one descriptor, no second
    // options structure, no hidden acquisition. This half loads nothing and resolves nothing: it checks
    // the root it was given (`require_core` above, the first thing the delegating constructor reaches)
    // and builds the renderer from it.
    //
    // AN EMPTY ROOT IS REFUSED LOUDLY rather than tolerated, which is the invariant the reference member
    // `vulkan_core` has always implied: the engine's startup path decides what a failed acquisition means
    // (main.cpp reports and exits), and this constructor is where "there is no root" stops being
    // representable.
    runtime::runtime(rhi::shared_object<rhi::api_core> core, deren::promise::rhi::create_info const& options)
        : runtime(options,
                  std::move(core),
                  // THE CLAMP IS APPLIED HERE, BY THE ONE OWNER OF THE RULE: the backend clamps the same
                  // value for the swapchain it sizes, and the engine must size the targets IT creates by
                  // the same number - `render_layout::clamp_render_scale` is that one spelling (see its
                  // note), and this is the engine's call to it.
                  deren::engine::render_layout::clamp_render_scale(options.render_scale)) {
    }

    runtime::runtime(deren::promise::rhi::create_info const& options, rhi::shared_object<rhi::api_core> shared_core, float const clamped_render_scale)
        : core_owner{std::move(shared_core)}
        , vulkan_core{require_core(this->core_owner)}
        , create_options{options}
        , render_scale{clamped_render_scale}
        , filtered_core{core_owner} {
        // `pass_resources` is NOT in this list any more (S2's shared filter batch): the pass filter holds no
        // device root at all now, so it default-constructs its (empty) resource table. Naming it would be an
        // initializer for a member the list cannot reach - see filters.cppm's measured note.

        // Two-tier texture streaming is opt-in; retain the synchronous GPU path as the default.
        this->progressive_textures = std::getenv("DEREN_PROGRESSIVE_TEXTURES") && std::string_view(std::getenv("DEREN_PROGRESSIVE_TEXTURES")) == "1";
        this->progressive_mips = progressive_textures && std::getenv("DEREN_PROGRESSIVE_MIPS") && std::string_view(std::getenv("DEREN_PROGRESSIVE_MIPS")) == "1";
        auto residency_limit = [](char const* name, std::uint64_t initial) {
            auto* value = std::getenv(name);
            if (!value)
                return initial;
            char* end = nullptr;
            auto parsed = std::strtoull(value, &end, 10);
            return *value && !*end && parsed <= 16384 ? parsed * 1024ull * 1024ull : initial;
        };
        this->progressive_budget = residency_limit("DEREN_PROGRESSIVE_BUDGET_MB", progressive_budget);
        this->progressive_upload_budget = residency_limit("DEREN_PROGRESSIVE_UPLOAD_MB", progressive_upload_budget);
        if (auto* trace = std::getenv("DEREN_PROGRESSIVE_TRACE")) {
            progressive_trace.open(trace);
            progressive_trace << "update,slot,high_bytes,floor_bytes,pending_bytes,jobs,wanted,high_ready,fallback,started,failed,resident_high_bytes,retired_bytes,checkpoint,update_ms,target_ready,intermediate,full,uploaded_bytes,screen_requests\n";
        }
        // Unset means no HZB allocations, pipelines or passes.
        this->hzb_build_enabled = std::getenv("DEREN_HZB_BUILD") != nullptr;
        // ---- THE HZB CULL LANE'S GATE (M3 phase B1), ALSO READ ONCE ----
        // DEREN_HZB_CULL is the cull lane's switch and it REQUIRES DEREN_HZB_BUILD: the lane tests against the
        // history pyramid the build lane produces, so "cull without build" has nothing to read. Unset (or set
        // without the build gate) means no phase-B buffer is allocated, no cull pipeline is created and no cull
        // lane is appended - the phase-A2 run and the OFF run are both exactly themselves.
        this->hzb_cull_enabled = this->hzb_build_enabled && std::getenv("DEREN_HZB_CULL") != nullptr;
        this->hzb_recovery_enabled = this->hzb_cull_enabled && std::getenv("DEREN_HZB_RECOVERY") != nullptr;
        auto motion_limit = [](char const* name, double fallback) noexcept {
            char const* text = std::getenv(name);
            if (text == nullptr) {
                return fallback;
            }
            char* end = nullptr;
            double const value = std::strtod(text, &end);
            if (end == text || *end != '\0' || !std::isfinite(value) || value <= 0.) {
                return fallback;
            }
            return value;
        };
        this->hzb_recovery_limits.translation_ratio = motion_limit("DEREN_HZB_RECOVERY_MAX_TRANSLATION_RATIO", .05);
        this->hzb_recovery_limits.rotation_degrees = motion_limit("DEREN_HZB_RECOVERY_MAX_ROTATION_DEGREES", 5.);
        this->hzb_recovery_limits.projection_delta = motion_limit("DEREN_HZB_RECOVERY_MAX_PROJECTION_DELTA", .01);

        // ---- THE SESSION-STABLE FORMATS AND DEVICE CAPABILITIES, ASKED ONCE (③-D/E step 2) ----------
        // THE SURFACE FORMAT: abi 17's accessor is the only answer that exists BEFORE a frame is acquired,
        // and the pipelines that render into the presentation image are created up front (see the
        // contract's own note on the accessor). Asked here, in the constructor, and read from this member
        // by `make_pipeline` and `make_pass_context`.
        this->swap_chain_image_format = this->rhi_face().frames->frame_swapchain()->format();
        // THE DEVICE'S CAPABILITIES: a device fact does not change while the device lives, so the five
        // legacy `core::ray_query_available` / `core::mesh_shader_available` reads become one query each.
        // The two derivations are the ENGINE's, from facts the escape exposes (the enabled device
        // extensions and the physical device's own feature chain), and they answer exactly what the
        // backend's own flags answer - the backend enables a feature iff the device supports it and the
        // extension is present, which is what these check.
        this->ray_query_available = runtime_detail::ray_query_available_of(this->rhi_face());
        this->mesh_shader_available = runtime_detail::mesh_shader_available_of(this->rhi_face());
        // ... AND THE GRAPHICS QUEUE'S FAMILY, derived ONCE (see runtime_detail::graphics_queue_family_of):
        // the probes allocate their own command pools outside the frame ring, and a pool's family index
        // must be the one the backend's queue belongs to. A failure inside the derivation PANICS there.
        this->graphics_queue_family_index = runtime_detail::graphics_queue_family_of(this->rhi_face());
        // THE TWO MESH-DISPATCH ENTRY POINTS ARE THE BACKEND'S, AND THEY ALWAYS WERE: this block used to resolve
        // `vkCmdDrawMeshTasksEXT`/`...Indirect` through `vkGetDeviceProcAddr` and call them from `runtime.cpp`
        // through `native_handle()`. The BACKEND had resolved the same two pointers at its own startup (its
        // `mesh_dispatch` members) and serves them as the contract's `draw_mesh_tasks()` /
        // `draw_mesh_tasks_indirect()`, so the engine's copy was a second resolution of one entry point - and
        // the only reason the engine could call it at all was the raw handle it borrowed back for the argument
        // (see `runtime::draw_mesh_tasks`). Two `PFN_` members, two `reinterpret_cast`s, one `VkBuffer` table
        // and `sizeof(VkDrawMeshTasksIndirectCommandEXT)` are gone with it.
        // The capability that GATES the path is asked once, through the ability that answers it
        // (`device_capabilities::mesh_shader()` - see `runtime_detail::mesh_shader_available_of`).

        // ---- THE FRAME'S RESOLUTION IS THE ENGINE'S OWN FROM HERE ON (abi 14) ---------------------
        // The scale both halves use was clamped ONCE above (the option is startup-only: `render_scale` is
        // read when the render targets are created, so the two numbers cannot drift). The swapchain the
        // backend created is the generation every extent below is relative to, and it is read through the
        // CONTRACT (`frame_swapchain()->extent()`) rather than out of a context field - which is what
        // lets the backend's own `render_extent()` leave the engine's boundary worklist. Every later
        // generation arrives through the very same function (see on_swapchain_recreated).
        this->refresh_frame_extents();

        // ---- THE RING DEPTH IS PINNED, AND A MISMATCH PANICS (③-D/E batch) ------------------------
        // The engine sizes per-slot C++ with the CONTRACT's compile-time depth
        // (`rhi::max_frames_in_flight`: `shadow_rendered_version` / `shadow_rendered_models` are arrays
        // of it), while the RUNTIME truth is the backend's ring - `frame_walker::slot_count()`, the one
        // authority. A backend built from a different revision compiled against its own copy of the
        // contract, so the backend's own static_assert cannot see this pairing; this is where the two
        // numbers meet, and a mismatch must PANIC here rather than write past the end of a per-slot
        // array on the first frame. Checked once, at construction, before anything uses the arrays.
        if (this->frame_ring().slot_count() != rhi::max_frames_in_flight) {
            deren::utility::panic(std::source_location::current(),
                                  "runtime: the backend's frame ring is {} slots but rhi::max_frames_in_flight promises {} - "
                                  "the engine's per-slot arrays are sized by the contract's number",
                                  this->frame_ring().slot_count(),
                                  rhi::max_frames_in_flight);
        }

        // ---- THE ONE PLACE GLFW CALLBACKS ARE INSTALLED, and it stays here now that the WINDOW belongs to
        //      the APPLICATION: the callbacks dereference THIS runtime through the window's user pointer, so
        //      they can only be installed once both exist - which is exactly this constructor, running after
        //      the caller created the window and handed it over. The application installs no GLFW callback
        //      and must not touch the user pointer (it is this object's slot). The debug overlay's ImGui
        //      backend chains onto whatever is installed here (ImGui_ImplGlfw_InitForVulkan with
        //      install_callbacks = true, later), so the ORDER - orbit camera first, ImGui after - must not
        //      move.
        //
        // ONLY WHEN THE CALLER HANDED ONE OVER, and that is the contract's own shape rather than a
        // shortcut (③-D/E step 2): `create_info::native_window` IS the window this half can name - the
        // legacy runtime read the BACKEND's `core::window`, which is either that same borrowed pointer or
        // a window the backend created because the caller passed none, and the contract deliberately
        // exposes neither (a window system is a backend detail; the field is an opaque `void*` the CALLER
        // fills). A runtime built without a caller window therefore installs no orbit-camera callbacks -
        // there is no slot it could borrow - and the shipped application always hands its window over
        // (main.cpp), so the interactive path is unchanged.
        if (this->create_options.native_window != nullptr) {
            auto* const window = static_cast<GLFWwindow*>(this->create_options.native_window);
            glfwSetWindowUserPointer(window, this);
            glfwSetMouseButtonCallback(window, mouse_button_callback);
            glfwSetCursorPosCallback(window, cursor_pos_callback);
            glfwSetScrollCallback(window, scroll_callback);
        }

        // The recording resources - one primary command buffer per frame slot, plus the secondary
        // buffers (and the per-consumer pools that must own them) the parallel recording stages hand
        // out - are built by init_recording_resources(): they depend on nothing else in this
        // constructor but the two capacities, and nothing else here reads them.
        this->init_recording_resources();
        // Scene tables must exist before default material/neutral toon values and animation setup write them.
        if (this->prepare_managed_resources(true) != rhi::error::ok) {
            deren::utility::panic("managed scene setup resources could not be prepared");
        }

        // Shared legacy scene resources: white fallback texture and scene tables/deformation buffers
        this->init_scene_resources();
        // The six samplers the declaration layer hands out by hint (③-D/E item C): created here, before any
        // pass resolves a binding, because `shared_samplers()` is what a declaration asks for a sampler with.
        this->init_shared_samplers();
        // THE RENDER CHAIN'S PER-IMAGE TARGETS THE ENGINE OWNS (③-D/E A1): generation 0, created through
        // the contract right here - the backend no longer creates the TAA history pair, and this is where
        // the engine takes it over. Every later generation arrives through the same function from
        // `on_swapchain_recreated`. It runs BEFORE anything publishes these images (the frame's resource
        // table names them per frame) and AFTER `refresh_frame_extents()` above, which is where the render
        // extent it sizes them by comes from.
        this->create_render_chain_targets();
        // Every per-image flag that describes this generation starts where the generation's images do.
        // The core has already built the groups it still owns (its constructor ran
        // create_render_targets), so the flags can be sized HERE, before any frame records; every
        // later generation gets the very same reset from on_swapchain_recreated - one function, so the
        // two lists cannot drift (which they already had).
        this->reset_image_generation_state();
        // (The furnace cube is a new image too - its level is part of the generation reset above.)
        // Camera buffers are also prepared here for setup-time heap publication.
        // NOTE: shadow, cluster and light/head constants are created lazily by
        // prepare_managed_resources() at the start of pace_and_acquire(). The shadow map is a layered 2D array
        // whose layer count is [render] shadow_cascades, and the app config that carries it is applied
        // after this constructor returns - creating them here would freeze the count at its default.
        // Setup can leave these empty. The first frame prepares them before updating light
        // buffers or publishing resources, even when no geometry was imported.
    }

    // The destructor body runs before member destruction, so vulkan_core (and the VkDevice it
    // holds) is still alive here: destroying cached pipelines in this order is guaranteed safe,
    // independent of future member reordering. Members then destruct in reverse declaration
    // order with pipelines already empty. The SCENE TREE is caller-owned (set_scene): the caller
    // destroys it before this runtime goes away (its leaves release GPU buffers through the vma
    // allocator while it is still alive), so no tree teardown happens here.
    runtime::~runtime() {
        // Workers may submit upload barriers; finish them before the final GPU idle wait.
        for (auto& entry : streamed_textures)
            if (entry.upload.valid())
                entry.high = entry.upload.get();
        // (no vma statistics log here: the accounting moved to the backend's own lifecycle point -
        // the allocator's teardown logs what the session held, which is where that decision belongs)
        // Wait for the GPU to finish BEFORE releasing anything below: the last submitted frame
        // may still be executing and destroying in-use resources would violate VUIDs (~core()
        // also waits, but that runs after this body — too late for the VMA frees here).
        this->vulkan_core.wait_idle();
        if (this->pending_present.valid()) {
            rhi::error const result = this->pending_present.get();
            if (result != rhi::error::ok && result != rhi::error::out_of_date) {
                deren::utility::log("runtime: pending present was refused during shutdown ({})", static_cast<uint32_t>(result));
            }
            this->pending_present = {};
        }

        this->pipelines.clear();
        // THE SESSION'S CULLING TOTALS, printed HERE rather than per frame: the counters are cumulative, and this is
        // the one point where the GPU has finished (wait_idle above) and the numbers are final - so a run of any
        // length reports exactly what the meshlet path did, without a read-back inside the frame loop.
        this->log_meshlet_stats();
        // THE LAST CULLING FRAME'S HZB COUNTS, printed HERE for the same reason: the stats block is GPU-written and
        // this is the one point where the GPU has finished (wait_idle above), so a plain mapped read is the whole
        // read-back. It is the phase-B evidence line the design's §10.3 asks for - the culled/survivor split has no
        // other CPU-visible source without a per-frame stall (the CANDIDATE total, which the host knows from its
        // item plans, is reported per frame by the cull lane's own line).
        this->log_hzb_cull_stats();
        // THE SESSION'S GPU TIMING AVERAGE, printed HERE for the same reason as the meshlet totals: the GPU has
        // finished (wait_idle above), so this is the one point where a whole-run average is final. The per-window
        // reports during the run are once per wall second (see collect_gpu_timings), not once per 60 frames.
        this->log_gpu_timing_average();

        // post-process objects: the FXAA pipeline and the two samplers are RAII members, and the post chain's
        // two pipelines belong to the post composite PASS (vulkan.pass.post::release_owned), the same rule
        // every extracted pass follows.
        //
        // NO SET LAYOUT, PIPELINE LAYOUT OR DESCRIPTOR POOL IS TORN DOWN HERE ANY MORE, and that is the whole
        // point of the deletion this destructor records: every stage is heap-native, so there is nothing of that
        // kind left in this class to destroy - no scene set layout (it was `core`'s), no post or G-buffer set
        // layout (this class created them for the families it wrote), no pool and no per-image family. What
        // remains of each extracted pass's GPU material is the PASS's, released by its own destructor.

        // Shared scene resources: views/samplers/buffers/images are RAII and free
        // themselves as this runtime's members destruct (after this body; vulkan_core, which
        // owns the vma allocator, is declared first and destructs last, so every vk_buffer /
        // vk_image still has a live allocator when it releases).

        // Shut the debug overlay down explicitly while the VkDevice is still alive (its ImGui
        // Vulkan backend owns device resources); member destruction would also run it before
        // vulkan_core, but doing it here keeps the order obvious.
        // THE PLUGIN IS RELEASED HERE TOO (plan X2): dropping the last reference destroys the overlay inside
        // `deren_gui_vulkan.dll`, the image that allocated it - and it has to happen while the device is
        // alive, which is why this is explicit rather than left to member order.
        if (this->debug_overlay != nullptr) {
            this->debug_overlay->shutdown();
            this->debug_overlay.reset();
        }
    }

    // THE SESSION'S MESH CULLING TOTALS (docs/mesh_shaders.md step 3, "what the culling buys"). Read from the
    // mapped counter buffer after `wait_idle`: the sums are per session, and the two DERIVED numbers are the point
    // of the line - the share of workgroups the frustum test rejected, and what that share means for a compute pass
    // (a rejected meshlet costs a workgroup launch today, and would cost nothing at all if the culling happened
    // before the dispatch). The VISIBILITY raster lane reports the same four numbers on its own line: it is a third
    // mesh stage, so its workgroups are not in the meshlet entries' totals and a pre-dispatch cull for it needs its
    // own witness.
    void runtime::log_meshlet_stats() const {
        if (this->meshlet_stats_mapped == nullptr) {
            return;
        }
        auto const* const counters = static_cast<uint32_t const*>(this->meshlet_stats_mapped);
        // SIXTEEN IS THE SHADER'S `mesh_stat_stride` (mesh_geometry.slang), not the number of ids this function
        // happens to name: a sum that used the latter would read a slot's lane from the wrong offset as soon as a
        // lane is added, and the totals would still look plausible - the counter buffer is one descriptor with the
        // per-frame lanes inside it, so a wrong stride is a silent cross-slot read.
        constexpr uint32_t stat_stride = 16u;
        uint64_t total[stat_stride] = {};
        for (int32_t slot = 0; slot < static_cast<int32_t>(this->frame_ring().slot_count()); ++slot) {
            for (uint32_t counter = 0; counter < stat_stride; ++counter) {
                total[counter] += counters[static_cast<std::size_t>(slot) * stat_stride + counter];
            }
        }
        uint64_t const workgroups = total[0];
        uint64_t const culled = total[1];
        uint64_t const emitted = total[2];
        uint64_t const triangles = total[3];
        uint64_t const mesh_workgroups = total[4];
        // THE VISIBILITY RESOLVE'S WITNESSES (visibility-buffer v0.4b): two counters its compute stage increments on
        // FAILING pixels only, so a healthy frame prints nothing and a nonzero number names the known v0 limits -
        // pixels whose id was valid but whose triangle could not be rebuilt (the near-plane clip v0 does not do), and
        // pixels whose rebuilt z disagreed with the frame's G-buffer depth (the id and the depth naming different
        // triangles).
        // Logged BEFORE the mesh-workgroup early return: a session whose mesh stages were all quiet can still have
        // had a resolve pass, and this is the only evidence that the ids/depth pair was actually consumed.
        uint64_t const visibility_unresolved = total[5];
        uint64_t const visibility_depth_mismatch = total[6];
        uint64_t const visibility_clipped = total[7];
        if (visibility_unresolved != 0u || visibility_depth_mismatch != 0u || visibility_clipped != 0u) {
            deren::utility::log("visibility resolve: {} pixel(s) could not rebuild their triangle (of which {} crossed the near plane - the clip item 3 lands recovers those), {} pixel(s) had an id/depth disagreement",
                                visibility_unresolved,
                                visibility_clipped,
                                visibility_depth_mismatch);
        }
        // THE VISIBILITY LANE'S OWN CULL TOTALS (mesh_geometry.slang's ids 8..11): a third mesh stage, and the
        // reason this line exists is the decision it informs - a compute pass that culled the visibility meshlets
        // BEFORE the dispatch would not launch `culled` of those workgroups at all.
        uint64_t const visibility_workgroups = total[8];
        uint64_t const visibility_culled = total[9];
        uint64_t const visibility_emitted = total[10];
        uint64_t const visibility_triangles = total[11];
        // EACH LANE DECIDES ITS OWN SILENCE: a session can run the visibility raster without either meshlet entry
        // (or the other way round), and the all-zero requirement is "no mesh stage ran", not "the pbr/shadow totals
        // are zero" - the latter would suppress a visibility line that has real numbers.
        bool const mesh_lane_ran = workgroups != 0u || mesh_workgroups != 0u;
        bool const visibility_lane_ran = visibility_workgroups != 0u || visibility_culled != 0u || visibility_emitted != 0u || visibility_triangles != 0u;
        if (!mesh_lane_ran && !visibility_lane_ran) {
            return; // no mesh stage ran: a vertex-path session says nothing rather than reporting zeroes
        }
        // THE EXISTING LINES ARE UNCHANGED (same condition, same text, same order) - they describe the pbr/shadow
        // meshlet entries and the non-meshlet mesh path, which the visibility lane's counters do not touch.
        if (mesh_lane_ran) {
            deren::utility::log("mesh culling: {} meshlet workgroups, {} emitted their triangles, {} were culled ({}% of the workgroups emitted nothing), {} triangles from meshlets",
                                workgroups,
                                emitted,
                                culled,
                                workgroups == 0u ? 0u : (culled * 100u) / workgroups,
                                triangles);
            deren::utility::log("mesh culling: {} workgroups of the NON-meshlet mesh path ran too, so this session's geometry cost {} workgroup launches either way - a compute pass that culled first would record {} fewer",
                                mesh_workgroups,
                                workgroups + mesh_workgroups,
                                culled);
        }
        if (visibility_lane_ran) {
            deren::utility::log("mesh culling (visibility lane): {} meshlet workgroups, {} emitted their triangles, {} were culled ({}% of the workgroups emitted nothing), {} triangles from meshlets - a compute pass that culled before the dispatch would record {} fewer",
                                visibility_workgroups,
                                visibility_emitted,
                                visibility_culled,
                                visibility_workgroups == 0u ? 0u : (visibility_culled * 100u) / visibility_workgroups,
                                visibility_triangles,
                                visibility_culled);
        }
    }

    std::pair<rhi::shared_object<rhi::image>, rhi::shared_object<rhi::image_view>> runtime::create_shared_image(render_resource::resource_id id, rhi::image_desc desc) {
        namespace managed = render_resource::instances;
        auto chain = std::make_shared<pass::pass_chain>();
        auto& provider = chain->emplace<pass::resource_storage_pass>();
        managed::request request;
        request.id = id;
        request.life = managed::lifetime::scene;
        request.layer_views = false;
        if (!desc.initial_bytes.empty()) {
            auto upload = std::make_shared<deren::utility::vessel::vector<std::byte> const>(desc.initial_bytes.begin(), desc.initial_bytes.end());
            request.initial_data = std::shared_ptr<std::byte const>{upload, upload->data()};
            request.initial_data_size = upload->size();
            request.initialization_key = std::make_shared<int const>(0);
        }
        desc.initial_bytes = {};
        request.image = desc;
        provider.configure(std::span{&request, 1});
        if (chain->prepare_resources(core_owner, {}) != rhi::error::ok) {
            return {};
        }
        auto generation = chain->resources().snapshot().find(id);
        auto* image = generation->objects[0].image.get();
        auto* view = generation->objects[0].array_view.get();
        request.initial_data.reset();
        request.image.debug_name = nullptr;
        provider.configure(std::span{&request, 1});
        return {rhi::shared_object<rhi::image>{chain, image}, rhi::shared_object<rhi::image_view>{chain, view}};
    }

    rhi::shared_object<rhi::sampler> runtime::create_shared_sampler(render_resource::resource_id id, rhi::sampler_desc const& desc) {
        namespace managed = render_resource::instances;
        auto chain = std::make_shared<pass::pass_chain>();
        auto& provider = chain->emplace<pass::resource_storage_pass>();
        managed::request request;
        request.id = id;
        request.type = managed::kind::sampler;
        request.life = managed::lifetime::device;
        request.sampler = desc;
        provider.configure(std::span{&request, 1});
        if (chain->prepare_resources(core_owner, {}) != rhi::error::ok) {
            return {};
        }
        auto generation = chain->resources().snapshot().find(id);
        return rhi::shared_object<rhi::sampler>{chain, generation->objects[0].sampler.get()};
    }

    rhi::shared_object<rhi::buffer> runtime::create_shared_upload(std::span<std::byte const> bytes, rhi::buffer_flags flags) {
        if (bytes.empty()) {
            return {};
        }
        namespace managed = render_resource::instances;
        auto chain = std::make_shared<pass::pass_chain>();
        auto& provider = chain->emplace<pass::resource_storage_pass>();
        auto upload = std::make_shared<deren::utility::vessel::vector<std::byte> const>(bytes.begin(), bytes.end());
        managed::request request;
        request.id = render_resource::resource_id::pass_upload_buffer;
        request.type = managed::kind::buffer;
        request.life = managed::lifetime::device;
        request.buffer = {.size = bytes.size(), .usage = rhi::buffer_usage::storage_coherent, .flags = flags};
        request.initial_data = std::shared_ptr<std::byte const>{upload, upload->data()};
        request.initial_data_size = upload->size();
        request.initialization_key = std::make_shared<int const>(0);
        provider.configure(std::span{&request, 1});
        if (chain->prepare_resources(core_owner, {}) != rhi::error::ok) {
            return {};
        }
        auto generation = chain->resources().snapshot().find(request.id);
        request.initial_data.reset();
        provider.configure(std::span{&request, 1});
        return rhi::shared_object<rhi::buffer>{chain, generation->objects[0].buffer.get()};
    }

    void runtime::init_scene_resources() {
        // Camera constants are prepared by the passchain alongside constructor output targets.
        // Their buffers retain uniform-coherent allocation and storage/device-address usage for the heap.

        // 1x1 white fallback texture, always the first entry of the scene texture array; missing
        // material textures point at it
        constexpr deren::utility::vessel::array<uint8_t, 4> white_pixels = {255, 255, 255, 255};
        rhi::image_desc white_desc{};
        white_desc.extent = rhi::image_extent{.width = 1u, .height = 1u, .depth = 1u};
        white_desc.format = rhi::image_format::rgba8_unorm;
        white_desc.flags = rhi::to_bits(rhi::image_flag::sampled);
        white_desc.initial_bytes = std::as_bytes(std::span(white_pixels));
        white_desc.debug_name = "white fallback texture";
        auto white = this->create_shared_image(render_resource::resource_id::white_texture, white_desc);
        this->owned_textures.push_back(std::move(white.first));
        this->owned_texture_views.push_back(std::move(white.second));
        if (!static_cast<bool>(this->owned_textures.back()) || !static_cast<bool>(this->owned_texture_views.back())) {
            deren::utility::panic("failed to create the white fallback texture");
        }
        // NO NATIVE VIEW CACHE (the `texture_array_views` vector that stood here is GONE): its elements were
        // never read - every use was `.size()`, which is `owned_texture_views.size()`, and the two pushes were
        // the only readers of the escape's `native_image_view` in this file. The heap write below is the
        // contract's own, so nothing ever needed the raw handle.
        this->white_texture_index = static_cast<uint32_t>(this->owned_texture_views.size() - 1);

        // THE WHITE ELEMENT NEEDS ITS OWN HEAP DESCRIPTOR HERE, and its absence was a class of black frames.
        // Every texture that reaches the bindless array through register_material has its heap slot written
        // there, but this one is created above that loop and never passes through it - so slot
        // heap_slots::textures + 0 stayed EMPTY and sampled as zero. A material slot with no texture arrives
        // here (Sponza's stone carries no occlusion map), so its ao read 0 - and shading.glsl multiplies BOTH
        // the diffuse ambient and the specular IBL by s.ao, which left those surfaces lit by the sun alone:
        // a black interior, while the metal test assets (whose materials do carry an occlusion map, and whose
        // diffuse term is multiplied away by (1 - metallic) anyway) looked untouched. The startup probe had
        // it in the log the whole time: "the heap-native probe sampled grid slot 16384 ... read back
        // 0x00000000", on the very slot this write fills, while the material table's white record read 0xffff.
        // The shader slot is a logical identity; descriptor_space resolves its physical storage.
        if (contract_descriptor_ready(this->rhi_face())) {
            // THE DESCRIPTOR IS THE CONTRACT'S (item d of the escape-shrink plan): the image AND its view are
            // the contract objects created six lines above (`owned_textures.back()` / `owned_texture_views.back()`),
            // so the descriptor is an `image_view_desc` - all remaining layers and mips, which is the view those
            // lines made. The hand-built `VkImageViewCreateInfo` and the `native_image` read that fed it are gone
            // with it: this was the last escape use in this function.
            rhi::image_view_desc const white_heap_view{};
            std::uint32_t const white_slot = deren::engine::render_layout::heap_slots::textures + this->white_texture_index;
            if (!contract_write_descriptor_image(this->rhi_face(), white_slot, *this->owned_textures.back(), white_heap_view, rhi::descriptor_type::sampled_image)) {
                deren::utility::log("descriptor heap: the white fallback texture did not reach grid slot {}", deren::engine::render_layout::heap_slots::textures + this->white_texture_index);
            }
        }

        // (The sampler the array entries are read through is NOT created here: the maxLod-12 REPEAT
        // sampler the comment that used to sit here described moved into the core, next to the other six
        // - see core::create_samplers / shared_samplers.) The white element above is the one entry this
        // function has to place, because every later texture index is assigned around it.

        // GPU material table: fixed capacity, host-visible (direct mapping); records are appended
        // at registration and read-only for the GPU (set 0 binding 5)

        // Allocation and zero initialization are provided by scene_tables_pass.

        // ---- THE MATERIAL TABLE'S HEAP DESCRIPTOR, written ONCE here ----
        //
        // A storage buffer descriptor is just an address range, so this is the simplest descriptor in the
        // renderer: no image view to create, no sampler, no embedded sampler. The buffer has a FIXED capacity and
        // is created above, so its address is stable and one write covers it - which is why this is not a
        // per-frame write. It goes at its OWN GRID SLOT (deren::engine::render_layout::heap_slots::materials), which is the same number a
        // heap-native shader bakes as `heap_slots_materials` (source/shaders/heap_slots.glsl): the write and the read are
        // the same number by construction rather than by review.
        if (contract_descriptor_ready(this->rhi_face())) {
            bool const written = this->write_heap_buffer(*this->material_buffer,
                                                         deren::engine::render_layout::heap_slots::materials,
                                                         static_cast<std::uint64_t>(deren::engine::material_capacity) * sizeof(material_record),
                                                         rhi::descriptor_type::storage_buffer);
            deren::utility::log("descriptor heap: material table {} ({} records, slot {})",
                                written ? "written" : "NOT written",
                                deren::engine::material_capacity,
                                deren::engine::render_layout::heap_slots::materials);
        }

        // ---- THE MATERIAL COLOURS: ONE `vec4` PER LANE PER MATERIAL, WHITE to begin with ----
        //
        // THE NEUTRAL IS THE INITIAL CONTENT and not a placeholder (see `toon_colour_lane`): every lane multiplies
        // or tints something, so a material whose sidecar states no colour must leave that thing alone - and
        // `register_material` overwrites the lanes it was given, per material, at the index the shader addresses.
        // THREE LANES ARE NOT WHITE, and all three are stated below: the outline lane's width (0, not 1) and the
        // TWO scalar lanes' sentinel (-1, not 1) - the specular strength and the parallax depth.
        //
        // A BUFFER OF FLOATS, so the fill below writes 1.0f as a FLOAT - not 0xFF, which is one in a UNORM texture
        // and 0.0 in this. That distinction is the whole reason a colour lane could not reuse the texture lanes.
        {
            deren::utility::vessel::vector<glm::vec4> neutral_colours(static_cast<size_t>(deren::engine::material_capacity) * static_cast<size_t>(deren::engine::toon_colour_lane::count), glm::vec4(1.0f));
            // ... EXCEPT THE OUTLINE LANE'S WIDTH, WHICH IS 0.0 AND NOT 1.0 (`toon_inputs::colours` states the
            // whole argument): three of that lane's four floats are the `_OutlineTintColor` tint, whose neutral is
            // white, and the fourth is `_OutlineWidth`, whose neutral is ZERO - a width of 1.0 would draw a hull
            // around every material that states no outline, i.e. the entire character. The layout is
            // MATERIAL-major (`material_index * toon_colour_lane::count + lane`, see `register_material`), so the
            // lane to reach is every `count`-th element.
            for (size_t material = 0; material < static_cast<size_t>(deren::engine::material_capacity); ++material) {
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::outline_edge)] =
                    glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
                // ... AND THE TWO SCALAR LANES' SENTINEL IS `-1.0` RATHER THAN 1.0, for the same kind of reason:
                // neither `.x` is a tint but a STRENGTH (`_Specular`) or a DEPTH (`_ParallaxScale`), whose "no
                // statement" cannot be a number in the value's own range (see `toon_colour_lane::specular_strength`
                // and `toon_colour_lane::parallax_scale`). A table left at 1.0 would shade every material that
                // states no `_Specular` with a full-strength highlight, and would push every material that states
                // no `_ParallaxScale` to a parallax offset thirty-three times the one the stage's own constant
                // gives - which is the same failure from the other side.
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::specular_strength)] =
                    glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::parallax_scale)] =
                    glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
                // ... AND THE GOO IRIS BRIGHTNESS LANE'S TWO COMPONENTS ARE BOTH SENTINELS, because both are
                // BRIGHTNESSES (`Eyes brightness` / `Eyes HightLight brightness`): `0.0` is a value the reference
                // really uses - it is what the group's own interface defaults the two sockets to - so it cannot
                // mean "absent", and anything else in range would be a brightness this port made up. The stage
                // tests each component separately and answers with that interface default (see
                // `toon_colour_lane::goo_eye_brightness`).
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::goo_eye_brightness)] =
                    glm::vec4(-1.0f, -1.0f, 0.0f, 0.0f);
                // ... AND THE TWO REWRITTEN CHAIN'S RIM LANES, whose neutrals are the REFERENCE'S OWN interface
                // defaults rather than one convention (see `toon_colour_lane::goo_rim_colour` /
                // `goo_rim_scalars`): the tint's neutral is the `Rim_Color` sub-group's own `[1,1,1,1]`, and the
                // four scalars are ALL sentinels because every one of them is a value the reference really uses
                // at zero - `Rim_ColorStrength = 0.0` is how its author switches a rim off
                // (`M_actor_laevat_cloth_03`), and `Use Rimlimitation?` is a BOOLEAN whose default is 0.0. The
                // stage resolves each of the four separately to that socket's group default (see the shader).
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::goo_rim_colour)] =
                    glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::goo_rim_scalars)] =
                    glm::vec4(-1.0f, -1.0f, -1.0f, -1.0f);
                // ... AND THE SCREEN-SPACE RIM'S TWO WIDTHS ARE BOTH SENTINELS TOO, because a width has no no-op
                // number either: `0.0` is a width the reference's author really states (it collapses the offset
                // sample onto the pixel and the depth difference is then exactly 0 - a rim that is off ON PURPOSE,
                // material by material), so it cannot mean "absent". The stage answers each component with the
                // `DepthRim` group's OWN interface default, `0.5` (see `toon_colour_lane::goo_rim_widths`, and
                // `gooblender/nodes.json`'s `meta.node_groups[DepthRim].interface[]`).
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::goo_rim_widths)] =
                    glm::vec4(-1.0f, -1.0f, -1.0f, -1.0f);
                // ... AND STEP 4'S SIX LANES, whose NEUTRALS ARE TWO DIFFERENT SHAPES FOR THE SAME REASON THE RIM
                // LANES ABOVE ALREADY GAVE: two of them ARE the reference's own interface defaults
                // (`BaseColor` = white, `directOcclusionColor` = black) and the other four are sentinels. THEIR
                // SENTINEL IS `-1000` AND NOT `-1`, because two of the eight per-material numbers behind them are
                // AUTHORED NEGATIVES - `CastShadow_center` is `-0.1` on both body materials and
                // `GlobalShadowBrightnessAdjustment` is `-1.8` on the cloth - and a `-1` neutral would make the
                // stage read those authored values as "not stated" (see `goo_lane_absent` in the shader).
                // The stage resolves each sentineled component to its group's default - see
                // `toon_colour_lane::goo_base_colour` .. `goo_direct_occlusion` and `source/shaders/toon/goo_toon.slang`.
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::goo_base_colour)] =
                    glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::goo_direct_occlusion)] =
                    glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                for (uint32_t lane : {static_cast<uint32_t>(deren::engine::toon_colour_lane::goo_diffuse_a),
                                      static_cast<uint32_t>(deren::engine::toon_colour_lane::goo_diffuse_b),
                                      static_cast<uint32_t>(deren::engine::toon_colour_lane::goo_fresnel_inside),
                                      static_cast<uint32_t>(deren::engine::toon_colour_lane::goo_fresnel_outside)}) {
                    neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + lane] =
                        glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f);
                }
                // ... AND STEP 5'S THREE LANES, whose neutrals are the reference's own `interface[]` defaults and
                // NOT the `-1000` sentinel the four above use, because none of these three numbers is ever negative
                // in the reference's asset: `specularFGD Strength` is `0.8` or `1.0`, `dirLight_lightColor` is
                // `(1, 0.958..., 0.958...)` and `AmbientLightColorTint` is white or `(1.512...)`. So the CHEAPER
                // `< 0` contract is enough here - and the reason step 4's four needed `-1000` is exactly that two
                // of THEIR eight numbers are authored negatives (`CastShadow_center` = `-0.1`,
                // `GlobalShadowBrightnessAdjustment` = `-1.8`). The scalar lane keeps its sentinel in `.x` only
                // (the stage resolves it to the reference's `1.0`); the two colour lanes are their own fallback.
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::goo_specular_fgd)] =
                    glm::vec4(-1.0f, 1.0f, 1.0f, 1.0f);
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::goo_light_color)] =
                    glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::goo_ambient_tint)] =
                    glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::goo_specular_color)] =
                    glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
                // ... AND STEP 7'S FOUR, WHOSE NEUTRALS ARE THREE DIFFERENT SHAPES AGAIN, each one the socket's own
                // `interface[]` default rather than a convention:
                //
                //   * THE TWO SCALAR LANES are `-1000` sentinels, like step 4's four and for the same reason: the
                //     chin pair, `sphereNormal_Strength` and the two brightnesses are all positive on every
                //     material this container is given, but `SmoothnessMax` is a value whose ZERO is meaningful
                //     (perfectly rough) - so a neutral of `0` would read "the material states no smoothness" as "the
                //     material is rough", and the stage would then have to invent one. `-1000` is outside every one
                //     of their domains, so a stated value - any sign - passes and only the lane itself fails.
                //   * `nose_shadow_Color` AND `Front R Color` ARE BLACK, because that is what the FACE container's
                //     own interface says (`::- ... :: 组输入.nose_shadow_Color = [0.0, 0.0, 0.0, 1.0]`,
                //     `Front R Color = [0.0, 0.0, 0.0, 1.0]`) and because WHITE WOULD BE A STATEMENT: the nose
                //     shadow is a MIX's A side (white = no shadow at all, the strongest possible statement) and
                //     `Front R`'s colour multiplies a term that is otherwise the albedo itself. A material that
                //     states neither row therefore gets the reference's own defaults, which is what a material
                //     calling the group without stating them gets in Goo.
                for (uint32_t lane : {static_cast<uint32_t>(deren::engine::toon_colour_lane::goo_face_scalars_a),
                                      static_cast<uint32_t>(deren::engine::toon_colour_lane::goo_face_scalars_b)}) {
                    neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + lane] =
                        glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f);
                }
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::goo_face_nose_shadow)] =
                    glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::goo_face_front_r)] =
                    glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                // STEP 8'S ONE LANE IS A `-1000` SENTINEL AND NOT THE REFERENCE'S GROUP DEFAULT `1.0`, because the
                // lane does not carry a tint or a factor that has a neutral - it carries the SWITCH that decides
                // whether the Goo chain's normal decode runs at all. A material with no row therefore keeps the
                // normal the chain had before this step, which is what `laevatain_no_sidecar` and the old chain's
                // frames are pinned on; `NormalStrength = 0` is a value the reference states on some materials, so
                // `0` could not be the "not stated" answer (see `goo_normal_strength`).
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::goo_normal_strength)] =
                    glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f);
                // STEP 10'S ONE IS `(0, 0, 0, 0)`, AND HERE - UNLIKE EVERY OTHER LANE ABOVE - THE NEUTRAL IS
                // DOING TWO JOBS AT ONCE. It IS the reference's own group default, so a material that states no
                // `_GooAnisoGate` row gets the answer the graph gives a caller that states nothing
                // (`ng[2].interface[3]` `Use anisotropy?` = `0.0`, `interface[45]` `Anisotropic mask` = `0.0`,
                // `interface[4]` `Use Toonaniso?` = `0.0`); AND it is the OVERRIDE that keeps that true, because
                // this table starts EVERY lane at `glm::vec4(1.0f)` (the constructor's first line), so an
                // unoverridden lane 25 would hand every such material `Use anisotropy? = 1.0` - the arm the
                // reference does not take - instead of the `0.0` it does. `.z` is recorded and unused, `.w`
                // reserved; keep the spelling of this initialiser and the two hosts' tables in step, because
                // `source/tests/test_goo_toon_math.cpp` pins all three.
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::goo_aniso_gate)] =
                    glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
                // STEP 12'S ONE IS `(0, 0, 0, 0)` TOO, AND THE TWO JOBS IT DOES ARE STEP 10'S OWN: it IS the
                // reference's group default (`ng[2].interface[20]` `Aniso_SmoothnessMaxT` = `0.0`,
                // `interface[21]` `Aniso_SmoothnessMaxB` = `0.0`, so a material with no `_GooAnisoRough` row gets
                // `rT = rB = 1` - the anisotropic lobe at full roughness, which is what the graph answers a
                // caller that states nothing); AND it is the override that keeps that true, because this table
                // starts EVERY lane at `glm::vec4(1.0f)`, so an unoverridden lane 26 would hand every rowless
                // material `rT = (1 - 1)^2 = 0` - a mirror - instead of the `0.0` the graph means. `.z` / `.w`
                // are reserved. Keep the spelling of this initialiser and the two hosts' tables in step, because
                // `source/tests/test_goo_toon_math.cpp` pins all three. See `toon_colour_lane::goo_aniso_rough`.
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::goo_aniso_rough)] =
                    glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
                // STEP 13'S TWO ARE `(0, 0, 0, 0)` AS WELL, and unlike the two above them the value is not the
                // reference's group default but the BRANCH'S OWN SWITCH: `_GooRSScalars.x` is `Use RS_Eff?`, so a
                // material whose sidecar states no `_GooRSScalars` row has to read `Use = 0` and the stage's gate
                // then leaves its colour untouched. Lane 28's `.w` is the mask's `SmoothStep.max`, where zero
                // means "unstated" rather than a zero-width window - the stage maps it to `1.0` so that this
                // neutral cannot divide by zero (see `toon_colour_lane::goo_rs_tint`, spec U7). Keep the spelling
                // of this initialiser and the two hosts' tables in step, because `source/tests/test_goo_toon_math.cpp`
                // pins all three.
                //
                // THEY ARE DEFENSIVE RATHER THAN LOAD-BEARING, AND THE COMMENT USED TO SAY OTHERWISE ELSEWHERE:
                // these are the BUFFER'S INITIAL CONTENT. `register_material` writes every one of the
                // `toon_colour_lane::count` lanes of every material it registers from `info.toon.colours` (the
                // loop that ends this table's use of `neutral_colours` - see the `colours[lane] =
                // info.toon.colours[lane]` loop in that function), and the host's `toon_colour_neutral` in
                // `main.cpp` is what answers a row that is absent. So this entry is not what makes the twenty
                // `Use RS_Eff? = 0` materials unchanged; it is the honest statement of the neutral plus a guard
                // against a future consumer that reads the buffer before any material is registered.
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::goo_rs_scalars)] =
                    glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::goo_rs_tint)] =
                    glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
                // STEP 15'S ONE IS `(0, 0, 0, 0)` FOR BOTH OF STEP 13'S REASONS, and both apply here verbatim: all
                // four `armA` sockets default to `0.0` in the reference's `interface[]` (so a material stating no
                // `_GooRSArm0` row reads what the graph gives a caller that states nothing), AND the value is the
                // arm's own off switch, because `.y` is `RS Strength` and `s023 = s036 * RS Strength` zeroes the
                // arm. It is ALSO the override that keeps the first statement true: this table starts every lane at
                // `glm::vec4(1.0f)`, so an unoverridden lane 29 would hand every rowless material `RS Strength = 1`
                // with `Layer weight Value = 1` - a `u` of `0.5` on sheet 0 for every material in the scene. Keep
                // the spelling of this initialiser and the two hosts' tables in step, because
                // `source/tests/test_goo_toon_math.cpp` pins all three. See `toon_colour_lane::goo_rs_arm0`.
                neutral_colours[material * static_cast<size_t>(deren::engine::toon_colour_lane::count) + static_cast<size_t>(deren::engine::toon_colour_lane::goo_rs_arm0)] =
                    glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
            }
            std::memcpy(this->toon_colour_mapped, neutral_colours.data(), std::as_bytes(std::span(neutral_colours)).size());
            if (contract_descriptor_ready(this->rhi_face())) {
                bool const written = this->write_heap_buffer(*this->toon_colour_buffer,
                                                             deren::engine::render_layout::heap_slots::toon_colours,
                                                             std::as_bytes(std::span(neutral_colours)).size(),
                                                             rhi::descriptor_type::storage_buffer);
                deren::utility::log("descriptor heap: toon colour table {} ({} lanes x {} materials, slot {})",
                                    written ? "written" : "NOT written",
                                    static_cast<uint32_t>(deren::engine::toon_colour_lane::count),
                                    deren::engine::material_capacity,
                                    deren::engine::render_layout::heap_slots::toon_colours);
            }
        }

        // ---- THE TOON LANES BESIDE THE RECORD: ONE `uvec4` PER MATERIAL, and a buffer of its own because the
        //      material record cannot hold them - see deren::engine::render_layout::heap_slots::toon_lanes for the measurement that
        //      settled that (the record is INLINE in the per-draw push block, and adding a word to it crashed the
        //      renderer).
        //
        //      x IS THE FACE SDF LANE (`_SDFLightmap`) and y IS THE METALLIC/GLOSS LANE (`_MetallicGlossMap`); z
        //      and w are reserved for the lanes the rest of the character work adds, which is why the block is a
        //      `uvec4` rather than the single uint the SDF lane needed on its own - one descriptor, four lanes.
        //
        //      IT IS MATERIAL-INDEXED, not lane-indexed like the record's `toon_indices`: the shader reaches it
        //      with the material index it already has, so nothing new has to be threaded through the push path.
        //      Zero means "do not read", which is the contract the record's four lanes use too.
        //
        //      WRITTEN ONCE, here, like the material table beside it: the values are fixed at import and never
        //      rewritten, which is why it is one descriptor and not a per-frame pair.

        // Allocation and zero initialization are provided by scene_tables_pass.
        if (contract_descriptor_ready(this->rhi_face())) {
            bool const written = this->write_heap_buffer(*this->toon_lane_buffer,
                                                         deren::engine::render_layout::heap_slots::toon_lanes,
                                                         static_cast<std::uint64_t>(deren::engine::material_capacity) * deren::engine::toon_lane_blocks * sizeof(glm::uvec4),
                                                         rhi::descriptor_type::storage_buffer);
            deren::utility::log("descriptor heap: toon lane table {} ({} lanes, slot {})",
                                written ? "written" : "NOT written",
                                deren::engine::material_capacity,
                                deren::engine::render_layout::heap_slots::toon_lanes);
        }

        // ---- THE TOON LIGHT RIG: ONE BLOCK FOR THE RUN, and it is not a per-frame pair for the reason the two
        //      tables above are not: nothing rewrites it while a frame is in flight (see
        //      deren::engine::render_layout::heap_slots::toon_rig). The application fills it from its config through
        //      `runtime::set_toon_rig`, and the zeros it is created with are replaced before the first frame.
        {

            // Allocation and zero initialization are provided by scene_tables_pass.
            if (contract_descriptor_ready(this->rhi_face())) {
                bool const written = this->write_heap_buffer(*this->toon_rig_buffer,
                                                             deren::engine::render_layout::heap_slots::toon_rig,
                                                             static_cast<std::uint64_t>(sizeof(deren::engine::toon_rig)),
                                                             rhi::descriptor_type::storage_buffer);
                deren::utility::log("descriptor heap: toon light rig {} ({} B, slot {})",
                                    written ? "written" : "NOT written",
                                    sizeof(deren::engine::toon_rig),
                                    deren::engine::render_layout::heap_slots::toon_rig);
            }
        }

        // ---- THE MESHLET TABLE (docs/mesh_shaders.md step 3): one `deren::engine::meshlet` record per meshlet - 48 bytes,
        //      the layout `static_assert`s in source/engine/meshlet/meshlet.cppm pin field by field, because a 28-byte
        //      host record against the shader's std430 stride WEDGED the GPU before it was found - appended by the
        //      primitive upload while the scene imports and read by a task stage through the heap. Created here,
        //      with the material table's shape and for its reasons: fixed capacity, host-visible (direct mapping),
        //      and a device address because a heap descriptor for a buffer IS an address range.
        {

            // Allocation and zero initialization are provided by scene_tables_pass.
            if (contract_descriptor_ready(this->rhi_face())) {
                bool const written = this->write_heap_buffer(*this->meshlet_buffer,
                                                             deren::engine::render_layout::heap_slots::meshlets,
                                                             static_cast<std::uint64_t>(deren::engine::meshlet_capacity) * sizeof(deren::engine::meshlet),
                                                             rhi::descriptor_type::storage_buffer);
                deren::utility::log("descriptor heap: meshlet table {} ({} records of {} B, slot {})",
                                    written ? "written" : "NOT written",
                                    deren::engine::meshlet_capacity,
                                    sizeof(deren::engine::meshlet),
                                    deren::engine::render_layout::heap_slots::meshlets);
            }
        }

        // ---- THE INDIRECT MESH COMMANDS (docs/mesh_shaders.md step 3, second mechanism): one
        //      `rhi::mesh_task_command` per (frame in flight, primitive), written by whichever thread records
        //      that primitive's meshlet dispatch and read by the GPU. THE SLOT IS THE PRIMITIVE'S OWN
        //      `meshlet_base`, which is what removes the cursor the first attempt used - and with it the
        //      flakiness that cursor caused (see the member's note in runtime.declarations.cppm). Not on the
        //      heap: it is command data, not a resource any shader reads.
        //
        //      THE RECORD AND ITS SIZE ARE THE CONTRACT'S (`rhi::mesh_task_command` /
        //      `mesh_task_command_size`): the engine WRITES these records and passes the size as the stride, so
        //      the layout has to be nameable by both halves - the backend static_asserts its own structure
        //      against it.
        {

            // Allocation and zero initialization are provided by scene_tables_pass.
            // THE NATIVE TABLE IS GONE: the dispatch takes the CONTRACT handle this class already owns
            // (`mesh_indirect_buffer`), so nothing here derives a `VkBuffer` for it any more - the derivation
            // was the reason a null handle could silently send every dispatch down the DIRECT path.
        }

        // ---- THE MESH CULLING COUNTERS (docs/mesh_shaders.md step 3, "what the culling buys"): one lane of sixteen
        //      uints per frame in flight, on the heap because a mesh stage has no other way to reach memory, and
        //      host-visible because the host reads it back once, at shutdown. Zeroed here; the entries only add.
        //      SIXTEEN and not eight since the visibility raster lane reports its own four (mesh_geometry.slang's ids
        //      8..11): the buffer has to hold every lane's counters or a lane's last frame slot writes past the end.
        //      The shader's `mesh_stat_stride` and this size are one contract, pinned only by this comment and the
        //      shutdown log's plausibility (a mismatch is an out-of-bounds atomic write, not a build error).
        {

            // Allocation and zero initialization are provided by scene_tables_pass - THE ALLOCATION SIZE IS THAT
            // PASS'S `descriptions()`, which is where the OTHER copy of this 16 lives; both have to move together.
            if (contract_descriptor_ready(this->rhi_face())) {
                bool const written = this->write_heap_buffer(*this->meshlet_stats_buffer,
                                                             deren::engine::render_layout::heap_slots::meshlet_stats,
                                                             static_cast<std::uint64_t>(this->frame_ring().slot_count()) * 16u * sizeof(uint32_t),
                                                             rhi::descriptor_type::storage_buffer);
                if (!written) {
                    deren::utility::log("descriptor heap: the mesh culling counters were NOT written - the counters stay zero");
                }
            }
        }

        // ---- THE HOST-CULLED MESHLET TABLE (docs/mesh_shaders.md step 3, the culling's cheapest stage): one lane of
        //      `meshlet_capacity` records per frame in flight, host-visible because the HOST writes it while it
        //      records and the heap-bound because the mesh entry reads it. Per frame rather than one slot, unlike the
        //      table it shadows: this one is rewritten from the camera every frame.
        {

            // Allocation and zero initialization are provided by scene_tables_pass.
            if (contract_descriptor_ready(this->rhi_face())) {
                bool const written = this->write_heap_buffer(*this->meshlet_culled_buffer,
                                                             deren::engine::render_layout::heap_slots::meshlet_culled,
                                                             static_cast<std::uint64_t>(this->frame_ring().slot_count()) * deren::engine::meshlet_capacity * sizeof(deren::engine::meshlet),
                                                             rhi::descriptor_type::storage_buffer);
                if (!written) {
                    deren::utility::log("descriptor heap: the culled meshlet table was NOT written - the draws stay unculled");
                }
            }
        }

        // ---- THE VISIBILITY ITEM TABLE (visibility-buffer v0.2b): one lane of id_item_capacity draw_item records
        //      per frame in flight, host-visible and heap-bound for the same two reasons the culled table above is:
        //      the host writes it while it records, and the resolve compute pass has no other route to it than the
        //      heap. NOTHING WRITES IT YET (the raster lane is V0.3), which is why this changes no picture. ----
        {

            // Allocation and zero initialization are provided by scene_tables_pass.
            if (contract_descriptor_ready(this->rhi_face())) {
                bool const written = this->write_heap_buffer(*this->visibility_items_buffer,
                                                             deren::engine::render_layout::heap_slots::visibility_items,
                                                             static_cast<std::uint64_t>(this->frame_ring().slot_count()) *
                                                                 static_cast<std::uint64_t>(runtime::visibility_item_capacity) * sizeof(deren::engine::visibility::draw_item),
                                                             rhi::descriptor_type::storage_buffer);
                if (!written) {
                    deren::utility::log("descriptor heap: the visibility item table was NOT written - the visibility path cannot bind it");
                }
            }
        }

        // Per-instance transform buffer (set 0 binding 6): one mat4 per instance, host-visible;
        // filled by set_instanced_draw() for instanced stress draws (see pbr.vert)

        // Allocation and zero initialization are provided by scene_tables_pass.

        // The chain prepares deformation storage before setup writes. Keep chronological CPU history here.
        this->motion_previous.assign(deren::engine::scene_motion_capacity, glm::mat4(1.0f));
        this->skin_current.resize(this->frame_ring().slot_count());
        this->skin_previous.clear();
        this->skin_history_started = false;

        // Reserve table index 0 as the DEFAULT material (white textures + identity factors):
        // registrations that overflow the table degrade to it (see register_material). Done
        // FIRST so it always lands at index 0 - the raw zeroed record at 0 would render black
        // (all factors zero), not white. Safe here: the texture it uploads is registered on the
        // heap as it arrives, exactly as a later material's is.
        {
            primitive_create_info const default_material = {};
            material_id const default_index = this->register_material(default_material);
            if (default_index.value != 0) {
                deren::utility::panic("default material must occupy table index 0");
            }
        }
    }

    // The recording resources: one primary command buffer per frame slot, plus the secondary buffers
    // of the two parallel recording stages and the pools that have to own them. They are pre-allocated
    // so the GPU can read a secondary while this slot's primary executes, and reused every frame, so
    // they must exist before the first recorded frame - but nothing else in the constructor depends on
    // them, and they depend on nothing else but the two capacities below (hence a function of their
    // own). This is also where the command pools would move to the core (the "command pools and
    // secondaries" item of the trim list): the SHAPE is policy and stays
    // here, the objects are device resources.
    void runtime::init_shared_samplers() {
        // ---- THE SIX SAMPLERS THE RENDERER DECLARES BY HINT (③-D/E item C, abi 16) --------------------
        // Each description below is the VALUE-FOR-VALUE equivalent of the sampler `core::create_samplers()`
        // used to build and lend out (its own comments are quoted where the choice needs a reason), and that
        // equality is what keeps the captures byte-identical: `create_sampler()` rides the same
        // `make_texture_sampler_info()` the backend's six rode, and overrides exactly these fields.
        //
        // THE ORDER IS THE SAMPLER GRID'S ORDER and it is not decoration: `render_resource::shared::sampler_set`
        // is filled from these six, and source/shaders/heap_slots.glsl names the heap grid's sampler slots in the
        // order `shared_sampler_infos` writes them. `test_render_resources` holds that order against the
        // shader's names; the two sets are separate objects but the same six descriptions.
        // The bindless texture array: REPEAT, all its mip levels (12 is the capacity's own limit).
        rhi::sampler_desc texture_desc = {};
        texture_desc.address_mode = rhi::sampler_address_mode::repeat;
        texture_desc.max_lod = 12.0f;
        this->texture_sampler = create_shared_sampler(render_resource::resource_id::texture_sampler, texture_desc);

        // The G-buffer's stored surface: NEAREST, clamp. An interpolated normal or a filterable material id
        // is a different surface, not a smoother one.
        rhi::sampler_desc gbuffer_desc = {};
        gbuffer_desc.address_mode = rhi::sampler_address_mode::clamp_to_edge;
        gbuffer_desc.max_lod = 0.0f;
        gbuffer_desc.mag_filter = rhi::sampler_filter::nearest;
        gbuffer_desc.min_filter = rhi::sampler_filter::nearest;
        this->gbuffer_sampler = create_shared_sampler(render_resource::resource_id::gbuffer_sampler, gbuffer_desc);

        // The TAA resolve upsamples the scene colour but must NOT average neighbouring history texels:
        // linear magnification, nearest minification.
        rhi::sampler_desc taa_desc = gbuffer_desc;
        taa_desc.mag_filter = rhi::sampler_filter::linear;
        this->taa_sampler = create_shared_sampler(render_resource::resource_id::taa_sampler, taa_desc);

        // The post chain and the FXAA filter: LINEAR, clamp, one mip.
        rhi::sampler_desc post_desc = {};
        post_desc.address_mode = rhi::sampler_address_mode::clamp_to_edge;
        post_desc.max_lod = 1.0f;
        this->post_sampler = create_shared_sampler(render_resource::resource_id::post_sampler, post_desc);

        // The composite's GI upsample taps depth and normals AT CENTRES: an averaged depth invents a surface
        // between two samples, which is exactly what an edge-aware test must not see.
        this->post_nearest_sampler = create_shared_sampler(render_resource::resource_id::post_nearest_sampler, gbuffer_desc);

        // The cascaded shadow map: a depth comparison with LINEAR filtering, which is the hardware PCF the
        // shader's `sampler2DShadow` tap expects (compareOp matches pbr.frag's "not deeper than stored depth").
        rhi::sampler_desc shadow_desc = {};
        shadow_desc.address_mode = rhi::sampler_address_mode::clamp_to_edge;
        shadow_desc.max_lod = 0.0f;
        shadow_desc.mipmap_mode = rhi::sampler_mipmap_mode::nearest;
        shadow_desc.compare_enable = true;
        shadow_desc.compare_op = rhi::sampler_compare_op::less_or_equal;
        this->shadow_sampler = create_shared_sampler(render_resource::resource_id::shadow_sampler, shadow_desc);
    }

    void runtime::init_recording_resources() {
        // THE FRAME'S PRIMARY COMMAND BUFFERS STAY THE BACKEND'S (abi 15): core allocates one per frame
        // slot in its own constructor, `begin_commands()` hands out that slot's borrowed recording view,
        // and this runtime takes the raw handle it begins, ends and submits from the contract handle
        // `frame_command_buffer()` answers (see `native_frame_commands()`). Nothing of that is held here any
        // more - the span that used to alias the core's container is gone with the type it named.
        //
        // One secondary command buffer per frame slot (stage 2/3 of parallel recording),
        // pre-allocated with the primaries so the GPU can read them while this slot's primary
        // executes. Stage 3 additionally gives the main pass one parallel segment per task-pool worker.
        // EVERY ONE OF THEM IS A CALLER-OWNED `rhi::shared_object<rhi::command_buffer>` (abi 21), MADE BY
        // `api_core::make_command_buffer`: the control block's deleter IS the contract's one drop
        // (`release()`), so no call site here spells it and no second lifetime rule exists. Each buffer
        // also owns its own command pool (handles/handles.cppm): a VkCommandPool is not thread safe, so the
        // workers must never begin buffers of a shared pool concurrently - which is exactly why this
        // factory (one pool per buffer) is the safe spelling rather than a cost.
        //
        // THE FACTORY IS CALLED THROUGH THE CONTRACT INTERFACE, and that is a boundary rule rather than a
        // preference: `rhi_face()` is the `rhi::api_core&` the whole engine speaks, so this emits no backend
        // symbol (a call on the concrete `core` would join the boundary worklist). `vulkan_core` IS that
        // object, but naming it as the interface is what keeps the call site inside the rule.
        rhi::api_core& face = this->rhi_face();
        this->secondary_command_buffers.reserve(this->frame_ring().slot_count());
        this->main_segments.reserve(this->frame_ring().slot_count());
        uint32_t const record_workers = static_cast<uint32_t>(std::max(1, this->task_pool_threads()));
        for (int32_t slot = 0; slot < static_cast<int32_t>(this->frame_ring().slot_count()); ++slot) {
            // one entry: the alpha-blended pass's secondary (see secondary_pass). The shadow cascades
            // and the main-pass segments own their buffers elsewhere, because they record concurrently.
            deren::utility::vessel::array<rhi::mergeable_command_buffer_ref, static_cast<std::size_t>(secondary_pass::count)> pair = {
                face.commands->make_mergeable_command_buffer({}), // transparent
            };
            this->secondary_command_buffers.push_back(std::move(pair));

            // Shadow cascades record on the task pool, so each cascade gets its OWN buffer - and its
            // own pool with it (M9).
            deren::utility::vessel::vector<rhi::mergeable_command_buffer_ref> cascade_recording;
            cascade_recording.reserve(deren::engine::max_shadow_cascades);
            for (uint32_t cascade = 0; cascade < deren::engine::max_shadow_cascades; ++cascade) {
                cascade_recording.push_back(face.commands->make_mergeable_command_buffer({}));
            }
            this->shadow_recording.push_back(std::move(cascade_recording));
            deren::utility::vessel::vector<rhi::mergeable_command_buffer_ref> segments;
            segments.reserve(record_workers);
            for (uint32_t s = 0; s < record_workers; ++s) {
                segments.push_back(face.commands->make_mergeable_command_buffer({})); // one per worker
            }
            this->main_segments.push_back(std::move(segments));
        }
    }

    // The per-image state a swapchain GENERATION starts from. A freshly created target image is in
    // UNDEFINED, holds nothing, and no pass has written it: that is equally true of generation 0 (this
    // constructor, after the core built the generation's targets) and of every later generation
    // (on_swapchain_recreated), so both call THIS and the two can no longer drift.
    //
    // NOT here, deliberately: the TAA history matrices (image_view_proj). They may only be written from
    // a real camera snapshot (current_ubo), which does not exist yet in the constructor - they stay with
    // the two call sites that have one (on_swapchain_recreated, and set_taa's off -> on edge).
    void runtime::reset_image_generation_state() {
        this->bloom_graph_pending = false;
        // The G-buffer depth layout flags are one per swapchain image, and a freshly created depth image
        // is in UNDEFINED (which is what a clear flag says); the graph declares the transitions that used to be tracked.
        // THE GENERATION'S COUNT COMES FROM THE SWAPCHAIN (③-D/E A1.4): the per-image groups are engine-held
        // fixed-size arrays now, so their `size()` is the contract's bound rather than the number of images
        // this generation actually has - `swap_chain_images` is the one that still answers that.
        std::size_t const image_count = static_cast<std::size_t>(rhi::max_swapchain_images);
        // The motion-vector images died with the generation as well: clear the layout flag so the first
        // frame of the new generation takes the attachment -> sampled transition (see
        // ensure_velocity_sampled).
        this->rt_binding_written.assign(image_count, nullptr);
        this->taa_history_initialized.assign(image_count, false);
        this->inactive_descriptor_images_seeded = false;
        // a generation has nothing to blend with, and it is reset HERE rather than only on the off -> on
        // vector reads as "no history" for every frame, which silently turns the temporal resolve into a
        // pass-through of the raw trace).
        // ... and its FRAME COUNT restarts with it: the cold-start widening is measured in frames since the
        // accumulation restarted, and a new generation IS that restart (see frame_facts::gi_cold_start).
        // ... and the furnace cube is a new image too, so its level has to be written again.
        this->furnace_cube_ready = false;
        // ... AND THE SPLAT SNAPSHOTS GO WITH THE GENERATION. This is the step NVIDIA's own renderer takes in its
        // onResize() - a resize there is an explicit re-initialisation of everything that names a target
        // (m_gBuffers.update, every dependent system re-pointed at the NEW depth/colour images, the descriptor sets
        // re-written, and resetFrameCounter()) - and it is the one structural difference the comparison left
        // standing: this viewer's snapshots carry device addresses into buffers and belong to a frame, and a
        // generation has neither. Holding the previous generation's answers across the boundary is what the
        // known-issue section of docs/gaussian_splatting.md was chasing.
        this->gaussian_frames = {};
    }

    // ---- THE ENGINE'S OWN RENDER-CHAIN TARGETS (③-D/E A1) ----------------------------------------------
    //
    // The creation run the member notes in :declarations describe. ONE function because two paths need it
    // (the constructor for generation 0, `on_swapchain_recreated` for every later one), and two copies of a
    // creation loop is how two generations drift - the same argument `reset_image_generation_state` makes.
    void runtime::create_render_chain_targets() {
        // Constructor setup creates outputs only; configuration-dependent scene resources stay lazy.
        if (this->prepare_managed_resources(this->light_buffers.empty()) != rhi::error::ok) {
            deren::utility::panic("failed to prepare output targets");
        }

        // All scene/post/ray targets are now passchain-owned. Only the furnace environment
        // remains in the legacy creation path; its contents are reset with the generation.
        this->furnace_cube_view = {};
        this->furnace_cube_image = {};

        // ---- THE FURNACE MODE'S CONSTANT ENVIRONMENT (③-D/E A1.6) --------------------------------------
        // ONE 1x1x6 cube, device-wide: TRANSFER_DST because a clear is what gives it contents, SAMPLED because
        // the IBL bindings point at it, and `cube_compatible` with six layers - which is ALL the contract has
        // to say for the backend to derive a CUBE view from the range (the derivation lives in the heap path,
        // so this group needs no new vocabulary). No heap descriptor: what the furnace mode (and the empty-IBL
        // placeholder) needs is the resource table's view.
        rhi::image_desc furnace_desc{};
        furnace_desc.extent = rhi::image_extent{.width = 1u, .height = 1u, .depth = 1u};
        furnace_desc.mip_levels = 1;
        furnace_desc.array_layers = 6;
        furnace_desc.format = deren::engine::render_layout::hdr_format;
        furnace_desc.flags = rhi::to_bits(rhi::image_flag::sampled) | rhi::to_bits(rhi::image_flag::transfer_destination) | rhi::to_bits(rhi::image_flag::cube_compatible);
        furnace_desc.debug_name = "furnace environment cube";
        auto furnace = create_shared_image(render_resource::resource_id::furnace_cube, furnace_desc);
        this->furnace_cube_image = std::move(furnace.first);
        this->furnace_cube_view = std::move(furnace.second);
        if (!this->furnace_cube_image || !this->furnace_cube_view) {
            deren::utility::panic("failed to create the furnace environment cube");
        }
    }

    rhi::error runtime::prepare_managed_resources(bool output_only) {
        namespace managed = render_resource::instances;
        this->shadow_cascades = std::clamp(this->shadow_cascades, 1u, deren::engine::max_shadow_cascades);
        bool const software_requested = this->software_raster_on && this->visibility_buffer_on && this->subtilis_on;
        if (software_requested && !this->software_raster_pipeline_failed) {
            this->ensure_software_raster_passes();
        }
        bool const software_resources_wanted = software_requested && this->software_visibility && this->software_visibility->ready() && this->software_merge &&
                                               this->software_merge->ready();
        if (this->output_targets_provider != nullptr) {
            this->output_targets_provider->select_software_resources(software_resources_wanted);
        }
        managed::resource_context const context{
            .frame_slots = this->frame_ring().slot_count(),
            .output_images = rhi::max_swapchain_images,
            .shadow_map_size = this->shadow_map_size,
            .shadow_layers = this->shadow_cascades,
            .render_extent = this->render_extent(),
        };
        using render_resource::resource_id;
        managed::resource_snapshot primary;
        if (this->frame_chain != nullptr) {
            auto const prepared = this->frame_chain->prepare_resources(this->core_owner, context);
            if (prepared != rhi::error::ok) {
                return prepared;
            }
            primary = this->frame_chain->resources().snapshot();
        }
        bool const missing_shadow = !primary.find(resource_id::shadow_map);
        bool const missing_light = !primary.find(resource_id::light_ubo);
        bool const missing_head = !primary.find(resource_id::head_ubo);
        bool const missing_camera = !primary.find(resource_id::camera_ubo);
        bool const missing_counts = !primary.find(resource_id::cluster_counts);
        bool const missing_indices = !primary.find(resource_id::cluster_indices);
        if (missing_counts != missing_indices) {
            return rhi::error::invalid_argument;
        }
        auto& fallback = this->shared_resource_fallback;
        fallback.clear();
        deren::utility::vessel::array<bool, pass::scene_tables_pass::resource_count> missing_tables;
        for (size_t i = 0; i < missing_tables.size(); ++i) {
            missing_tables[i] = !primary.find(pass::scene_tables_pass::ids[i]);
        }
        if (std::any_of(missing_tables.begin(), missing_tables.end(), [](bool missing) { return missing; })) {
            if (this->fallback_tables_provider == nullptr) {
                this->fallback_tables_provider = &fallback.emplace<pass::scene_tables_pass>();
            } else {
                fallback.add(*this->fallback_tables_provider);
            }
            this->fallback_tables_provider->select_resources(missing_tables);
        } else if (this->fallback_tables_provider != nullptr) {
            this->fallback_tables_provider->bind_resources({});
        }
        deren::utility::vessel::array<bool, 4> missing_deformation;
        for (size_t i = 0; i < missing_deformation.size(); ++i) {
            missing_deformation[i] = !primary.find(pass::scene_deformation_pass::ids[i]);
        }
        if (std::any_of(missing_deformation.begin(), missing_deformation.end(), [](bool missing) { return missing; })) {
            if (this->fallback_deformation_provider == nullptr) {
                this->fallback_deformation_provider = &fallback.emplace<pass::scene_deformation_pass>();
            } else {
                fallback.add(*this->fallback_deformation_provider);
            }
            this->fallback_deformation_provider->select_resources(missing_deformation);
        } else if (this->fallback_deformation_provider != nullptr) {
            this->fallback_deformation_provider->bind_resources({});
        }
        if (!output_only) {
            if (missing_shadow) {
                if (this->fallback_shadow_provider == nullptr) {
                    this->fallback_shadow_provider = &fallback.emplace<pass::shadow_pass>();
                } else {
                    fallback.add(*this->fallback_shadow_provider);
                }
            } else if (this->fallback_shadow_provider != nullptr) {
                this->fallback_shadow_provider->bind_resources({});
            }
            if (missing_counts) {
                if (this->fallback_cluster_provider == nullptr) {
                    this->fallback_cluster_provider = &fallback.emplace<pass::cluster_pass>();
                } else {
                    fallback.add(*this->fallback_cluster_provider);
                }
            } else if (this->fallback_cluster_provider != nullptr) {
                this->fallback_cluster_provider->bind_resources({});
            }
        }
        // Camera is also needed by setup-time scene heap publication; the other constants stay lazy.
        if (missing_camera || (!output_only && (missing_light || missing_head))) {
            if (this->fallback_constants_provider == nullptr) {
                this->fallback_constants_provider = &fallback.emplace<pass::scene_constants_pass>();
            } else {
                fallback.add(*this->fallback_constants_provider);
            }
            this->fallback_constants_provider->select_resources(!output_only && missing_light, !output_only && missing_head, missing_camera);
        } else if (this->fallback_constants_provider != nullptr) {
            this->fallback_constants_provider->bind_resources({});
        }
        deren::utility::vessel::array<bool, pass::output_targets_pass::resource_count> missing_output;
        for (size_t i = 0; i < missing_output.size(); ++i) {
            missing_output[i] = (i < 15u || software_resources_wanted) && !primary.find(pass::output_targets_pass::ids[i]);
        }
        if (std::any_of(missing_output.begin(), missing_output.end(), [](bool missing) { return missing; })) {
            if (this->fallback_output_provider == nullptr) {
                this->fallback_output_provider = &fallback.emplace<pass::output_targets_pass>();
            } else {
                fallback.add(*this->fallback_output_provider);
            }
            this->fallback_output_provider->select_resources(missing_output);
        } else if (this->fallback_output_provider != nullptr) {
            this->fallback_output_provider->bind_resources({});
        }
        auto const prepared = fallback.prepare_resources(this->core_owner, context);
        if (prepared != rhi::error::ok) {
            return prepared;
        }
        auto const fallback_view = fallback.resources().snapshot();
        auto const shadow_view = missing_shadow ? fallback_view : primary;
        auto const cluster_view = missing_counts ? fallback_view : primary;
        auto const light_view = missing_light ? fallback_view : primary;
        auto const head_view = missing_head ? fallback_view : primary;
        auto const camera_view = missing_camera ? fallback_view : primary;
        auto const camera = camera_view.find(resource_id::camera_ubo);
        auto const light = light_view.find(resource_id::light_ubo);
        auto const head = head_view.find(resource_id::head_ubo);
        auto const shadow = shadow_view.find(resource_id::shadow_map);
        auto const counts = cluster_view.find(resource_id::cluster_counts);
        auto const indices = cluster_view.find(resource_id::cluster_indices);
        uint64_t const count_bytes = uint64_t(deren::engine::max_cluster_count) * sizeof(uint32_t);
        auto const valid_buffer = [&](std::shared_ptr<managed::generation const> const& value, uint64_t bytes) {
            return value && value->description.type == managed::kind::buffer &&
                   value->description.scope == render_resource::resource_scope::per_frame_slot &&
                   value->objects.size() == context.frame_slots && value->description.buffer.size >= bytes &&
                   value->description.buffer.usage == rhi::buffer_usage::storage_coherent &&
                   (value->description.buffer.flags & rhi::to_bits(rhi::buffer_flag::device_address)) != 0 &&
                   value->description.zero_on_create &&
                   std::all_of(value->objects.begin(), value->objects.end(), [bytes](auto const& object) { return object.buffer->mapped().size() >= bytes; });
        };
        auto const valid_constant = [&](std::shared_ptr<managed::generation const> const& value, uint64_t bytes) {
            return value && value->description.type == managed::kind::buffer &&
                   value->description.scope == render_resource::resource_scope::per_frame_slot && value->objects.size() == context.frame_slots &&
                   value->description.buffer.size >= bytes && !value->description.zero_on_frame &&
                   ((value->description.buffer.usage == rhi::buffer_usage::uniform_coherent &&
                     (value->description.buffer.flags & rhi::to_bits(rhi::buffer_flag::storage)) != 0) ||
                    value->description.buffer.usage == rhi::buffer_usage::storage_coherent) &&
                   (value->description.buffer.flags & rhi::to_bits(rhi::buffer_flag::device_address)) != 0 &&
                   std::all_of(value->objects.begin(), value->objects.end(), [bytes](auto const& object) { return object.buffer->mapped().size() >= bytes; });
        };
        if (!valid_constant(camera, sizeof(camera_ubo))) {
            return rhi::error::invalid_argument;
        }
        deren::utility::vessel::array<managed::resource_snapshot, pass::scene_tables_pass::resource_count> table_views;
        deren::utility::vessel::array<std::shared_ptr<managed::generation const>, pass::scene_tables_pass::resource_count> tables;
        auto const required_tables = pass::scene_tables_pass::descriptions(context);
        bool tables_changed = false;
        for (size_t i = 0; i < tables.size(); ++i) {
            auto const& expected = required_tables[i];
            table_views[i] = missing_tables[i] ? fallback_view : primary;
            auto const value = table_views[i].find(expected.id);
            auto const& desc = value->description;
            if (desc.type != managed::kind::buffer || desc.scope != expected.scope || desc.life != expected.life ||
                value->objects.size() != 1 || desc.buffer.size < expected.buffer.size || desc.buffer.usage != expected.buffer.usage ||
                (desc.buffer.flags & expected.buffer.flags) != expected.buffer.flags || !desc.zero_on_create || desc.zero_on_frame ||
                value->objects[0].buffer->mapped().size() < expected.buffer.size) {
                return rhi::error::invalid_argument;
            }
            tables[i] = value;
            tables_changed |= this->scene_table_resource_views[i].find(expected.id) != value;
        }
        deren::utility::vessel::array<managed::resource_snapshot, 4> deformation_views;
        deren::utility::vessel::array<std::shared_ptr<managed::generation const>, 4> deformations;
        deren::utility::vessel::array<bool, 4> deformation_changed;
        for (size_t i = 0; i < deformations.size(); ++i) {
            auto const id = pass::scene_deformation_pass::ids[i];
            deformation_views[i] = missing_deformation[i] ? fallback_view : primary;
            deformations[i] = deformation_views[i].find(id);
            if (!valid_buffer(deformations[i], pass::scene_deformation_pass::sizes[i]) || deformations[i]->description.zero_on_frame ||
                deformations[i]->description.life != managed::lifetime::frame_slot) {
                return rhi::error::invalid_argument;
            }
            deformation_changed[i] = this->deformation_resource_views[i].find(id) != deformations[i];
        }
        if (!output_only && (!valid_constant(light, sizeof(light_ubo)) || !valid_constant(head, sizeof(head_ubo)) ||
                             !valid_buffer(counts, count_bytes) || !counts->description.zero_on_frame ||
                             !valid_buffer(indices, count_bytes * deren::engine::cluster_light_capacity) ||
                             shadow->description.type != managed::kind::image || shadow->objects.size() != context.frame_slots ||
                             shadow->description.image.extent.width != context.shadow_map_size ||
                             shadow->description.image.extent.height != context.shadow_map_size ||
                             shadow->description.image.array_layers < context.shadow_layers)) {
            return rhi::error::invalid_argument;
        }
        deren::utility::vessel::array<managed::resource_snapshot, pass::output_targets_pass::resource_count> output_views;
        deren::utility::vessel::array<std::shared_ptr<managed::generation const>, pass::output_targets_pass::resource_count> outputs;
        auto const required_outputs = pass::output_targets_pass::descriptions(context);
        bool outputs_changed = false;
        for (size_t i = 0; i < outputs.size(); ++i) {
            if (i >= 15u && !software_resources_wanted) {
                continue;
            }
            auto const& expected = required_outputs[i];
            output_views[i] = missing_output[i] ? fallback_view : primary;
            auto const value = output_views[i].find(expected.id);
            auto const& desc = value->description;
            if (desc.type != managed::kind::image || desc.scope != expected.scope ||
                desc.life != expected.life || value->objects.size() != (expected.scope == render_resource::resource_scope::per_frame_slot ? context.frame_slots : context.output_images) ||
                desc.image.extent.width != expected.image.extent.width || desc.image.extent.height != expected.image.extent.height ||
                desc.image.extent.depth != 1 || desc.image.format != expected.image.format ||
                desc.image.array_layers != 1 || desc.image.mip_levels != 1 ||
                (desc.image.flags & expected.image.flags) != expected.image.flags) {
                return rhi::error::invalid_argument;
            }
            outputs[i] = value;
            outputs_changed |= this->output_resource_views[i].find(expected.id) != value;
        }
        auto software_tiles_view = software_resources_wanted && primary.find(resource_id::software_tiles) ? primary : fallback_view;
        auto software_tiles_generation = software_resources_wanted ? software_tiles_view.find(resource_id::software_tiles) : nullptr;
        if (software_resources_wanted) {
            std::uint64_t const expected_tile_bytes = render_resource::software_tiles_buffer_bytes(context.render_extent.width, context.render_extent.height);
            if (!software_tiles_generation || software_tiles_generation->description.type != managed::kind::buffer ||
                software_tiles_generation->description.scope != render_resource::resource_scope::device_wide ||
                software_tiles_generation->description.life != managed::lifetime::frame_slot ||
                software_tiles_generation->objects.size() != 1u || software_tiles_generation->description.buffer.size < expected_tile_bytes * context.frame_slots ||
                software_tiles_generation->description.buffer.usage != rhi::buffer_usage::storage_coherent ||
                software_tiles_generation->objects[0].buffer->mapped().size() < expected_tile_bytes * context.frame_slots) {
                return rhi::error::invalid_argument;
            }
        }
        bool const shadow_changed = !output_only && this->shadow_resource_view.find(resource_id::shadow_map) != shadow;
        bool const cluster_changed = !output_only && (this->cluster_resource_view.find(resource_id::cluster_counts) != counts ||
                                                      this->cluster_resource_view.find(resource_id::cluster_indices) != indices);
        bool const light_changed = !output_only && this->light_resource_view.find(resource_id::light_ubo) != light;
        bool const head_changed = !output_only && this->head_resource_view.find(resource_id::head_ubo) != head;
        bool const camera_changed = this->camera_resource_view.find(resource_id::camera_ubo) != camera;
        if ((shadow_changed && !this->shadow_images.empty()) || (cluster_changed && !this->cluster_count_buffers.empty()) ||
            (light_changed && !this->light_buffers.empty()) || (head_changed && !this->head_buffers.empty()) ||
            (camera_changed && !this->camera_buffers.empty()) || (tables_changed && static_cast<bool>(this->material_buffer)) ||
            (std::any_of(deformation_changed.begin(), deformation_changed.end(), [](bool changed) { return changed; }) && !this->motion_buffers.empty()) ||
            (outputs_changed && static_cast<bool>(this->hdr_images[0]))) {
            this->rhi_face().wait_idle();
        }
        if (tables_changed) {
            deren::utility::vessel::array<decltype(&this->material_buffer), 10> const handles{&this->material_buffer, &this->instance_buffer, &this->toon_colour_buffer, &this->toon_lane_buffer, &this->toon_rig_buffer,
                                                                                              &this->meshlet_buffer, &this->mesh_indirect_buffer, &this->meshlet_stats_buffer, &this->meshlet_culled_buffer,
                                                                                              &this->visibility_items_buffer};
            deren::utility::vessel::array<decltype(&this->material_mapped), 10> const mappings{&this->material_mapped, &this->instance_mapped, &this->toon_colour_mapped, &this->toon_lane_mapped, &this->toon_rig_mapped,
                                                                                               &this->meshlet_mapped, &this->mesh_indirect_mapped, &this->meshlet_stats_mapped, &this->meshlet_culled_mapped,
                                                                                               &this->visibility_items_mapped};
            for (size_t i = 0; i < tables.size(); ++i) {
                if (this->scene_table_resource_views[i].find(required_tables[i].id) == tables[i]) {
                    continue;
                }
                auto const target = tables[i]->objects[0].buffer->mapped();
                if (*handles[i]) {
                    auto const source = handles[i]->get()->mapped();
                    std::memcpy(target.data(), source.data(), std::min(source.size(), target.size()));
                }
                *handles[i] = {tables[i]->objects[0].buffer.get()};
                *mappings[i] = target.data();
            }
            this->publish_scene_table_bindings();
        }
        auto const adopt_deformation = [&](size_t family, auto& buffers, auto& mapped) {
            if (!deformation_changed[family]) {
                return;
            }
            auto const& generation = deformations[family];
            // Preserve setup deltas, partial uploads and history. Old snapshots stay alive through this copy.
            for (size_t slot = 0; slot < std::min(buffers.size(), generation->objects.size()); ++slot) {
                auto const source = buffers[slot].get()->mapped();
                auto const target = generation->objects[slot].buffer->mapped();
                std::memcpy(target.data(), source.data(), std::min(source.size(), target.size()));
            }
            buffers.clear();
            mapped.clear();
            for (auto const& object : generation->objects) {
                buffers.push_back({object.buffer.get()});
                mapped.push_back(object.buffer->mapped().data());
            }
        };
        adopt_deformation(0, this->motion_buffers, this->motion_mapped);
        adopt_deformation(1, this->skin_buffers, this->skin_mapped);
        adopt_deformation(2, this->skin_buffers_previous, this->skin_previous_mapped);
        adopt_deformation(3, this->morph_buffers, this->morph_mapped);
        if (std::any_of(deformation_changed.begin(), deformation_changed.end(), [](bool changed) { return changed; })) {
            this->publish_deformation_bindings();
        }
        if (camera_changed) {
            auto& owner = missing_camera ? fallback : *this->frame_chain;
            camera_ubo initial{};
            for (uint32_t slot = 0; slot < context.frame_slots; ++slot) {
                auto const copied = owner.write_frame_buffer(slot, resource_id::camera_ubo, std::as_bytes(std::span(&initial, 1)));
                if (copied != rhi::error::ok) {
                    return copied;
                }
            }
            this->camera_buffers.clear();
            for (auto const& object : camera->objects) {
                this->camera_buffers.push_back({object.buffer.get()});
            }
            this->publish_camera_bindings();
        }
        if (light_changed) {
            auto& owner = missing_light ? fallback : *this->frame_chain;
            light_ubo initial{};
            for (uint32_t slot = 0; slot < context.frame_slots; ++slot) {
                auto const copied = owner.write_frame_buffer(slot, resource_id::light_ubo, std::as_bytes(std::span(&initial, 1)));
                if (copied != rhi::error::ok) {
                    return copied;
                }
            }
            this->light_buffers.clear();
            for (auto const& object : light->objects) {
                this->light_buffers.push_back({object.buffer.get()});
            }
        }
        if (head_changed) {
            auto& owner = missing_head ? fallback : *this->frame_chain;
            head_ubo initial{}; // usable fallback axes, not an all-zero head basis
            for (uint32_t slot = 0; slot < context.frame_slots; ++slot) {
                auto const copied = owner.write_frame_buffer(slot, resource_id::head_ubo, std::as_bytes(std::span(&initial, 1)));
                if (copied != rhi::error::ok) {
                    return copied;
                }
            }
            this->head_buffers.clear();
            for (auto const& object : head->objects) {
                this->head_buffers.push_back({object.buffer.get()});
            }
        }
        if (shadow_changed) {
            this->shadow_images.clear();
            this->shadow_array_views.clear();
            this->shadow_layer_views.clear();
            for (auto const& object : shadow->objects) {
                this->shadow_images.push_back({object.image.get()});
                this->shadow_array_views.push_back({object.array_view.get()});
                deren::utility::vessel::vector<managed::borrowed<rhi::image_view>> layers;
                for (auto const& layer : object.layers) {
                    layers.push_back({layer.get()});
                }
                this->shadow_layer_views.push_back(std::move(layers));
            }
            this->shadow_allocated_layers = shadow->description.image.array_layers;

            ++this->shadow_content_version;
            this->shadow_frustum_valid = false;
        }
        if (cluster_changed) {
            this->cluster_count_buffers.clear();
            this->cluster_index_buffers.clear();
            for (uint32_t slot = 0; slot < context.frame_slots; ++slot) {
                this->cluster_count_buffers.push_back({counts->objects[slot].buffer.get()});
                this->cluster_index_buffers.push_back({indices->objects[slot].buffer.get()});
            }
            this->publish_cluster_bindings();
        }
        if (shadow_changed || light_changed || head_changed) {
            this->write_light_and_shadow_bindings();
        }
        if (outputs_changed) {
            auto const adopt = [&](size_t family, auto& images, auto& views) {
                auto const& generation = outputs[family];
                for (uint32_t i = 0; i < generation->objects.size(); ++i) {
                    images[i] = {generation->objects[i].image.get()};
                    views[i] = {generation->objects[i].array_view.get()};
                    if (contract_descriptor_ready(this->rhi_face()) &&
                        !contract_write_descriptor_image(this->rhi_face(), pass::output_targets_pass::heap_slots[family] + i,
                                                         *images[i], rhi::image_view_desc{.layer_count = 0, .mip_count = 0}, rhi::descriptor_type::sampled_image)) {
                        deren::utility::log("descriptor heap: managed output target publication failed");
                    }
                    uint32_t const storage_slot = pass::output_targets_pass::storage_slots[family];
                    if (storage_slot != 0 && contract_descriptor_ready(this->rhi_face())) {
                        rhi::image_view_desc range{};
                        range.role = rhi::view_role::storage;
                        if (!contract_write_descriptor_image(this->rhi_face(), storage_slot + i,
                                                             *images[i], range, rhi::descriptor_type::storage_image)) {
                            deren::utility::log("descriptor heap: managed storage target publication failed");
                        }
                    }
                }
            };
            adopt(0, this->hdr_images, this->hdr_image_views);
            for (size_t target = 0; target < this->gbuffer_images.size(); ++target) {
                adopt(target + 1, this->gbuffer_images[target], this->gbuffer_image_views[target]);
            }
            adopt(4, this->velocity_images, this->velocity_image_views);
            adopt(5, this->scene_color_images, this->scene_color_image_views);
            adopt(6, this->gbuffer_depth_images, this->gbuffer_depth_image_views);
            adopt(7, this->taa_history_images, this->taa_history_image_views);
            adopt(8, this->ldr_images, this->ldr_image_views);
            for (size_t level = 0; level < this->bloom_images.size(); ++level) {
                adopt(9 + level, this->bloom_images[level], this->bloom_image_views[level]);
            }
            adopt(13, this->rt_shadow_images, this->rt_shadow_image_views);
            adopt(14, this->visibility_images, this->visibility_image_views);
            if (software_resources_wanted) {
                adopt(15, this->software_depth_images, this->software_depth_image_views);
                adopt(16, this->software_id_images, this->software_id_image_views);
            } else {
                this->software_depth_images = {};
                this->software_id_images = {};
                this->software_depth_image_views = {};
                this->software_id_image_views = {};
            }
            this->reset_image_generation_state();
        }
        if (!software_resources_wanted) {
            this->software_tiles_resource_view = {};
            this->software_tiles_buffer = {};
            this->software_tiles_mapped = nullptr;
            this->software_raster_resources_ready = false;
        } else {
            bool const tile_generation_changed = this->software_tiles_resource_view.find(resource_id::software_tiles) != software_tiles_generation;
            if (tile_generation_changed) {
                this->software_tiles_resource_view = software_tiles_view;
                this->software_tiles_buffer = {software_tiles_generation->objects[0].buffer.get()};
                this->software_tiles_mapped = software_tiles_generation->objects[0].buffer->mapped().data();
            }
            if (!this->software_tiles_buffer || this->software_tiles_mapped == nullptr) {
                return rhi::error::invalid_argument;
            }
            if (contract_descriptor_ready(this->rhi_face()) &&
                !this->write_heap_buffer(*this->software_tiles_buffer, deren::engine::render_layout::heap_slots::software_tiles,
                                         this->software_tiles_buffer->size(), rhi::descriptor_type::storage_buffer)) {
                deren::utility::log("software raster: CSR tile buffer descriptor publication failed");
            }
            this->software_raster_resources_ready = static_cast<bool>(this->software_depth_images[0]) && static_cast<bool>(this->software_id_images[0]);
        }
        this->output_resource_views = output_views;
        this->camera_resource_view = camera_view;
        this->deformation_resource_views = deformation_views;
        this->scene_table_resource_views = table_views;
        if (!output_only) {
            this->light_resource_view = light_view;
            this->head_resource_view = head_view;
            this->shadow_resource_view = shadow_view;
            this->cluster_resource_view = cluster_view;
        }
        // THE HZB BUILD LANE'S TWO GPU-ONLY TABLES (M3 phase A2): sized from the CURRENT render extent and
        // rebuilt only when that extent changes. A no-op unless DEREN_HZB_BUILD is set - the OFF path allocates
        // nothing, records nothing and therefore cannot move a pixel. It runs here (the one resource-preparation
        // point every frame passes through) rather than in `record_main_drawcalls`'s frozen segment because the
        // renderer's frame partitions are not this task's scope; the ordering property the spec asks for is the
        // same: the buffers and their heap descriptors exist BEFORE the frame that imports them records.
        this->ensure_hzb_resources();
        return rhi::error::ok;
    }

    rhi::error runtime::upload_scene_constant(uint32_t completed_slot, render_resource::resource_id id, std::span<std::byte const> bytes) {
        auto* owner = &this->shared_resource_fallback;
        if (this->frame_chain != nullptr && this->frame_chain->resources().snapshot().find(id)) {
            owner = this->frame_chain;
        }
        return owner->write_frame_buffer(completed_slot, id, bytes);
    }

    // Explicit cascade growth retains the setter's existing fatal-error policy.
    // Frame preparation calls prepare_managed_resources directly so it can report acquire_failed.
    void runtime::log_hzb_cull_stats() {
        // THE PHASE-B EVIDENCE LINES (spec §10.3): the ONLY GPU->CPU read of the cull stats block in a phase-B run,
        // taken here because the destructor has already waited for the device.
        if (!this->hzb_cull_enabled || this->hzb_cull_failed || !this->hzb_cull_stats_valid || this->hzb_results_mapped == nullptr) {
            return;
        }
        auto* const results = static_cast<std::uint32_t*>(this->hzb_results_mapped);
        // ---- THE LAST FRAMES' PENDING STATS ARE FOLDED IN HERE ----
        // Each slot's stats hold its LAST culling frame's numbers; every earlier frame was accumulated when the slot
        // was reused (`reset_hzb_cull_stats`). So at shutdown each slot is pending exactly once - add them all.
        std::uint64_t last_candidates = 0u, last_culled = 0u, last_survivors = 0u;
        for (std::uint64_t slot = 0u; slot < this->frame_ring().slot_count(); ++slot) {
            std::uint32_t const* const stats = results + static_cast<std::size_t>(slot) * pass::hzb_results_words_per_lane + pass::hzb_results_stats_offset;
            std::uint64_t const candidates = stats[pass::hzb_stat_candidates];
            std::uint64_t const culled = stats[pass::hzb_stat_culled];
            std::uint64_t const survivors = stats[pass::hzb_stat_survivors];
            std::uint64_t const overflow = stats[pass::hzb_stat_overflow_items];
            if (candidates != 0u || culled != 0u || survivors != 0u || overflow != 0u) {
                this->hzb_cull_session_candidates += candidates;
                this->hzb_cull_session_culled += culled;
                this->hzb_cull_session_survivors += survivors;
                this->hzb_cull_session_overflow_items += overflow;
                this->hzb_recovery_session_retested += stats[pass::hzb_stat_retested];
                this->hzb_recovery_session_recovered += stats[pass::hzb_stat_recovered];
                ++this->hzb_cull_session_frames;
            }
            if (slot == this->hzb_cull_last_slot) {
                last_candidates = candidates;
                last_culled = culled;
                last_survivors = survivors;
            }
        }
        if (this->hzb_recovery_enabled) {
            deren::utility::log("hzb recovery: session {} retested / {} recovered / {} still occluded", this->hzb_recovery_session_retested, this->hzb_recovery_session_recovered, this->hzb_recovery_session_retested - this->hzb_recovery_session_recovered);
        }
        std::uint64_t const session_candidates = this->hzb_cull_session_candidates;
        std::uint64_t const session_culled = this->hzb_cull_session_culled;
        std::uint64_t const session_survivors = this->hzb_cull_session_survivors;
        std::uint32_t const frames = this->hzb_cull_session_frames;
        double const session_share = session_candidates != 0u ? 100.0 * static_cast<double>(session_culled) / static_cast<double>(session_candidates) : 0.0;
        double const last_share = last_candidates != 0u ? 100.0 * static_cast<double>(last_culled) / static_cast<double>(last_candidates) : 0.0;
        double const average = frames != 0u ? static_cast<double>(session_candidates) / static_cast<double>(frames) : 0.0;
        // THE SESSION TOTALS AND THE LAST FRAME, one line each: the first is the run's exact cumulative evidence (what
        // every acceptance number is calibrated against), the second names the frame the probe dumped. The overflow
        // count is 0 in every healthy run and names the items whose run could not be described (§9.4), i.e. the only
        // way this stage ever answers "nothing survived" for an item that had candidates.
        deren::utility::log("hzb cull: session {} candidate(s) / {} culled / {} survivor(s) ({}% culled) over {} culling frame(s), "
                            "{} candidate(s)/frame; test = conservative sphere-vs-HISTORY-HZB, CPU-reference verified",
                            session_candidates,
                            session_culled,
                            session_survivors,
                            session_share,
                            frames,
                            average);
        deren::utility::log("hzb cull: last culling frame (slot {}) had {} candidate(s) / {} culled / {} survivor(s) ({}% culled); "
                            "{} overflow item(s) in the session",
                            this->hzb_cull_last_slot,
                            last_candidates,
                            last_culled,
                            last_survivors,
                            last_share,
                            this->hzb_cull_session_overflow_items);
    }

    void runtime::create_shadow_resources() {
        if (this->prepare_managed_resources() != rhi::error::ok) {
            deren::utility::panic("failed to prepare pass resources");
        }
    }

    namespace {

        /// THE HEAP'S COPY OF ONE IMAGE, built from the SAME arguments `core::make_image_view` uses (see
        /// deren::engine::make_image_view_info in deren.vulkan.constant_init): a heap image descriptor carries a CREATE INFO
        /// rather than a view, and the driver makes the view inside it. That is why this is called where the image
        /// and its view are created - only that site knows the format, the view type and the range.
        bool write_heap_grid_image(rhi::api_core& vk, uint32_t const slot, rhi::image const* const image) {
            if (!contract_descriptor_ready(vk) || image == nullptr) {
                return false;
            }
            return contract_write_descriptor_image(vk, slot,
                                                   *image, rhi::image_view_desc{.layer_count = 0, .mip_count = 0}, rhi::descriptor_type::sampled_image);
        }

        /**
         * @brief write ONE per-slot binding of the scene block into the GRID - one slot per frame in flight
         *
         * Publish each frame's buffer object at `slot_base + slot`: one logical slot per frame in
         * flight, which is exactly where the shaders read it (`heap_slots_scene_camera + heap_frame_slot` and its
         * neighbours). The heap is the only path now, so "the heap is not in use" is a startup
         * failure rather than a quiet fall back to a descriptor set.
         */
        template <typename Buffer>
        void write_heap_scene_buffer(rhi::api_core& vk, deren::utility::vessel::vector<Buffer> const& buffers, uint32_t const slot_base, std::uint64_t const size, rhi::descriptor_type const type) {
            if (!contract_descriptor_ready(vk)) {
                return;
            }
            uint32_t written = 0;
            for (uint32_t slot = 0; slot < buffers.size(); ++slot) {
                std::uint32_t const descriptor_slot = slot_base + slot;
                if (contract_write_descriptor_buffer(vk, descriptor_slot, *buffers[slot], size, type)) {
                    ++written;
                } else {
                    deren::utility::log("descriptor heap: the per-frame buffer for grid slot {} (frame slot {}) did not reach logical slot {}", slot_base, slot, descriptor_slot);
                }
            }
            // SUCCESS IS LOGGED TOO, and that is not noise: a heap write has no picture to show for itself until
            // the shaders read the heap, so "no failure line" and "the buffers were empty, so nothing was written"
            // look exactly alike. This line is what tells them apart (it is the same reason the texture array and
            // the material table each log their count).
            deren::utility::log("descriptor heap: {} per-frame descriptor(s) written for grid slots {}..{}", written, slot_base, slot_base + (buffers.empty() ? 0u : static_cast<uint32_t>(buffers.size()) - 1u));
        }
    } // namespace

    void runtime::publish_camera_bindings() {
        write_heap_scene_buffer(this->vulkan_core, this->camera_buffers, deren::engine::render_layout::heap_slots::scene_camera,
                                sizeof(camera_ubo), rhi::descriptor_type::storage_buffer);
    }

    void runtime::publish_deformation_bindings() {
        write_heap_scene_buffer(this->vulkan_core, this->motion_buffers, deren::engine::render_layout::heap_slots::previous_transforms, pass::scene_deformation_pass::sizes[0], rhi::descriptor_type::storage_buffer);
        write_heap_scene_buffer(this->vulkan_core, this->skin_buffers, deren::engine::render_layout::heap_slots::skin_matrices, pass::scene_deformation_pass::sizes[1], rhi::descriptor_type::storage_buffer);
        write_heap_scene_buffer(this->vulkan_core, this->skin_buffers_previous, deren::engine::render_layout::heap_slots::skin_matrices_previous, pass::scene_deformation_pass::sizes[2], rhi::descriptor_type::storage_buffer);
        write_heap_scene_buffer(this->vulkan_core, this->morph_buffers, deren::engine::render_layout::heap_slots::morph_data, pass::scene_deformation_pass::sizes[3], rhi::descriptor_type::storage_buffer);
    }

    void runtime::publish_scene_table_bindings() {
        deren::utility::vessel::array<decltype(this->material_buffer), 10> const handles{this->material_buffer, this->instance_buffer, this->toon_colour_buffer, this->toon_lane_buffer, this->toon_rig_buffer,
                                                                                         this->meshlet_buffer, this->mesh_indirect_buffer, this->meshlet_stats_buffer, this->meshlet_culled_buffer,
                                                                                         this->visibility_items_buffer};
        auto const required = pass::scene_tables_pass::descriptions(render_resource::instances::resource_context{.frame_slots = this->frame_ring().slot_count()});
        if (!contract_descriptor_ready(this->rhi_face())) {
            return;
        }
        for (size_t i = 0; i < handles.size(); ++i) {
            auto const slot = pass::scene_tables_pass::heap_slots[i];
            if (slot != 0 && !this->write_heap_buffer(*handles[i], slot, required[i].buffer.size, rhi::descriptor_type::storage_buffer)) {
                deren::utility::log("descriptor heap: managed scene table publication failed for slot {}", slot);
            }
        }
    }

    void runtime::publish_hzb_bindings() {
        if (!contract_descriptor_ready(this->rhi_face()) || !this->hzb_levels_buffer || !this->hzb_header_buffer) {
            return;
        }
        // THE SAME SPELLING EVERY PUBLISH PATH USES: an address and a length, one storage-class descriptor per
        // table. The descriptor covers the WHOLE buffer (every frame-slot lane); a shader picks its lane by
        // indexing `frame_slot * stride + index` INSIDE it - the internal-lane rule the heap slot comments state
        // (§5.3), which is why neither table gets a per-frame descriptor mirror.
        if (!this->write_heap_buffer(*this->hzb_levels_buffer.get(), deren::engine::render_layout::heap_slots::hzb_levels, this->hzb_levels_buffer.get()->size(),
                                     rhi::descriptor_type::storage_buffer)) {
            deren::utility::log("hzb: the levels table did not reach heap slot {}", deren::engine::render_layout::heap_slots::hzb_levels);
            this->hzb_resources_ready = false;
        }
        if (!this->write_heap_buffer(*this->hzb_header_buffer.get(), deren::engine::render_layout::heap_slots::hzb_header, this->hzb_header_buffer.get()->size(),
                                     rhi::descriptor_type::storage_buffer)) {
            deren::utility::log("hzb: the header table did not reach heap slot {}", deren::engine::render_layout::heap_slots::hzb_header);
            this->hzb_resources_ready = false;
        }
        // ---- THE PHASE-B TABLES (M3 phase B1): only when the cull lane is armed, so an A2-only run publishes
        // exactly the two descriptors it always did. Each table is ONE descriptor over the whole buffer with the
        // per-frame lanes inside it - the same §5.3 rule the two above follow. ----
        if (!this->hzb_cull_enabled || this->hzb_cull_failed) {
            return;
        }
        this->hzb_cull_resources_ready = true;
        struct publication {
            rhi::buffer const* buffer;
            std::uint32_t slot;
            char const* what;
        };
        std::array const publications{
            publication{this->hzb_items_buffer.get(), deren::engine::render_layout::heap_slots::hzb_items, "items"},
            publication{this->hzb_results_buffer.get(), deren::engine::render_layout::heap_slots::hzb_results, "results"},
            publication{this->hzb_survivors_buffer.get(), deren::engine::render_layout::heap_slots::hzb_survivors, "survivors"},
            publication{this->mesh_indirect_buffer.get(), deren::engine::render_layout::heap_slots::hzb_commands, "commands"},
        };
        for (auto const& entry : publications) {
            if (entry.buffer == nullptr || !this->write_heap_buffer(*entry.buffer, entry.slot, entry.buffer->size(), rhi::descriptor_type::storage_buffer)) {
                deren::utility::log("hzb: the {} table did not reach heap slot {}", entry.what, entry.slot);
                this->hzb_cull_resources_ready = false;
            }
        }
    }

    void runtime::ensure_hzb_resources() {
        // THE SWITCH: DEREN_HZB_BUILD (read once, at construction) and nothing else. Unset means this function
        // returns before allocating or publishing anything, so the OFF frame has no HZB buffer, no HZB pipeline and
        // no HZB heap descriptor - the strict form of "OFF is byte-identical to the pre-HZB frame".
        if (!this->hzb_build_enabled || this->hzb_build_failed) {
            return;
        }
        if (!contract_descriptor_ready(this->rhi_face())) {
            return; // the descriptor heap is what the tables are published through: the next frame retries
        }
        // ---- THE PASSES, LAZILY ----
        // The application is the shader loader (chores::setup_pipeline) and it runs AFTER this runtime's
        // constructor, so the passes cannot be created at construction: the first frame's resource preparation is
        // the first point where both the shader bytes and the device exist. A registry miss is not a failure -
        // it just means the app has not registered the module yet (or never will, e.g. a headless test).
        if (this->hzb_build == nullptr) {
            if (this->registered_shader(pass::hzb_build_shader_name).empty()) {
                return;
            }
            auto owned = std::make_unique<pass::hzb_build_pass>();
            owned->create(this->make_pass_context());
            if (!owned->ready()) {
                this->hzb_build_failed = true;
                deren::utility::log("hzb: the build lane is unavailable (the pipeline was refused); no HZB table will be built this run");
                return;
            }
            this->hzb_build = std::move(owned);
            deren::utility::log("hzb: build lane ARMED (DEREN_HZB_BUILD) - the max pyramid is built from the frame's "
                                "G-buffer depth and validated by the DEREN_HZB_PROBE_CAPTURE readback; nothing culls against it yet");
        }
        // THE CULL PASS IS A LAZY SECOND PASS, AND A MISSING SHADER DOES NOT BLOCK THE BUILD LANE: the app registers
        // the module under the same two env gates (chores::setup_pipeline), so an absent registration means the
        // cull lane is simply not available this run - the lane stays pruned and the frame keeps today's path.
        if (this->hzb_cull_enabled && this->hzb_cull == nullptr && !this->hzb_cull_failed) {
            if (!this->registered_shader(pass::hzb_cull_shader_name).empty()) {
                auto owned = std::make_unique<pass::hzb_cull_pass>();
                owned->create(this->make_pass_context());
                if (!owned->ready()) {
                    this->hzb_cull_failed = true;
                    deren::utility::log("hzb: the cull lane is unavailable (the pipeline was refused); no candidate will be tested this run");
                } else {
                    this->hzb_cull = std::move(owned);
                    deren::utility::log("hzb: cull lane ARMED (DEREN_HZB_BUILD + DEREN_HZB_CULL) - per-item groups test the "
                                        "host-selected candidates against the HISTORY pyramid and publish flags/survivors/counts; "
                                        "visibility consumes the survivors and GPU indirect commands while the static gate is open");
                }
            }
        }
        if (this->hzb_recovery_enabled && !this->hzb_recovery_failed && this->hzb_recovery == nullptr && this->hzb_cull != nullptr) {
            auto recover = std::make_unique<pass::hzb_cull_pass>();
            recover->create(this->make_pass_context());
            auto raster = std::make_unique<pass::visibility_raster_pass>();
            raster->set_load_existing(true);
            raster->create(this->make_pass_context());
            if (!recover->ready() || !raster->has_pipeline()) {
                this->hzb_recovery_failed = true;
                deren::utility::log("hzb recovery: unavailable; keep complete single-pass draws");
            } else {
                this->hzb_recovery = std::move(recover);
                this->visibility_recovery = std::move(raster);
                deren::utility::log("hzb recovery: ARMED; current-depth retest and LOAD raster, limits {} radius / {} degrees / {} projection delta", this->hzb_recovery_limits.translation_ratio, this->hzb_recovery_limits.rotation_degrees, this->hzb_recovery_limits.projection_delta);
            }
        }
        rhi::image_extent const extent = this->render_extent();
        bool const cull_tables_ready = !this->hzb_cull_enabled || this->hzb_cull_failed || (this->hzb_items_buffer && this->hzb_results_buffer && this->hzb_survivors_buffer);
        if (this->hzb_resources_ready && cull_tables_ready && extent.width == this->hzb_extent.width && extent.height == this->hzb_extent.height) {
            this->hzb_build->set_hzb_levels(this->hzb_levels_buffer.get());
            return;
        }
        // ---- (RE)CREATION: the first frame, or a RENDER EXTENT CHANGE ----
        // §5.2's fallback does not get the resource manager's rebuild for free, so it takes the one wait the spec
        // budgets: at most ONE device idle per extent change (frames in flight may still read/write the previous
        // generation's buffers, and two frames in flight is the ring's own rule). There is no per-frame stall.
        // The phase-B tables do not depend on the extent, but they are recreated with the same generation so one
        // ownership path (and one invalidation path) covers every HZB table.
        if (this->hzb_levels_buffer || this->hzb_header_buffer || this->hzb_items_buffer || this->hzb_results_buffer || this->hzb_survivors_buffer ||
            this->hzb_resources_ready) {
            this->rhi_face().wait_idle();
        }
        this->hzb_resources_ready = false;
        this->hzb_cull_resources_ready = false;
        this->hzb_levels_buffer = {};
        this->hzb_header_buffer = {};
        this->hzb_items_buffer = {};
        this->hzb_results_buffer = {};
        this->hzb_survivors_buffer = {};
        this->hzb_items_mapped = nullptr;
        this->hzb_results_mapped = nullptr;
        this->hzb_valid = {};        // §9.3: a resize invalidates both lanes; the next frames rebuild before any read
        this->hzb_source_valid = {}; // ... and the phase-B scene gate has no snapshot to compare against either
        this->hzb_cull_stats_valid = false;
        if (extent.width == 0u || extent.height == 0u) {
            return;
        }
        std::uint32_t const frame_slots = this->frame_ring().slot_count();
        std::uint32_t const levels = deren::engine::visibility::hzb::level_count(extent.width, extent.height);
        std::uint32_t const words = deren::engine::visibility::hzb::pyramid_words(extent.width, extent.height);
        std::uint64_t const levels_bytes = static_cast<std::uint64_t>(frame_slots) * deren::engine::visibility::hzb::pyramid_bytes(extent.width, extent.height);
        std::uint64_t const header_bytes = static_cast<std::uint64_t>(frame_slots) * pass::hzb_header_byte_capacity;
        if (levels == 0u || words == 0u || levels_bytes == 0u) {
            return; // a degenerate extent has no pyramid: nothing to do, and the next frame's extent may be real
        }
        auto cluster = this->create_resource_cluster("hzb tables");
        if (!cluster) {
            this->hzb_build_failed = true;
            deren::utility::log("hzb: the build lane is unavailable (no resource cluster for its tables)");
            return;
        }
        // THE TWO BUILD TABLES, GPU-ONLY (§5.2): the GPU is their only writer, so they must not be host-visible
        // memory (a 1280x720 pyramid is 4.92 MB per lane; 4K's is 44.2 MB, and a host-visible twin of that would be
        // waste the design explicitly ruled out). `device_address` is what a heap buffer descriptor is built from
        // (address + length), and `transfer_source` is what the readback probe's `copy_buffer` requires.
        rhi::buffer_desc levels_desc{};
        levels_desc.size = levels_bytes;
        levels_desc.usage = rhi::buffer_usage::storage_gpu_only;
        levels_desc.flags = rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::transfer_source);
        auto created_levels = cluster->create_buffer(levels_desc);
        rhi::buffer_desc header_desc{};
        header_desc.size = header_bytes;
        header_desc.usage = rhi::buffer_usage::storage_gpu_only;
        header_desc.flags = levels_desc.flags;
        auto created_header = cluster->create_buffer(header_desc);
        if (!created_levels || !created_header) {
            this->hzb_build_failed = true;
            deren::utility::log("hzb: the build lane is unavailable (the HZB tables could not be created)");
            return;
        }
        this->hzb_levels_buffer = std::move(*created_levels);
        this->hzb_header_buffer = std::move(*created_header);
        this->hzb_extent = extent;
        this->hzb_resources_ready = true;
        // ---- THE THREE PHASE-B TABLES (M3 phase B1, spec §5.1) ----
        if (this->hzb_cull_enabled && !this->hzb_cull_failed) {
            this->create_hzb_cull_tables(*cluster);
        }
        this->publish_hzb_bindings();
        this->hzb_build->set_hzb_levels(this->hzb_levels_buffer.get());
        deren::utility::log("hzb: tables (re)created for {}x{} - {} level(s), {} word(s) per frame lane ({} B), levels table {} B for {} frame slot(s), "
                            "header {} B per lane; the pyramid is rebuilt from the frame's G-buffer depth every build frame",
                            extent.width,
                            extent.height,
                            levels,
                            words,
                            deren::engine::visibility::hzb::pyramid_bytes(extent.width, extent.height),
                            levels_bytes,
                            frame_slots,
                            pass::hzb_header_byte_capacity);
    }

    void runtime::ensure_software_raster_passes() {
        if (this->software_raster_pipeline_failed || !contract_descriptor_ready(this->rhi_face())) {
            return;
        }
        if (this->registered_shader("software_visibility.comp.spv").empty() ||
            this->registered_shader("software_merge.vert.spv").empty() ||
            this->registered_shader("software_merge.frag.spv").empty()) {
            return; // shader registration happens after runtime construction; retry at the next resource preparation
        }
        if (!this->software_visibility) {
            auto owned = std::make_unique<pass::software_visibility_pass>();
            owned->create(this->make_pass_context());
            if (!owned->ready()) {
                this->software_raster_pipeline_failed = true;
                deren::utility::log("software raster unavailable: software visibility pipeline creation failed");
                return;
            }
            this->software_visibility = std::move(owned);
        }
        if (!this->software_merge) {
            auto owned = std::make_unique<pass::software_merge_pass>();
            owned->create(this->make_pass_context());
            if (!owned->ready()) {
                this->software_raster_pipeline_failed = true;
                deren::utility::log("software raster unavailable: depth merge pipeline creation failed");
                return;
            }
            this->software_merge = std::move(owned);
        }
        deren::utility::log("software raster: raster and depth-merge passes ready");
    }

    void runtime::create_hzb_cull_tables(resource_cluster& cluster) {
        std::uint32_t const frame_slots = this->frame_ring().slot_count();
        std::uint64_t const items_bytes = static_cast<std::uint64_t>(frame_slots) * pass::hzb_cull_item_capacity * sizeof(pass::hzb_item);
        std::uint64_t const results_bytes = static_cast<std::uint64_t>(frame_slots) * pass::hzb_results_bytes_per_lane;
        std::uint64_t const survivors_bytes = static_cast<std::uint64_t>(frame_slots) * pass::hzb_cull_candidate_capacity * sizeof(deren::engine::meshlet);
        rhi::buffer_desc items_desc{};
        items_desc.size = items_bytes;
        items_desc.usage = rhi::buffer_usage::storage_coherent; // HOST-written per frame (the item recorder writes the lane)
        items_desc.flags = rhi::to_bits(rhi::buffer_flag::device_address);
        rhi::buffer_desc results_desc{};
        results_desc.size = results_bytes;
        results_desc.usage = rhi::buffer_usage::storage_coherent; // GPU-written, host-read at shutdown/probe time
        results_desc.flags = rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::transfer_source);
        rhi::buffer_desc survivors_desc{};
        survivors_desc.size = survivors_bytes;
        survivors_desc.usage = rhi::buffer_usage::storage_gpu_only; // GPU-only: nothing but a GPU consumer reads it
        survivors_desc.flags = rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::transfer_source);
        auto created_items = cluster.create_buffer(items_desc);
        auto created_results = cluster.create_buffer(results_desc);
        auto created_survivors = cluster.create_buffer(survivors_desc);
        if (!created_items || !created_results || !created_survivors) {
            this->hzb_cull_failed = true;
            deren::utility::log("hzb: the cull lane is unavailable (the phase-B tables could not be created)");
            return;
        }
        this->hzb_items_buffer = std::move(*created_items);
        this->hzb_results_buffer = std::move(*created_results);
        this->hzb_survivors_buffer = std::move(*created_survivors);
        // THE HOST-SIDE ZEROING IS PART OF THE CREATION because the two mapped tables are read before they are
        // first written: the item recorder writes only the entries of THIS frame's items, and the probe/shutdown
        // read every lane. A zeroed table means "no item, no candidates, every counter zero" rather than whatever
        // the allocator held.
        auto const items_bytes_mapped = this->hzb_items_buffer->mapped();
        auto const results_bytes_mapped = this->hzb_results_buffer->mapped();
        if (items_bytes_mapped.size() < items_bytes || results_bytes_mapped.size() < results_bytes) {
            this->hzb_cull_failed = true;
            deren::utility::log("hzb: the cull lane is unavailable (its host-visible tables are not mapped)");
            return;
        }
        std::fill(items_bytes_mapped.begin(), items_bytes_mapped.begin() + static_cast<std::ptrdiff_t>(items_bytes), std::byte{0});
        std::fill(results_bytes_mapped.begin(), results_bytes_mapped.begin() + static_cast<std::ptrdiff_t>(results_bytes), std::byte{0});
        this->hzb_items_mapped = items_bytes_mapped.data();
        this->hzb_results_mapped = results_bytes_mapped.data();
        this->hzb_cull_resources_ready = true;
        deren::utility::log("hzb cull: tables (re)created - items {} B ({} item(s) x {} frame slot(s), host-written), results {} B "
                            "({} word(s) per lane: flags[{}] + pass1/pass2 counts[{}] each + stats[{}], host-readable), survivors {} B "
                            "({} record(s) x {} frame slot(s), GPU-only)",
                            items_bytes,
                            pass::hzb_cull_item_capacity,
                            frame_slots,
                            results_bytes,
                            pass::hzb_results_words_per_lane,
                            pass::hzb_cull_candidate_capacity,
                            pass::hzb_cull_item_capacity,
                            pass::hzb_results_stats_capacity,
                            survivors_bytes,
                            pass::hzb_cull_candidate_capacity,
                            frame_slots);
    }

    void runtime::publish_cluster_bindings() {
        write_heap_scene_buffer(this->vulkan_core, this->cluster_count_buffers, deren::engine::render_layout::heap_slots::cluster_counts, static_cast<std::uint64_t>(deren::engine::max_cluster_count) * sizeof(uint32_t), rhi::descriptor_type::storage_buffer);
        write_heap_scene_buffer(this->vulkan_core, this->cluster_index_buffers, deren::engine::render_layout::heap_slots::cluster_indices, static_cast<std::uint64_t>(deren::engine::max_cluster_count) * deren::engine::cluster_light_capacity * sizeof(uint32_t), rhi::descriptor_type::storage_buffer);
    }

    void runtime::create_cluster_buffers() {
        if (this->scene_heap_buffers_published) {
            return;
        }
        // The managed cluster pair is already published by prepare_managed_resources.
        // Publish the remaining legacy scene-buffer bindings once.
        // A STORAGE DESCRIPTOR, NOT A UNIFORM ONE, and the shaders declare it as a `buffer` block: the storage
        // class a shader reads a heap descriptor through must match the descriptor's type, and a mismatch reads
        // as zeros with NO validation finding. Slang's `DescriptorHandle<ConstantBuffer<T>>` always fetches
        // through a StorageBuffer-class pointer, so a rhi::descriptor_type::uniform_buffer here made the Slang
        // G-buffer read a zero camera matrix (velocity NaN, motion channel black) while glslc's Uniform read of
        // the same slot worked. See docs/slang_migration.md, "the storage class that has to match".
        write_heap_scene_buffer(this->vulkan_core, this->camera_buffers, deren::engine::render_layout::heap_slots::scene_camera, sizeof(camera_ubo), rhi::descriptor_type::storage_buffer);
        // ... and the rest of the per-frame arrays, from the same place and with the SAME sizes the scene block
        // writes them with (see prepare_scene_resources's write_buffer_binding calls): motion (binding 13), skin (9) and
        // morph (10), each a two-slot array whose slot is the frame's. Bound here rather than in the per-slot loop
        // because a heap descriptor is an ADDRESS: the buffers are allocated once, so their addresses do not
        // change per frame, and only the CONTENTS are rewritten (see the per-frame slot rule in runtime.cppm).
        write_heap_scene_buffer(this->vulkan_core, this->motion_buffers, deren::engine::render_layout::heap_slots::previous_transforms, static_cast<std::uint64_t>(deren::engine::scene_motion_capacity) * sizeof(glm::mat4), rhi::descriptor_type::storage_buffer);
        write_heap_scene_buffer(this->vulkan_core, this->skin_buffers, deren::engine::render_layout::heap_slots::skin_matrices, static_cast<std::uint64_t>(deren::engine::scene_skin_capacity) * sizeof(glm::mat4), rhi::descriptor_type::storage_buffer);
        // ... and the previous-frame twin of the skin block, whose CONTENTS advance_motion_deformations()
        // rewrites per frame slot - so it is registered here, from the same size, for the same reason.
        write_heap_scene_buffer(this->vulkan_core, this->skin_buffers_previous, deren::engine::render_layout::heap_slots::skin_matrices_previous, static_cast<std::uint64_t>(deren::engine::scene_skin_capacity) * sizeof(glm::mat4), rhi::descriptor_type::storage_buffer);
        write_heap_scene_buffer(this->vulkan_core, this->morph_buffers, deren::engine::render_layout::heap_slots::morph_data, static_cast<std::uint64_t>(deren::engine::scene_morph_capacity) * sizeof(float), rhi::descriptor_type::storage_buffer);
        // THE INSTANCE TRANSFORM TABLE (set 0 binding 6) is the odd one: a SINGLE buffer rather than one per frame
        // slot (see runtime.cppm's member), so it takes ONE grid slot instead of a two-slot array - which is what
        // heap_slots::instance_transforms reserved. Written from the same size the scene block gives it.
        if (contract_descriptor_ready(this->rhi_face())) {
            if (!this->write_heap_buffer(*this->instance_buffer, deren::engine::render_layout::heap_slots::instance_transforms, static_cast<std::uint64_t>(deren::engine::instance_capacity) * sizeof(glm::mat4), rhi::descriptor_type::storage_buffer)) {
                deren::utility::log("descriptor heap: the instance transform table did not reach grid slot {}", deren::engine::render_layout::heap_slots::instance_transforms);
            }
        }
        this->scene_heap_buffers_published = true;
    }

    void runtime::prepare_scene_resources() {
        if (this->scene_heap_buffers_published) {
            return;
        }
        // A frame needs these resources even when no mesh was imported. Defer allocation until
        // application configuration is applied, then publish their heap bindings once.
        this->create_cluster_buffers();
        // binding 7 (light UBO, per-slot) is bound into each slot's heap block here, together with the shadow map's
        // grid slot.
        this->write_light_and_shadow_bindings();
    }

    void runtime::write_light_and_shadow_bindings() {
        // binding 7 (light UBO) + binding 8 (shadow map): BOTH point at THIS slot's own
        // resources (per-slot light buffers like the camera UBO, per-slot shadow images), so no
        // per-frame re-pointing is needed and an in-flight frame never shares a buffer the next
        // frame rewrites. Only the HEAP half is left: the two bindings are written into this slot's heap block
        // (and the shadow map into its grid slot) rather than into a descriptor set.
        for (int32_t slot = 0; slot < static_cast<int32_t>(this->frame_ring().slot_count()); ++slot) {
            rhi::image& shadow_image = *this->shadow_images[static_cast<std::size_t>(slot)];

            // THE SHADOW MAP GOES ONTO THE GRID HERE, because an image binding cannot be written the way a buffer
            // binding is: its heap descriptor is a CREATE INFO, rebuilt from the same arguments
            // core::make_depth_array_view uses - this slot's image, the depth format, a 2D-array view and the DEPTH
            // aspect (a colour aspect here would be a validation error, not a wrong picture). Which slot it
            // occupies is the frame's, matching shadow_images[slot].
            if (!write_heap_grid_image(this->vulkan_core, deren::engine::render_layout::heap_slots::shadow_map + static_cast<uint32_t>(slot), &shadow_image)) {
                deren::utility::log("descriptor heap: the shadow map for frame slot {} did not reach grid slot {}", slot, deren::engine::render_layout::heap_slots::shadow_map + static_cast<uint32_t>(slot));
            }

            // ---- AND THE LIGHT UBO, INTO THE HEAP'S BLOCK FOR THIS SLOT ----
            //
            // A heap descriptor for a buffer IS an address range: take the buffer's device address, write it at
            // the binding's offset inside this slot's block (core reserved the block and computed the offsets), and
            // nothing else changes. What does NOT work this way is an IMAGE binding: a heap image descriptor
            // carries a VkImageViewCreateInfo while a VkDescriptorImageInfo carries a view, not the image and range
            // that create info is made of - which is why the shadow map above is written where its image is known.
            if (contract_descriptor_ready(this->rhi_face())) {
                std::uint32_t const descriptor_slot = deren::engine::render_layout::heap_slots::scene_light + slot;
                if (!this->write_heap_buffer(*this->light_buffers[static_cast<std::size_t>(slot)], deren::engine::render_layout::heap_slots::scene_light + slot, sizeof(light_ubo), rhi::descriptor_type::storage_buffer)) {
                    deren::utility::log("descriptor heap: the light UBO did not fit slot {}'s block at logical slot {}", slot, descriptor_slot);
                }
            }

            // ---- AND THE HEAD FRAME, into its own block for this slot ----
            //
            // Its own block rather than a field of the light's, because it is a property of the CHARACTER rather
            // than of the scene's lighting - see `deren::engine::render_layout::heap_slots::scene_head`. Written here beside the light
            // because it is per-frame-slot for the same reason: on a model whose head turns it changes every
            // frame, so each slot needs its own copy.
            if (contract_descriptor_ready(this->rhi_face())) {
                std::uint32_t const head_slot = deren::engine::render_layout::heap_slots::scene_head + slot;
                if (!this->write_heap_buffer(*this->head_buffers[static_cast<std::size_t>(slot)], deren::engine::render_layout::heap_slots::scene_head + slot, sizeof(head_ubo), rhi::descriptor_type::storage_buffer)) {
                    deren::utility::log("descriptor heap: the head frame did not fit slot {}'s block at logical slot {}", slot, head_slot);
                }
            }
        }
    }

    void runtime::set_ibl(ibl_input const& info) {
        if (!info.env_size || !info.env_mip_count || !info.irr_size || !info.lut_size) {
            return;
        }
        deren::utility::vessel::vector<rhi::shared_object<rhi::image>> images;
        deren::utility::vessel::vector<rhi::shared_object<rhi::image_view>> views;
        auto const upload = [&](render_resource::resource_id id, std::span<uint8_t const> data, uint32_t size, uint32_t mips, rhi::image_format format, bool cube) {
            rhi::image_desc desc{};
            desc.extent = {size, size, 1};
            desc.mip_levels = mips;
            desc.array_layers = cube ? 6u : 1u;
            desc.format = format;
            desc.flags = rhi::to_bits(rhi::image_flag::sampled) | (cube ? rhi::to_bits(rhi::image_flag::cube_compatible) : rhi::no_image_flags);
            desc.initial_bytes = std::as_bytes(data);
            auto created = create_shared_image(id, desc);
            if (!created.first || !created.second) {
                return false;
            }
            images.push_back(std::move(created.first));
            views.push_back(std::move(created.second));
            return true;
        };
        using render_resource::resource_id;
        if (!upload(resource_id::ibl_env, info.prefiltered_env, info.env_size, info.env_mip_count, rhi::image_format::r16g16b16a16_sfloat, true) ||
            !upload(resource_id::ibl_irradiance, info.irradiance, info.irr_size, 1, rhi::image_format::r16g16b16a16_sfloat, true) ||
            !upload(resource_id::brdf_lut, info.brdf_lut, info.lut_size, 1, rhi::image_format::r16g16_sfloat, false)) {
            deren::utility::log("IBL resources could not be prepared; keeping the previous environment");
            return;
        }
        rhi::sampler_desc desc{};
        desc.address_mode = rhi::sampler_address_mode::clamp_to_edge;
        desc.max_lod = static_cast<float>(info.env_mip_count - 1);
        auto sampler = create_shared_sampler(resource_id::environment_sampler, desc);
        if (!sampler) {
            return;
        }
        this->rhi_face().wait_idle();
        deren::utility::vessel::array<uint32_t, 3> const slots{deren::engine::render_layout::heap_slots::env_cube,
                                                               deren::engine::render_layout::heap_slots::irradiance_cube, deren::engine::render_layout::heap_slots::brdf_lut};
        auto const write = [&](size_t index, rhi::image const& image) {
            return contract_write_descriptor_image(this->rhi_face(), slots[index], image,
                                                   rhi::image_view_desc{.layer_count = 0, .mip_count = 0}, rhi::descriptor_type::sampled_image);
        };
        size_t written = 0;
        for (; written < slots.size(); ++written) {
            if (!write(written, *images[written])) {
                break;
            }
        }
        if (written != slots.size()) {
            bool restored = this->ibl_images.size() == slots.size();
            for (size_t i = 0; i < written; ++i) {
                if (this->ibl_images.size() <= i || !this->ibl_images[i] || !write(i, *this->ibl_images[i])) {
                    restored = false;
                }
            }
            if (written && !restored) {
                for (auto const& image : images) {
                    this->failed_shared_publication.push_back(image);
                }
            }
            this->shared_heap_ready = this->shared_heap_ready && restored;
            deren::utility::log("IBL descriptor publication failed; previous bindings restored: {}", restored);
            return;
        }
        // Commit only after all heap writes succeed; old owners remain alive during rollback.
        this->ibl_views = std::move(views);
        this->ibl_images = std::move(images);
        this->env_sampler = std::move(sampler);
        this->failed_shared_publication.clear();
        this->shared_heap_ready = true;
        this->ibl_ready = true;
    }
    // THE ARTICLE'S POST LUT (`ZmdLutPost.shader`'s `_LutTex`), uploaded exactly as the BRDF LUT above is - the same
    // `create_image(data, ...)` -> `make_image_view` -> `write_heap_grid_image` triple, because a baked cube and a
    // generated one differ only in who filled the bytes.
    void runtime::set_post_lut(std::span<uint8_t const> const pixels, uint32_t const width, uint32_t const height) {
        if (pixels.empty() || width == 0u || height == 0u) {
            deren::utility::log("post LUT: nothing to upload ({} bytes, {}x{})", pixels.size_bytes(), width, height);
            return;
        }
        rhi::image_desc lut_desc{};
        lut_desc.extent = rhi::image_extent{.width = width, .height = height, .depth = 1u};
        lut_desc.mip_levels = 1;
        lut_desc.array_layers = 1;
        lut_desc.format = rhi::image_format::rgba8_srgb;
        lut_desc.flags = rhi::to_bits(rhi::image_flag::sampled);
        lut_desc.initial_bytes = std::as_bytes(std::span(pixels));
        lut_desc.debug_name = "post LUT image";
        auto created = create_shared_image(render_resource::resource_id::post_lut, lut_desc);
        if (!created.first || !created.second) {
            return;
        }
        this->rhi_face().wait_idle();
        if (!write_heap_grid_image(this->vulkan_core, deren::engine::render_layout::heap_slots::post_lut, created.first.get())) {
            deren::utility::log("descriptor heap: post_lut publication refused; keeping the previous LUT");
            return;
        }
        this->post_lut_image = std::move(created.first);
        this->post_lut_view = std::move(created.second);
        deren::utility::log("post LUT uploaded: {}x{}, {} bytes -> grid slot {}", width, height, pixels.size_bytes(), deren::engine::render_layout::heap_slots::post_lut);
    }

    // THE GOO REFERENCE'S PRE-INTEGRATED FGD LUT: the same `create_image` -> `make_image_view` ->
    // `write_heap_grid_image` triple as the two LUTs above, from the reference's own PNG bytes. WHAT DIFFERS FROM
    // BOTH OF THEM IS ONE FIELD, AND IT IS THE WHOLE POINT OF THIS FUNCTION:
    //
    //     `rhi::image_format::rgba8_unorm`, NOT `_SRGB`.
    //
    // The reference's image data-block is `colorspace = 'Non-Color'` - the ONLY such image in this project, every
    // `_RD` / `_D` map being `'sRGB'` - so Blender's texture node hands the graph the texel's raw bytes and the
    // screenshot's three FGD outputs are reads of those bytes. An sRGB upload would linearize each channel once and
    // every one of `specularFGD` / `reflectivity` / `diffuseFGD` would be wrong in the same direction (spec §3.1
    // item 1: the texel at `(0,32)` has `R = 47/255 = 0.184314`, which sRGB-decodes to `0.028`). It is also why
    // `source/tests/test_goo_toon_math.cpp` pins this format string and asserts the file's own SHA-256.
    //
    // IT IS A GLOBAL IMAGE AND NOT A LANE (step-5 spec §3.4): the Goo `GetPreIntegratedFGDGGXAndDisneyDiffuse`
    // group has ONE `ShaderNodeTexImage`, its `users == 3` containers share that data-block, and its coordinate is
    // computed from `sqrt(NoV)` / `perceptualRoughness` / `fresnel0` rather than from a material's UV or a material
    // map - so a lane per material would be eleven sidecar rows pointing at one file for no information at all.
    void runtime::set_goo_fgd_lut(std::span<uint8_t const> const pixels, uint32_t const width, uint32_t const height) {
        if (pixels.empty() || width == 0u || height == 0u) {
            deren::utility::log("goo FGD LUT: nothing to upload ({} bytes, {}x{})", pixels.size_bytes(), width, height);
            return;
        }
        rhi::image_desc fgd_desc{};
        fgd_desc.extent = rhi::image_extent{.width = width, .height = height, .depth = 1u};
        // ONE MIP, WHICH IS THE REFERENCE'S OWN ANSWER rather than a saving: Blender's Texture node has no `Mip`
        // input here (`image_user` states interpolation / extension / projection only), so it samples lod 0 - and a
        // generated chain read by a rough surface would give a different number from the reference's (spec §9-U6).
        fgd_desc.mip_levels = 1;
        fgd_desc.array_layers = 1;
        fgd_desc.format = rhi::image_format::rgba8_unorm;
        fgd_desc.flags = rhi::to_bits(rhi::image_flag::sampled);
        fgd_desc.initial_bytes = std::as_bytes(std::span(pixels));
        fgd_desc.debug_name = "goo FGD LUT image";
        auto created = create_shared_image(render_resource::resource_id::goo_fgd_lut, fgd_desc);
        if (!created.first || !created.second) {
            return;
        }
        this->rhi_face().wait_idle();
        if (!write_heap_grid_image(this->vulkan_core, deren::engine::render_layout::heap_slots::goo_fgd_lut, created.first.get())) {
            deren::utility::log("descriptor heap: goo_fgd publication refused; keeping the previous LUT");
            return;
        }
        this->goo_fgd_image = std::move(created.first);
        this->goo_fgd_view = std::move(created.second);
        deren::utility::log("goo FGD LUT uploaded: {}x{}, {} bytes, R8G8B8A8_UNORM -> grid slot {}", width, height, pixels.size_bytes(), deren::engine::render_layout::heap_slots::goo_fgd_lut);
    }

    material_id runtime::register_material(primitive_create_info const& info) {
        // ---- 1. Resolve the 5 texture slots against the shared array: identical texture bytes
        //         upload once, keyed by a CONTENT hash of the decoded bytes (xxh3 digest +
        //         format/dimensions - the loader hands every material its own copy of a shared
        //         glTF image, so a raw data pointer is NOT a stable identity; the digest lookup
        //         below is what actually dedups); missing slots point at the white fallback
        //         (element 0).
        deren::utility::vessel::array<std::pair<texture_input const*, rhi::image_format>, 5 + static_cast<std::size_t>(toon_slot::count)> const slots = {
            std::pair{&info.albedo, rhi::image_format::rgba8_srgb},
            std::pair{&info.metallic_roughness, rhi::image_format::rgba8_unorm},
            std::pair{&info.normal, rhi::image_format::rgba8_unorm},
            std::pair{&info.occlusion, rhi::image_format::rgba8_unorm},
            std::pair{&info.emissive, rhi::image_format::rgba8_srgb}, // glTF emissive textures are sRGB
            // ---- THE TOON SLOTS, in `toon_slot` order ----
            // THE FOUR COLOUR LANES ARE sRGB. They are colour data the reference decodes before using (its ramp
            // lookup is followed by `srgbToLinear(rd.rgb)`), so uploading them as linear would double-decode
            // them. The specular ramp is in there for the same reason: it is a COLOUR ramp rather than a scalar
            // curve.
            //
            // THE SDF IS NOT, AND THAT IS NOT AN OVERSIGHT: the reference declares its SDF sampler with LINEAR
            // filtering and CLAMP addressing and NO sRGB read flag, because the lightmap holds a DISTANCE FIELD
            // - a number per texel, not a colour - and decoding it as sRGB would bend the very quantity the
            // sigmoid thresholds. So this is the one toon lane uploaded UNORM.
            //
            // AND THIS ARRAY IS WHY THE ENUM AND THE INITIALISERS MUST AGREE: its size is `5 + toon_slot::count`,
            // so adding a lane to the enum without adding it here makes the last element value-initialised -
            // `{nullptr, VK_FORMAT_UNDEFINED}` - and the loop below dereferences `slots[i].first`. Measured: the
            // renderer died with an access violation inside `register_material` and the log stopped one stage
            // earlier, which is what sent the search to the descriptor writes instead of here.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::diffuse_ramp)], rhi::image_format::rgba8_srgb},
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::shadow_lut)], rhi::image_format::rgba8_srgb},
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::specular_ramp)], rhi::image_format::rgba8_srgb},
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::matcap)], rhi::image_format::rgba8_srgb},
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::sdf_lightmap)], rhi::image_format::rgba8_unorm},
            // THE METALLIC/GLOSS LANE IS UNORM FOR THE SDF'S REASON RATHER THAN THE RAMPS': its four channels are
            // metallic, reflectivity, occlusion and smoothness - NUMBERS, not colour - so an sRGB decode would
            // bend every one of them, and roughness read out of a decoded smoothness would be wrong everywhere on
            // the surface rather than at one band edge.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::metallic_gloss)], rhi::image_format::rgba8_unorm},
            // THE FACE MASK IS UNORM FOR THE SDF'S REASON RATHER THAN THE RAMPS': its four channels are REGION
            // WEIGHTS and a normal term - numbers, not colour - so an sRGB decode would bend every one of them.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::sdf_mask)], rhi::image_format::rgba8_unorm},
            // THE EMOTION ATLAS IS COLOUR - it is a painting of eyebrows and a mouth that REPLACES the albedo where
            // its alpha says so - so it takes the sRGB treatment the ramps do, not the mask's.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::emotion)], rhi::image_format::rgba8_srgb},
            // THE SPLIT NORMAL IS UNORM: its two packed tangent-space normals are DATA (`* 2 - 1` on the way in),
            // so an sRGB decode would bend both of them.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::split_normal)], rhi::image_format::rgba8_unorm},
            // THE GOO IRIS BALL IS COLOUR, so it takes the ramps' treatment rather than the masks': the reference
            // samples it and adds it to the albedo (`source/shaders/toon/goo_toon.slang`), i.e. it is light, not a number.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::goo_matcap05)], rhi::image_format::rgba8_srgb},
            // THE GOO BASE RAMP IS COLOUR, so it takes the treatment the reference gives it rather than the
            // masks': `images[...].colorspace = 'sRGB'` on all seven `_RD` images means Blender LINEARIZES each
            // texel before its node graph sees it (spec §5.3), so uploading this lane as `_SRGB` is what makes the
            // sampler do the same thing - and a lane uploaded UNORM would hand the shader a texel 2.2 gamma too
            // bright, which on a mid-grey ramp entry is the difference between a shadow and a highlight. ITS
            // ALPHA IS UNAFFECTED BY EITHER (Blender's colour management does not touch it), which is why the same
            // upload serves `RampAlpha` and the spec's §8-A6 asset-bound assertion.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::goo_base_ramp)], rhi::image_format::rgba8_srgb},
            // ---- STEP 7'S THREE FACE LANES, AND THE FORMAT OF EACH ONE IS A STATEMENT ABOUT ITS CHANNELS ----
            //
            // THE FACE'S SDF IS UNORM, for the article's `sdf_lightmap` above and NOT for the `_RD` ramps': the
            // reference reads it as `(R + G) / 2` and feeds that NUMBER to a `SigmoidSharp` whose `center` is
            // `0.10000000894069672` - a threshold on a distance field, not a colour - so an sRGB decode would bend
            // the very quantity the sigmoid thresholds, by 2.2 gamma, in the region where its slope is steepest.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::goo_face_sdf)], rhi::image_format::rgba8_unorm},
            // `cm_M` IS UNORM TOO, and it is the same kind of statement: its three channels are a LAYER SELECTOR
            // (`G`, compared against 0 and 1 exactly - `混合.002`'s factor), a WEIGHT (`R`, raised to `Front R Pow`
            // and smoothstepped) and a GATE (`A`, a multiply). None of them is light, so none of them may be
            // gamma-decoded; a `_SRGB` upload would turn a mask's "1.0" into "1.0" but its 0.2 into 0.033.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::goo_face_cm)], rhi::image_format::rgba8_unorm},
            // `CsutmMask` IS UNORM FOR THE SAME REASON ONE STEP FURTHER: the only channel the Face container reads
            // is `G`, and it is read through a `GREATER_THAN(·, 0.5)` - a comparison whose whole answer is the
            // comparison, so the upload's transfer function decides which side of 0.5 a texel lands on.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::goo_face_csumt)], rhi::image_format::rgba8_unorm},
            // `_M` IS UNORM AS WELL, and for the same statement once more: the `RS EFF` mask of mechanism table #14
            // is a MASK (`_M（非色彩）` in the reference's own node name), read through a luminance dot and then a
            // smoothstep, and its image's colorspace is Non-Color. A `_SRGB` upload would gamma-decode the very
            // quantity the smoothstep thresholds. See `toon_slot::goo_rs_mask`.
            //
            // THIS ENTRY IS NOT OPTIONAL AND NOT A TIDINESS: the array's size is `5 + toon_slot::count`, so adding
            // the enum lane above without this line value-initialises the LAST element to `{nullptr,
            // VK_FORMAT_UNDEFINED}` and the loop below dereferences `slots[i].first`. Measured: the renderer died
            // with an access violation inside `register_material` (see the block comment above this array).
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::goo_rs_mask)], rhi::image_format::rgba8_unorm},
            // ---- STEP 15'S ONE, AND IT IS THE LANE THAT SPENDS THE TABLE'S LAST SLOT ----
            //
            // THE SHEET IS SRGB, AND THE STATEMENT IS THE OPPOSITE OF THE MASK LINE ABOVE RATHER THAN A COPY OF
            // IT: an `_RS` sheet is COLOUR - the reference multiplies it into `RS ColorTint` and then into the
            // shaded colour - so it takes the ramps' treatment, and uploading it UNORM would hand the shader a
            // texel 2.2 gamma off in the very product this branch exists to make. The decode belongs to the
            // SAMPLER: the shader must not decode it a second time (see `toon_slot::goo_rs_sheet`).
            //
            // AND THIS LINE IS NOT OPTIONAL FOR THE REASON THE COMMENT ABOVE GIVES, WHICH IS NOW A MEASURED ONE: the
            // array's size is `5 + toon_slot::count`, and leaving this entry out value-initialises the last element
            // to `{nullptr, VK_FORMAT_UNDEFINED}` - which the loop below dereferences. Step 15 raises `count` from 15
            // to 16, so omitting this line is the same access violation inside `register_material` the RS mask's own
            // note records.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::goo_rs_sheet)], rhi::image_format::rgba8_srgb},
        };

        deren::utility::vessel::array<uint32_t, 5 + static_cast<std::size_t>(toon_slot::count)> texture_indices = {};
        uint32_t heap_texture_descriptors = 0; // how many went into the descriptor heap (see the log below)
        for (std::size_t i = 0; i < slots.size(); ++i) {
            texture_input tex = *slots[i].first;
            // Capture probe for imported single/partial-mip sources. Ordinary sessions leave it unset.
            if (auto* limit_text = std::getenv("DEREN_BENCH_TEXTURE_LEVELS")) {
                auto limit = std::atoi(limit_text);
                if (limit > 0 && std::uint32_t(limit) < tex.mip_levels) {
                    auto range = progressive_texture::mip_range(tex.width, tex.height, std::uint32_t(limit), 0);
                    if (range && range->bytes <= tex.data.size_bytes()) {
                        tex.mip_levels = std::uint32_t(limit);
                        tex.data = tex.data.first(range->bytes);
                    }
                }
            }
            if (!tex.valid || tex.data.empty()) {
                texture_indices[i] = this->white_texture_index; // white fallback
                continue;
            }
            // Content-addressed dedup: the loader hands every material its OWN byte copy of a
            // shared glTF image, so identical pixels arrive under different pointers. Hash the
            // decoded bytes (xxh3-128, the same digest vma uses for GPU-image dedup) and key the
            // slot cache on (digest, format, dimensions): N materials over one image upload
            // once and share the array element. The image itself is also vma-deduped below.
            deren::utility::xxh3_digest const digest = deren::utility::xxh3_128bits(std::span<uint8_t const>(tex.data.data(), tex.data.size_bytes()));
            // key on the digest data_block itself (not a raw byte array): data_block carries the
            // equality/ordering the deren::utility::vessel::map key needs
            auto const key = std::tuple<deren::utility::xxh3_digest, rhi::image_format, std::uint32_t, std::uint32_t, std::uint32_t>{
                digest, slots[i].second, tex.width, tex.height, tex.mip_levels};
            auto const cached = this->texture_slot_cache.find(key);
            if (cached != this->texture_slot_cache.end()) {
                texture_indices[i] = cached->second; // shared texture: reuse its slot
                continue;
            }
            if (this->owned_texture_views.size() >= runtime_detail::scene_texture_capacity) {
                // Array full (pathological scene with > scene_texture_capacity distinct images):
                // degrade this texture slot to the white element instead of crashing - the
                // material still renders untextured. Same policy as the material-table overflow
                // below (one-time log, then keep going); content dedup means real scenes rarely
                // get close to the cap.
                if (!this->texture_overflow_logged) {
                    this->texture_overflow_logged = true;
                    deren::utility::log("scene texture array capacity ({}) exceeded - extra textures render as white (element {})",
                                        runtime_detail::scene_texture_capacity, this->white_texture_index);
                }
                texture_indices[i] = this->white_texture_index; // white fallback, like an invalid texture
                continue;
            }
            rhi::image_desc image_info{};
            image_info.extent = rhi::image_extent{.width = tex.width, .height = tex.height, .depth = 1u};
            image_info.mip_levels = tex.mip_levels; // the caller uploads a full mip-major chain
            image_info.array_layers = 1;
            image_info.format = slots[i].second;
            image_info.flags = rhi::to_bits(rhi::image_flag::sampled);
            image_info.initial_bytes = std::as_bytes(tex.data);
            image_info.debug_name = "material texture";
            std::pair<rhi::shared_object<rhi::image>, rhi::shared_object<rhi::image_view>> created;
            if (texture_streaming.enabled && progressive_textures) {
                auto low = image_info;
                auto full_levels = tex.mip_levels ? tex.mip_levels : std::bit_width(std::max(tex.width, tex.height));
                auto low_mip = progressive_texture::floor_mip(tex.width, tex.height);
                std::optional<progressive_texture::fallback_image> reduced;
                auto suffix = progressive_texture::mip_range(tex.width, tex.height, full_levels, low_mip);
                if (progressive_mips && suffix) {
                    if (suffix->offset + suffix->bytes > tex.data.size_bytes())
                        deren::utility::panic("invalid source mip suffix");
                    low.extent.width = suffix->width;
                    low.extent.height = suffix->height;
                    low.mip_levels = suffix->levels;
                    low.initial_bytes = std::as_bytes(tex.data).subspan(suffix->offset, suffix->bytes);
                } else {
                    reduced = progressive_mips
                                  ? progressive_texture::resample(std::span(tex.data.data(), tex.data.size_bytes()), tex.width, tex.height,
                                                                  std::max(1u, tex.width >> low_mip), std::max(1u, tex.height >> low_mip), slots[i].second == rhi::image_format::rgba8_srgb)
                                  : progressive_texture::fallback(std::span(tex.data.data(), tex.data.size_bytes()), tex.width, tex.height, slots[i].second == rhi::image_format::rgba8_srgb);
                    if (!reduced)
                        deren::utility::panic("invalid progressive texture source");
                    low.extent.width = reduced->width;
                    low.extent.height = reduced->height;
                    low.mip_levels = 1;
                    low.initial_bytes = std::as_bytes(std::span(reduced->bytes));
                }
                low.debug_name = "resident low texture";
                created = create_shared_image(render_resource::resource_id::scene_textures, low);
                streamed_textures.resize(owned_textures.size() + 1);
                auto& entry = streamed_textures.back();
                entry.bytes = tex.data.size_bytes();
                entry.fallback = created.first;
                entry.fallback_bytes = low.initial_bytes.size();
                entry.source = share_texture_source(tex);
                entry.description = image_info;
                if (!entry.description.mip_levels)
                    entry.description.mip_levels = std::bit_width(std::max(tex.width, tex.height));
                entry.fallback_mip = progressive_texture::floor_mip(tex.width, tex.height);
                entry.resident_mip = entry.fallback_mip;
                if (progressive_mips && entry.fallback_mip == 0) {
                    // This low image already contains every original mip; no duplicate upload is needed.
                    entry.pinned = true;
                    entry.high = entry.fallback;
                }
                entry.description.initial_bytes = {};
                entry.description.debug_name = "progressive full texture";
            } else if (texture_streaming.enabled && rhi::query_extension<rhi::frame_checkpoint>(rhi_face())) {
                auto source = share_texture_source(tex);
                image_info.initial_bytes = {};
                auto lazy = rhi_face().resources->create_lazy_image({.resource = image_info, .data = source});
                if (!lazy)
                    deren::utility::panic("failed to create streamed material texture");
                if (streamed_textures.size() < owned_textures.size())
                    streamed_textures.resize(owned_textures.size());
                rhi::shared_object<rhi::lazy_image> object(std::move(*lazy));
                created.first = object;
                // Only slot0's view is consumed by legacy runtime bindings; no physical view for streamed images.
                created.second = owned_texture_views[white_texture_index];
                streamed_textures.resize(owned_textures.size() + 1);
                streamed_textures.back().image = object;
                streamed_textures.back().bytes = tex.data.size_bytes();
            } else {
                created = create_shared_image(render_resource::resource_id::scene_textures, image_info);
                streamed_textures.resize(owned_textures.size() + 1);
            }
            this->owned_textures.push_back(std::move(created.first));
            this->owned_texture_views.push_back(std::move(created.second));
            rhi::image_view_desc tex_range{};
            tex_range.layer_count = 0;
            tex_range.mip_count = 0;
            if (!static_cast<bool>(this->owned_textures.back()) || !static_cast<bool>(this->owned_texture_views.back())) {
                deren::utility::panic("failed to create material texture");
            }
            // THE INDEX IS THE VIEW JUST APPENDED (see where the white element sets `white_texture_index` the
            // same way): the native cache that used to be counted here held the same views in the same order, so
            // its `size()` - taken before its own push - was this index.
            uint32_t const index = static_cast<uint32_t>(this->owned_texture_views.size() - 1);
            this->texture_slot_cache.emplace(key, index);
            texture_indices[i] = index;

            // A texture is registered ONCE, at scene load, and never rewritten - which is why the texture array is
            // the first binding this renderer puts on the heap: there is no per-frame rewrite and therefore no
            // frame-in-flight hazard to design around. The array starts at its own grid slot
            // (deren::engine::render_layout::heap_slots::textures) and advances one SLOT per texture, i.e. 64 B - NOT the device's
            // imageDescriptorSize: the grid's single stride is what lets a shader index it with
            // `descriptor_stride = 64` (see docs/descriptor_heap_migration.md). The descriptor is a VIEW TO CREATE
            // rather than the view above: VkImageDescriptorInfoEXT carries a VkImageViewCreateInfo and the driver
            // makes the view itself. The values below are the ones core::make_image_view uses, on purpose - a view
            // that differs in mip range would sample a different image than the descriptor-set path.
            //
            // Nothing READS the heap yet, so a failure here is a log line and not a wrong frame - but it is the
            // write path that has to work first.
            if (contract_descriptor_ready(this->rhi_face())) {
                {
                    std::uint32_t const descriptor_slot = deren::engine::render_layout::heap_slots::textures + index;
                    auto& descriptor_image = streamed_textures[index].image ? *owned_textures[white_texture_index] : *owned_textures.back();
                    if (contract_write_descriptor_image(this->rhi_face(), descriptor_slot, descriptor_image, tex_range, rhi::descriptor_type::sampled_image)) {
                        ++heap_texture_descriptors;
                    } else {
                        deren::utility::log("descriptor heap: texture {} did not fit the resource heap at logical slot {}", index, descriptor_slot);
                    }
                }
            }
        }

        for (std::size_t i = 0; i < texture_indices.size(); ++i) {
            auto id = texture_indices[i];
            if (id >= streamed_textures.size() || (!streamed_textures[id].image && !streamed_textures[id].source))
                continue;
            bool const pin = texture_residency::pin_material_texture(i, info.factors.alpha_mask, info.factors.alpha_blend, info.toon_family, info.overlay_kind);
            if (pin && !streamed_textures[id].pinned) {
                streamed_textures[id].pinned = true;
                auto& entry = streamed_textures[id];
                if (entry.source) {
                    auto full = entry.description;
                    full.initial_bytes = std::as_bytes(std::span(*entry.source));
                    entry.high = rhi::shared_object<rhi::image>(rhi_face().resources->create_image(full));
                    if (!entry.high)
                        deren::utility::panic("failed to create pinned full texture");
                    owned_textures[id] = entry.high;
                    entry.resident_mip = 0;
                    entry.resident_bytes = entry.bytes;
                }
                rhi::image_view_desc range{};
                range.mip_count = 0;
                range.layer_count = 0;
                auto* pinned_image = entry.source ? entry.high.get() : entry.image.get();
                if (!contract_write_descriptor_image(rhi_face(), render_layout::heap_slots::textures + id, *pinned_image, range, rhi::descriptor_type::sampled_image))
                    deren::utility::panic("failed to publish pinned scene texture");
                streamed_textures[id].published = true;
            }
        }
        // THE HEAP-NATIVE PROBE, ONCE, now that the bindless array is actually in the heap: it is the first thing
        // in this renderer to read the heap instead of a descriptor set, and its answer goes to the log (see
        // run_heap_probe). Texture slot 0 is the white placeholder, which is registered first and always exists.
        if (heap_texture_descriptors != 0u) {
            this->run_heap_probe(static_cast<uint32_t>(deren::engine::render_layout::heap_slots::textures));
        }
        // ... and its GRAPHICS half, twice: once with the material table's REAL grid slot (which must come back
        // white, the default material's base colour) and once with a deliberately WRONG one (which must not). The
        // pair is the negative proof the mechanism needs - the same draw, the same shader, one different number.
        if (contract_descriptor_ready(this->rhi_face())) {
            this->run_heap_graphics_probe(static_cast<uint32_t>(deren::engine::render_layout::heap_slots::materials));
            this->run_heap_graphics_probe(static_cast<uint32_t>(deren::engine::render_layout::heap_slots::materials) + 1u);
            // ... AND THE MESH HALF OF THE SAME QUESTION (docs/mesh_shaders.md step 0): a MESH pipeline created
            // with the heap flag and no layout, dispatched with vkCmdDrawMeshTasksEXT, must read the same slot
            // and come back the same white value. That is the mechanism a mesh-shader geometry path would stand
            // on, and the log line is the proof - the same comparison the other two lines make.
            this->run_heap_graphics_probe(static_cast<uint32_t>(deren::engine::render_layout::heap_slots::materials), true);
        }

        // SAY WHAT WENT INTO THE HEAP, because the success path of a heap write is silent by nature (it returns
        // true and writes memory) and "no failure line" is not evidence that anything happened. This is the line a
        // reader checks to know the texture array really is on the heap; the mapping that points a shader stage at
        // it is the step after this one.
        if (heap_texture_descriptors > 0) {
            deren::utility::log("descriptor space: {} texture descriptors written", heap_texture_descriptors);
        }

        // ---- 2. Append one material record: texture indices + presence flags; factors keep
        //         their identity defaults (extend primitive_create_info to pass custom factors) ----
        material_record record = {};
        record.tex_indices = glm::uvec4(texture_indices[0], texture_indices[1], texture_indices[2], texture_indices[3]);
        record.emissive_index = texture_indices[4];
        // ---- THE TOON SLOTS, from the same upload loop and therefore through the same content-addressed dedup:
        //      a ramp shared by six materials of one character is uploaded ONCE. The FLAGS come from the sidecar
        //      rather than from the indices, because a lane can hold a real texture whose `_Use` flag is off -
        //      and a shader must not read it (see material_record::toon_flags).
        constexpr std::size_t toon_base = 5;
        record.toon_indices = glm::uvec4(texture_indices[toon_base + 0], texture_indices[toon_base + 1], texture_indices[toon_base + 2], texture_indices[toon_base + 3]);
        record.base_color_factor = info.factors.base_color_factor;
        record.emissive_factor = info.factors.emissive_factor;
        record.metallic_factor = info.factors.metallic_factor;
        record.roughness_factor = info.factors.roughness_factor;
        record.normal_scale = info.factors.normal_scale;
        record.alpha_cutoff = info.factors.alpha_cutoff;
        record.occlusion_strength = info.factors.occlusion_strength;
        // The toon family the loader classified this material's name into (0 == none). It rides the record's
        // fourth uint lane, which the std430 layout had already reserved as padding - see material_record.
        record.toon_family = info.toon_family;
        record.flags = 0;
        if (info.normal.valid) {
            record.flags |= 1u;
        }
        if (info.occlusion.valid) {
            record.flags |= 2u;
        }
        if (info.emissive.valid) {
            record.flags |= 4u;
        }
        if (info.double_sided) {
            record.flags |= 8u; // bit3: back faces are rendered, fragment shader flips normals
        }
        if (info.factors.alpha_mask) {
            record.flags |= 16u; // bit4: alphaMode MASK - fragment shader discards below alpha_cutoff
        }
        if (info.factors.alpha_blend) {
            record.flags |= 32u; // bit5: alphaMode BLEND - alpha-blended / transparent material
        }
        // ---- THE OVERLAY CHANNEL, on the record's two free bits ----
        //
        // THE FRAGMENT STAGE HAS TO BRANCH ON THIS, and that is the only reason it is in the record as well as
        // on the primitive: the article's two masks compute DIFFERENT multipliers from the same inputs -
        // `MyZmdEyeDarkShader` multiplies by `mask * _Alpha` while `MyZmdHairShadowShader` multiplies by the
        // scalar `_DayStrength` and never reads its mask - so one overlay pipeline draws both only because the
        // record tells it which. The primitive's copy is what the HOST reads to build the frame's leaf lists
        // (`primitive::overlay_kind`); this one is what the SHADER reads. It is the same deliberate
        // two-copies-of-one-import-fact arrangement `alpha_blend`/bit5 above already has, and it is two bits
        // rather than a lane because the record is at its 80-byte `static_assert` and `flags` had room.
        //
        // THE NUMBERS ARE `deren::gltf::overlay_kind`'S (1 = eye_dark, 2 = hair_shadow) and they are spelled as
        // literals here for the same reason every other flavour of this value is a number on this side of the
        // boundary: `deren.engine.runtime` does not import the loader's types - the classification is done where the
        // NAME exists and travels onwards as a value (see `toon_family` above, which does exactly this).
        // source/tests/test_gltf_loader.cpp asserts the enum's numbering so this pair cannot drift silently.
        if (info.overlay_kind == 1u) {
            record.flags |= 64u; // bit6: the EYE-DARK overlay (mask-driven)
        } else if (info.overlay_kind == 2u) {
            record.flags |= 128u; // bit7: the HAIR-SHADOW overlay (day-strength-driven)
        }
        // ---- THE AUTHOR'S TOON TRANSPARENT VARIANT (`_TRANSPARENT_ON`), ON ITS OWN BIT ----
        //
        // SEPARATE FROM bit5 (`alphaMode BLEND`) ON PURPOSE, and the two are not redundant: bit5 is glTF's
        // statement about coverage and is what the FRAGMENT stage writes a real `out_alpha` for; this one is the
        // sidecar's `_SrcBlend 5 / _DstBlend 10` pair, which the runtime evaluates at import (see
        // `toon_lookup::scalar`) because the blend state itself is per-PASS here. A material with this bit gets
        // the alpha-blended pipeline; every other material keeps the overwrite, and the two are the same
        // arithmetic at alpha 1.
        if (info.toon.alpha_blend) {
            record.flags |= 256u; // bit8: `_TRANSPARENT_ON` - the toon stage's own transparent variant
        }

        // ---- 3. Content-address the record, then append (or degrade on overflow) ----
        // Identical materials (same texture slots, factors and flags) share ONE table entry:
        // registration happens per primitive, so a scene with N primitives over M shared glTF
        // materials would otherwise append N records and burn the table needlessly. The key is
        // the byte-exact 80-byte record carried in a data_block - no hash collisions, because
        // the unordered lookup hashes the block only for bucketing while equality stays
        // byte-exact.
        // ---- THE LANES BESIDE THE RECORD ARE PART OF THE DEDUP KEY, and that is a fix rather than tidiness ----
        //
        // The record does NOT carry these lanes (see deren::engine::render_layout::heap_slots::toon_lanes), so two primitives whose
        // records are byte-identical but whose materials differ in their SDF or metallic/gloss map would COLLIDE
        // on the key below: the second would take the early return, never write its lanes, and that material would
        // silently lose the feature - with the frame showing nothing but a slightly wrong face. MEASURED BEFORE
        // THIS LINE EXISTED: an instrumented probe showed the SDF texture arriving at this function VALID
        // (1024x1024, 4 MB) and every material's written lane was nevertheless 0, which is exactly the signature
        // of the material that owns the map losing the race to one that does not.
        glm::uvec4 const toon_lanes_extra(texture_indices[toon_base + static_cast<std::size_t>(deren::engine::toon_slot::sdf_lightmap)],
                                          texture_indices[toon_base + static_cast<std::size_t>(deren::engine::toon_slot::metallic_gloss)],
                                          texture_indices[toon_base + static_cast<std::size_t>(deren::engine::toon_slot::sdf_mask)],
                                          texture_indices[toon_base + static_cast<std::size_t>(deren::engine::toon_slot::emotion)]);
        // THE SECOND BLOCK, whose remaining lane (`w`) is reserved: it exists because a fifth lane does not fit a
        // `uvec4`, and it is zeroed rather than left out so that a shader reading a lane nobody set reads "do not
        // read" - the same contract the first block's lanes follow.
        //
        // EACH COMPONENT IS NAMED BY THE LANE IT CARRIES RATHER THAN BY ITS POSITION, which is a correction the
        // rewritten chain's iris forced. This initialiser was `(split_normal, 0, 0, 0u)` under a comment saying the
        // rest were reserved, and a lane written NOWHERE is not a reserved lane: it is a lane that reads "do not
        // read" for every material that states it. MEASURED: with `goo_matcap05` at lane 9 the rewrite's iris
        // produced a frame BYTE-IDENTICAL to the one with no matcap lane at all (`CB321ADE1673AC9C` both ways)
        // while the sidecar reported `_GooMatcap05 -> texture #27 | ON` - the host never wrote the lane and the
        // shader read 0 (see `deren-ab/goo_step1_result.md`). `test_goo_toon_math` now names this line among the
        // lane's sync points, so the next lane cannot be added to the enum and forgotten here.
        glm::uvec4 const toon_lanes_extra2(texture_indices[toon_base + static_cast<std::size_t>(deren::engine::toon_slot::split_normal)],
                                           texture_indices[toon_base + static_cast<std::size_t>(deren::engine::toon_slot::goo_matcap05)],
                                           texture_indices[toon_base + static_cast<std::size_t>(deren::engine::toon_slot::goo_base_ramp)],
                                           texture_indices[toon_base + static_cast<std::size_t>(deren::engine::toon_slot::goo_face_sdf)]);
        // THE THIRD BLOCK, WHICH STEP 7 ADDED AND WHICH IS WHY `toon_lane_blocks` IS 3: the FACE container's three
        // masks are lanes 11..13, and lane 11 was the last free component of the block above. `w` is reserved and
        // zeroed, the same contract every other block follows ("a lane nobody set reads DO NOT READ").
        //
        // STEP 13'S `goo_rs_mask` TAKES `.z`, AND THE LANE WAS DEAD UNTIL IT DID. `toon_slot::goo_rs_mask` was
        // added to the enum, given a format in `register_material` and named in the application's vocabulary, and
        // the sidecar resolved the image -- and the frame did not move by one pixel, because THIS is where a slot
        // becomes a number the shader can see and this line still packed `0u`. The shader tests `block3.z != 0u`
        // before it samples (slot 0 is the white fallback, so zero means "do not read"), so the mask branch was
        // skipped for every material and `rs_eff` was zero. That is EXACTLY the failure this block's comment above
        // records for the matcap lane at step 1 ("the host never wrote the lane and the shader read 0"), and it is
        // the second time the enum-to-lane-table join has been the last edit missing rather than the first: an
        // enum value, a format table entry and a vocabulary row are all compile-visible, and a component of a
        // `glm::uvec4` is not.
        //
        // `test_goo_toon_math` pins this line by name, next to the matcap one that was pinned after step 1, so a
        // further lane cannot be added to the enum and forgotten here either.
        glm::uvec4 const toon_lanes_extra3(texture_indices[toon_base + static_cast<std::size_t>(deren::engine::toon_slot::goo_face_cm)],
                                           texture_indices[toon_base + static_cast<std::size_t>(deren::engine::toon_slot::goo_face_csumt)],
                                           texture_indices[toon_base + static_cast<std::size_t>(deren::engine::toon_slot::goo_rs_mask)],
                                           // STEP 15'S SHEET IS THIS BLOCK'S `.w`, and it is written HERE rather than
                                           // left at the `0u` it used to be for the reason the block comment below
                                           // gives at length: `0` is the shader's "do not read", so a slot packed as
                                           // `0u` reads as a material that states no sheet. The mask one component to
                                           // the left was left at `0u` for a whole step with the enum, the format row
                                           // and the vocabulary row all present, and the frame did not move.
                                           texture_indices[toon_base + static_cast<std::size_t>(deren::engine::toon_slot::goo_rs_sheet)]);
        // ---- AND THE COLOUR LANES, THE SAME FIX ONE TABLE FURTHER ALONG ----
        //
        // They are written BELOW, after the early return, which is the whole reason they have to be in the key:
        // the early return is "this material's row is already in the tables", and without these bytes that claim
        // was false for the six `vec4`s - the second of two record-identical materials read the FIRST one's
        // colours. MEASURED, not argued (see `remaining_port_spec.md`'s "材质去重键补上 colour lanes" section): a copy of
        // `M_actor_chen_hair_01` stating `_Specular = 0.0`, attached to a mesh whose node comes earlier in the
        // scene, took the earlier index; the hair itself then took this early return and rendered BYTE-FOR-BYTE
        // like an asset whose hair states 0.0 - its own 1.0 discarded.
        //
        // `info.toon.colours` IS the array the table is filled from, lane for lane and in the same order, so
        // keying exactly these bytes dedups precisely the materials whose six rows would come out identical -
        // a pair stating the same values still shares one entry (the control arm of that probe: 0 px).
        deren::utility::data_block<sizeof(deren::engine::material_record) + deren::engine::toon_lane_blocks * sizeof(glm::uvec4) + static_cast<std::size_t>(deren::engine::toon_colour_lane::count) * sizeof(glm::vec4)> material_key = {};
        std::memcpy(material_key.data.data(), &record, sizeof(record));
        std::memcpy(material_key.data.data() + sizeof(record), &toon_lanes_extra, sizeof(toon_lanes_extra));
        std::memcpy(material_key.data.data() + sizeof(record) + sizeof(toon_lanes_extra), &toon_lanes_extra2, sizeof(toon_lanes_extra2));
        std::memcpy(material_key.data.data() + sizeof(record) + sizeof(toon_lanes_extra) + sizeof(toon_lanes_extra2), &toon_lanes_extra3, sizeof(toon_lanes_extra3));
        std::memcpy(material_key.data.data() + sizeof(record) + sizeof(toon_lanes_extra) + sizeof(toon_lanes_extra2) + sizeof(toon_lanes_extra3),
                    info.toon.colours.data(),
                    static_cast<std::size_t>(deren::engine::toon_colour_lane::count) * sizeof(glm::vec4));
        if (auto const cached = this->material_slot_cache.find(material_key); cached != this->material_slot_cache.end()) {
            return cached->second; // already registered: share the existing record
        }
        if (this->material_count >= deren::engine::material_capacity) {
            // Table full (pathological - 16384 unique materials): degrade to the reserved
            // default material (index 0, white + identity factors, registered at setup) instead
            // of crashing; the mesh still draws. Logged once, not per registration.
            if (!this->material_overflow_logged) {
                this->material_overflow_logged = true;
                deren::utility::log("material table capacity ({}) exceeded - extra materials render with the default (index 0)", deren::engine::material_capacity);
            }
            return {};
        }
        uint32_t const material_index = this->material_count++;
        if (material_texture_edges.size() <= material_index)
            material_texture_edges.resize(material_index + 1);
        material_texture_edges[material_index].assign(texture_indices.begin(), texture_indices.end());
        std::memcpy(static_cast<uint8_t*>(this->material_mapped) + static_cast<size_t>(material_index) * sizeof(material_record), &record, sizeof(record));
        // THE LANES BESIDE THE RECORD, written here rather than into it, and this is the one place that knows both
        // the material's index and its lanes (see deren::engine::render_layout::heap_slots::toon_lanes for why the record cannot carry
        // them). They are MATERIAL-indexed, so the shader reaches them with the index it already uses for the
        // record.
        //
        // IT SITS AFTER THE DEDUP'S EARLY RETURN, deliberately and safely: that return is for a record ALREADY in
        // the table, whose lanes were written when it was appended. A lane of 0 - no map, or the artist's
        // `_UseSDFLightmap` / `_UseMetallicGlossMap` off, which collapse to the same value here exactly as they do
        // for the record's four - is the "do not read" the shader tests.
        static_cast<glm::uvec4*>(this->toon_lane_mapped)[static_cast<std::size_t>(material_index) * deren::engine::toon_lane_blocks] = toon_lanes_extra;
        static_cast<glm::uvec4*>(this->toon_lane_mapped)[static_cast<std::size_t>(material_index) * deren::engine::toon_lane_blocks + 1u] = toon_lanes_extra2;
        // ... AND THE THIRD BLOCK, for the reason the second one's own note gives: a lane the host never writes
        // reads "do not read" for every material that states it, which on the FACE would silently drop its SDF, its
        // `cm_M` and its brightness switch at once while the log still reported all three as ON (lane 11 is
        // `goo_face_sdf`, i.e. the component that closed the second block - see `toon_slot`).
        static_cast<glm::uvec4*>(this->toon_lane_mapped)[static_cast<std::size_t>(material_index) * deren::engine::toon_lane_blocks + 2u] = toon_lanes_extra3;
        // AND THE MATERIAL'S COLOURS, at the same index and in the same once-written spirit: the shader addresses
        // them with `material_index * toon_colour_lane::count + lane`, so the two sides' stride has to agree - see
        // `character_toon_colour_lanes` in the stage and the drift check in the sidecar test.
        //
        // AND THEY *ARE* PART OF THE DEDUP KEY ABOVE, which is a fix and not tidiness: they are written at this
        // index, once, and this paragraph used to say the opposite - that the key was the record plus the two
        // `uvec4` blocks alone, so two materials whose records AND texture lanes were byte-identical shared ONE
        // index here and therefore ONE set of colours, whichever registered first. That was latent on this
        // repository's assets (chen registers all seven of its materials separately - see the `toon: material N`
        // lines - because their records differ), but it governed all three per-material things that live ONLY in
        // these lanes: `toon_colour_lane::specular_strength`, `toon_colour_lane::parallax_scale`, and the outline
        // width in `toon_colour_lane::outline_edge`'s `.w` (which nothing else carries either). Fixed by keying
        // the six lanes' bytes beside the two blocks - see the `material_key` construction above and the note on
        // `material_slot_cache` in `runtime.declarations.cppm`.
        if (this->toon_colour_mapped != nullptr) {
            glm::vec4* const colours = static_cast<glm::vec4*>(this->toon_colour_mapped) + static_cast<std::size_t>(material_index) * static_cast<std::size_t>(deren::engine::toon_colour_lane::count);
            for (uint32_t lane = 0; lane < static_cast<uint32_t>(deren::engine::toon_colour_lane::count); ++lane) {
                colours[lane] = info.toon.colours[lane];
            }
        }
        // LOGGED WHILE THESE LANES ARE BEING WIRED, and the reason is that a lane which silently stays zero is
        // this table's only failure mode and it is invisible in the frame: the shader's test is `lane != 0`, so a
        // table that was never filled renders exactly like a model with no SDF and no metallic/gloss map at all -
        // the feature does not happen and nothing says why. THE SECOND BLOCK'S THREE NAMED LANES ARE PRINTED TOO,
        // for the same reason one table along: `_GooMatcap05` and `_GooBaseRamp` are lanes the REWRITTEN chain
        // gates on, and a zero here is the difference between "the asset states no ramp" and "the host resolved one
        // and never wrote it" - which is exactly the failure step 1 recorded (the iris rendered a black ball while
        // the sidecar line said `ON`).
        deren::utility::log("toon: material {} (family {}) -> lanes: sdf {}, metallic/gloss {}, face mask {}, split normal {}, goo matcap {}, goo base ramp {} of {} texture(s)",
                            material_index,
                            record.toon_family,
                            toon_lanes_extra.x,
                            toon_lanes_extra.y,
                            toon_lanes_extra.z,
                            toon_lanes_extra2.x,
                            toon_lanes_extra2.y,
                            toon_lanes_extra2.z,
                            this->owned_texture_views.size());
        // ... AND THE SPECULAR STRENGTH, WHICH IS THE ONE LANE WHOSE "NOTHING STATED" IS A SENTINEL RATHER THAN
        // THE TABLE'S NEUTRAL (`-1`, see `toon_colour_lane::specular_strength`), so a log line is the only place
        // the difference between "the asset said 0.0" and "no source spoke" is visible at all: both reach the
        // shader as a number, and one of them means the family table answers. Printed for every registered
        // material, because the value is per material and the interesting case is the one that DISAGREES with its
        // family - which is exactly the case a family table cannot show.
        deren::utility::log("toon: material {} -> specular strength {:.4f}{}",
                            material_index,
                            static_cast<double>(info.toon.colours[static_cast<std::size_t>(deren::engine::toon_colour_lane::specular_strength)].x),
                            info.toon.colours[static_cast<std::size_t>(deren::engine::toon_colour_lane::specular_strength)].x < 0.0f ? "  <- no source stated `_Specular`: the family table's number stands" : "");
        // ... AND THE PARALLAX DEPTH BESIDE IT, for the same reason one lane up and with one difference that makes
        // the line more useful rather than less: the stage's fallback for it is not a family number but its own
        // constant (`character_eye_parallax_depth`, 0.03), so `-1` here reads as "this material keeps the offset
        // the port had before the lane existed". That is the whole expected output for chen: a value on the three
        // materials whose `extras` state the row (0.03 iris / 0.5 brow / 0.5 cloth_01) and the sentinel on the rest.
        deren::utility::log("toon: material {} -> parallax depth {:.4f}{}",
                            material_index,
                            static_cast<double>(info.toon.colours[static_cast<std::size_t>(deren::engine::toon_colour_lane::parallax_scale)].x),
                            info.toon.colours[static_cast<std::size_t>(deren::engine::toon_colour_lane::parallax_scale)].x < 0.0f ? "  <- no source stated `_ParallaxScale`: the stage's own constant stands" : "");
        // ---- AND THE EMISSIVE LANE, WHICH THE TOON STAGE ADDS AND THEREFORE HAS TO BE ABLE TO TRUST ----
        //
        // `s.emissive` is `emissive_factor * the texture at emissive_index`, and index 0 is the WHITE FALLBACK
        // rather than "no emission" - so a material whose emissive map did not reach the record does not lose a
        // term, it gains `factor * 1`, i.e. the emission its artist stated applied at FULL STRENGTH over the
        // whole surface. That is the opposite of a missing feature and it is invisible in the log without this
        // line: measured on `chars\chen_full2.glb`, where the cloth's near-black emissive map (mean 0.3/255)
        // resolved in one file and not in the other, the difference is 30% of the frame.
        deren::utility::log("toon: material {} -> emissive: index {} factor ({:.3f}, {:.3f}, {:.3f}){}",
                            material_index,
                            record.emissive_index,
                            static_cast<double>(record.emissive_factor.x),
                            static_cast<double>(record.emissive_factor.y),
                            static_cast<double>(record.emissive_factor.z),
                            record.emissive_index == 0u && (record.emissive_factor.x != 0.0f || record.emissive_factor.y != 0.0f || record.emissive_factor.z != 0.0f)
                                ? "  <- FACTOR AGAINST THE WHITE FALLBACK: this material states emission but has no emissive map in the record"
                                : "");
        this->material_slot_cache.emplace(material_key, material_id{material_index});
        // THE TOON FAMILY, LOGGED WHEN IT IS NOT `none`, and this is the one place it can be logged once per
        // MATERIAL rather than once per primitive (the dedup above returns early for a shared record). A
        // classification is a DECISION about a string, and a wrong one is invisible in the frame: a face
        // material shaded with hair's ramp still looks like a toon character, just not like the reference's.
        // Printing the assignment at import is what makes a mis-classification a line to read instead of a
        // look to argue about. Silent for `none`, which is every non-character model and would be log spam.
        if (record.toon_family != 0u) {
            deren::utility::log("toon: material {} -> family {} (1=base 2=skin 3=face 4=hair 5=eye 6=cloth)", material_index, record.toon_family);
        }
        return material_id{material_index};
    }

} // namespace deren::engine
