// ============================================================================
// module: deren.engine.runtime:frames  - the frame pipeline
//
// THE FRAME: pace_and_acquire through render_frame, uninterrupted - the per-frame-slot phases, the
// recording of every stage, the pass plumbing that resolves and verifies them, the resource publication
// and the submit/present handshake. It was the largest single run left in runtime.cpp, and the reason
// that file was worth splitting at all.
//
// Imports are NOT transitive: this partition imports what its own code calls, and uses the shared vessel allocation service.
// ============================================================================
module;

#include <GLFW/glfw3.h>
#include <algorithm> // std::min in the resource publication
#include <array>
#include <bit> // std::bit_cast for the caster world-matrix hash
#include <chrono>
#include <cstdlib>
#include <cstring> // std::memcpy, for composing a pass's push block
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <span>   // std::as_bytes for the init_utils calls (the bytes behind a UBO or a zeroed table)
#include <thread> // std::this_thread::yield in the frame limiter

module deren.engine.runtime:frames;
import deren.utility.shared;

import :declarations;

import deren.engine.profiling;
import deren.engine.pipelines;
import deren.engine.render_resource;
import deren.engine.render_resource.shared;
import deren.engine.render_graph.bloom;

import deren.utility;
import deren.engine.frame_constants; // one frame's shared constants (see update_frame_constants)
import deren.promise.rhi;            // the contract's recording surface: the frame's list, image and read-back slot

namespace deren::engine {
    namespace {
        /// abi 36: bind a pipeline through whichever contract interface the session records into.
        void bind_pipeline_on(recording_target const& target, auto const& pipeline) noexcept {
            if (target.command != nullptr) {
                pipeline.begin_pipeline(*target.command);
            } else if (target.mergeable != nullptr) {
                pipeline.begin_pipeline(*target.mergeable);
            }
        }
    } // namespace

    // ---- THE BUFFER-CREATION HELPERS THE CONVERTED SITES SHARE (see their notes in :declarations) ----
    //
    // THE INITIAL BYTES ARE THE UPLOAD, and the contract's `initial_bytes` is where they travel: a
    // host-visible buffer the caller then writes through `mapped()` keeps an EMPTY span, which is the
    // descriptor's own "allocate only" (the backend keys its content registry on those bytes, so handing it
    // a span it never wrote would be a lie about the content). Only a buffer the caller already has the
    // bytes for - the vertex and index geometry - passes a non-empty one.

    // ... AND A SIZE SMALLER THAN THE UPLOAD IS NOT A BUFFER AT ALL, so it is a PANIC rather than a
    // truncation: every call site here passes either the upload's own size or a buffer it fills later
    // through `mapped()`, and a backend handed the first without the bytes would allocate a buffer nobody
    // described. The panic is loud on purpose - the alternative, a silently empty allocation, is the class
    // of failure this migration is removing.
    [[nodiscard]] rhi::buffer_desc make_buffer_desc(std::uint64_t const size, rhi::buffer_usage const usage, rhi::buffer_flags const flags, std::span<std::byte const> const initial_bytes) noexcept {
        if (initial_bytes.size() > size) {
            deren::utility::panic(std::source_location::current(), "buffer descriptor: {} initial bytes for a {} B buffer", initial_bytes.size(), size);
        }
        return rhi::buffer_desc{
            .size = size,
            .usage = usage,
            .flags = flags,
            .initial_bytes = initial_bytes,
        };
    }

    void create_buffer(rhi::api_core& device, rhi::buffer_usage const usage, rhi::buffer_flags const flags, std::span<std::byte const> const initial_bytes, std::string_view const what, rhi::object_ptr<rhi::buffer>& output, void** const mapped) {
        // THE FACTORY IS CALLED THROUGH THE CONTRACT INTERFACE, not through `core`: a call on the concrete
        // class emits an undefined symbol in this half that the backend defines (it JOINS the boundary
        // worklist), while a virtual call through `rhi::api_core&` loads the vptr and emits none. `core`
        // derives from `api_core`, so the cast is the upcast the interface was built for.
        //
        // THE OUTPUT IS THE OWNER, not the handle: an `object_ptr` is move-only and null-on-move, so the
        // reference the factory hands over is taken by the manager and released by ITS destructor - on every
        // path out, which is the whole reason the type exists.
        output = rhi::object_ptr<rhi::buffer>{static_cast<rhi::api_core&>(device).create_buffer(make_buffer_desc(initial_bytes.size(), usage, flags, initial_bytes))};
        if (!output) {
            deren::utility::panic(std::source_location::current(), "failed to create {}", what);
        }
        if (mapped != nullptr) {
            *mapped = output->mapped().data();
        }
    }

    void create_buffers(rhi::api_core& device, deren::utility::vessel::vector<rhi::object_ptr<rhi::buffer>>& outputs, rhi::buffer_usage const usage, rhi::buffer_flags const flags, std::span<std::byte const> const initial_bytes, std::string_view const what, deren::utility::vessel::vector<void*>* const mapped) {
        for (int32_t slot = 0; slot < static_cast<int32_t>(device.walk_frames()->slot_count()); ++slot) {
            outputs.emplace_back();
            void* slot_mapped = nullptr;
            create_buffer(device, usage, flags, initial_bytes, what, outputs.back(), mapped != nullptr ? &slot_mapped : nullptr);
            if (mapped != nullptr) {
                mapped->push_back(slot_mapped);
            }
        }
    }

    std::uintptr_t buffer_address(rhi::api_core& device, rhi::buffer const& buffer) noexcept {
        // THROUGH THE ABILITY POINTER, never through a concrete member: `query_extension` is a virtual on
        // `rhi::api_core`, and `buffer_address` is called on the `device_address` interface it answers with -
        // a call on the backend's own `buffer_address_view` member would emit a symbol into this half.
        auto* const addresses = static_cast<rhi::device_address*>(device.query_extension(rhi::extension_kind::device_address));
        // The ability answers 0 itself for a buffer that was not created with `buffer_flag::device_address`,
        // so a null ability is the only additional case here - and it is the contract's "this backend cannot
        // serve addresses", which every call site rules out by creating the buffer WITH that flag.
        return addresses == nullptr ? 0u : addresses->buffer_address(buffer, 0u);
    }

    std::uintptr_t runtime::buffer_address(rhi::buffer const& buffer) const noexcept {
        return deren::engine::buffer_address(this->vulkan_core, buffer);
    }

    bool runtime::write_heap_buffer(rhi::buffer const& buffer, uint32_t const slot, std::uint64_t const size, rhi::descriptor_type const type) const {
        return contract_write_heap_buffer(this->rhi_face(), deren::engine::render_layout::heap_slot_offset(slot), this->buffer_address(buffer), size, type);
    }

    rhi::frame_walker& runtime::frame_ring() const noexcept {
        // THE FACE, PER CALL SITE: walk_frames() answers this core's own borrowed view (the same
        // object every call) - a virtual call through `rhi::api_core&`, so no backend symbol joins
        // the boundary worklist for it (the same rule create_buffer's contract-interface note states).
        return *this->rhi_face().walk_frames();
    }

    std::shared_ptr<rhi::command_buffer> runtime::frame_command_buffer() const noexcept {
        // THE BORROWED CONTRACT HANDLE OF THE FRAME'S OWN BUFFER (abi 21): `begin_commands()` answers the
        // CORE's stable `commands_view` for the slot in flight, which is why the same object comes back
        // every call - and why the control block below must be NON-OWNING. `frame_commands::release()`
        // (the one drop a deleter could perform) refuses BY NAME, because the core owns the buffer and this
        // view never held a reference to drop; a deleter that called it would only draw that log.
        //
        // `nullptr` IS THE HONEST ANSWER OUTSIDE A FRAME: nothing is recording, so there is no buffer to
        // hand out - the state the removed `frame_primary_handle()` answered VK_NULL_HANDLE for, and the
        // state the frame loop's own begin/end guards already name.
        rhi::command_buffer* const list = this->rhi_face().begin_commands();
        if (list == nullptr) {
            return {};
        }
        return std::shared_ptr<rhi::command_buffer>(list, [](rhi::command_buffer*) noexcept {
            // NOTHING IS DROPPED, on purpose: the core owns the frame's buffer and its
            // `frame_commands::release()` refuses by name (see the note above).
        });
    }

    // THE FRAME OPEN'S CLASSIFIER (the error mechanism's verdict family): the one place that knows
    // what an open failure MEANS for the frame loop. Success stays success; the two usually-fatal
    // codes (a lost device, a dead host heap) become `fatal` - EXECUTED BY THE ENGINE, here in
    // pace_and_acquire, because the backend only reports; everything else rides as `pass_failure`
    // and the dispatch below decides skipped-vs-failed. A classifier that contradicts its outcome is
    // a panic (the lying-classifier rule) - which is why this routes through a verdict instead of a
    // bare if-chain on the code.
    [[nodiscard]] inline deren::utility::verdict classify_acquire(deren::promise::rhi::frame_open_info const& open) noexcept {
        using deren::utility::fatal;
        using deren::utility::pass_failure;
        using deren::utility::pass_success;
        if (open.result.code == rhi::error::ok) {
            return pass_success{};
        }
        if (open.result.code == rhi::error::device_lost || open.result.code == rhi::error::out_of_host_memory) {
            return fatal{open.result};
        }
        return pass_failure{open.result}; // out_of_date -> skipped + rebuild; the rest -> acquire_failed
    }

    frame_status runtime::pace_and_acquire() {
        this->cluster_retention_open = false;
        this->bloom_graph_error = rhi::error::ok;
        this->bloom_graph_pending = false;
        this->bloom_graph_pending_generations.fill({});
        this->post_graph_drew_frame = false;
        this->main_graph_frame = false;
        this->post_graph_output = {};
        deren::engine::profiling::cpu_phase_timer const phase_timer{this->cpu_timings, deren::engine::profiling::cpu_phase::pace};
        if (!this->shared_heap_ready)
            return frame_status::acquire_failed; // No frame may read a descriptor update whose rollback failed.

        // A zero-sized swapchain (a window that has not been sized yet, or was restored from
        // minimized into a 0-sized client area) has no valid attachments: recording would set a
        // 0-wide viewport - a VUID - and produce nothing. Skip the frame exactly like the
        // minimized case; nothing was acquired, so no semaphore is left pending.
        //
        // THE SWAPCHAIN'S OWN EXTENT, not the render extent, and the two are not interchangeable here:
        // this is the question "is there an image to record into at all", which the OUTPUT answers. A
        // render extent that rounded up to at least 1x1 (`render_extent` clamps there) would report a
        // usable frame for a 0-sized window, which is the image the acquire just failed on. The output
        // extent is the contract's (`swapchain::extent()`), cached by the engine and refreshed on every
        // successful rebuild - the same number the backend's own `swap_chain_extent` holds.
        if (this->presentation_extent().width == 0 || this->presentation_extent().height == 0) {
            return frame_status::skipped;
        }

        // Frame limit (set_max_fps): hold the frame until its deadline before touching the swapchain, so
        // the wait can never happen with an image acquired. yield() and not a timer: on a fast scene the
        // remaining slack is a few milliseconds at most, and an OS sleep at Windows' default 15.6 ms
        // granularity overshoots by more than the target period (a 144 Hz target would sag towards 60).
        // The deadline moves by exactly one period, so overrunning a frame resyncs to now instead of
        // accumulating debt that would be paid back as a burst.
        if (this->max_fps > 0.0) {
            auto const period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / this->max_fps));
            auto const now = std::chrono::steady_clock::now();
            this->next_frame_deadline += period;
            if (this->next_frame_deadline > now) {
                // Sleep to a millisecond before the deadline, then spin that last millisecond: sleeping
                // alone cannot hit a sub-millisecond target, and spinning alone reaches it but burns a
                // whole core doing nothing. Splitting it costs about a millisecond of spin per frame
                // (6% of a core at 60 fps) and still lands on the deadline to within microseconds.
                deren::utility::sleep_for_nanoseconds(std::chrono::duration_cast<std::chrono::nanoseconds>(this->next_frame_deadline - std::chrono::milliseconds(1) - std::chrono::steady_clock::now()).count()); // portable: Win32 waitable timer / POSIX nanosleep
                while (std::chrono::steady_clock::now() < this->next_frame_deadline) {
                    std::this_thread::yield();
                }
            } else {
                this->next_frame_deadline = now; // fell behind: resync, do not bank debt
            }
        }

        // Configuration is now applied. Create shared scene resources before the first frame's
        // light updates and resource publication, including scenes with no mesh primitives.
        // Later frames reuse the same resources and bindings.
        // Creation failure, including first-frame initialization, must precede acquisition.
        if (this->prepare_managed_resources() != rhi::error::ok)
            return frame_status::acquire_failed;
        this->prepare_scene_resources();

        // THE FRAME OPENS THROUGH THE CONTRACT (abi 13): the walker fuses today's prologue - wait this
        // slot's timeline, latch the slot's previous frame's GPU timings, acquire the next image - and
        // reports the DECISION, not a bare VkResult. The wait-then-acquire order (which guards both the
        // command buffer and the acquire semaphore: acquiring first could reuse a binary acquire
        // semaphore with pending operations, VUID-vkAcquireNextImageKHR-semaphore-01779) is the
        // backend's now, and so is the cursor: `open.frame.frame_index` is the one truth.
        rhi::frame_walker& frames = *this->rhi_face().walk_frames();
        rhi::frame_open_info const open = frames.wait_and_acquire();
        // 读归引擎: the LATCH (inside wait_and_acquire) collected the slot's completed frame - the
        // backend's job; this aggregation is the engine's. It runs on EVERY path below, the skipped
        // ones included, exactly as the pre-acquire position did.
        this->collect_gpu_timings();
        // THE CLASSIFIER IS THE ERROR MECHANISM WORKING (the verdict family, the error-mechanism
        // batch): `fatal` is EXECUTED BY THE ENGINE - the backend only reported (device_lost,
        // out_of_host_memory) - so a lost device ends the loop here instead of spinning acquire
        // failures; out_of_date rebuilds and skips; everything else is the acquire_failed the caller
        // already handles. This replaces the hand-written VkResult if-ladder.
        deren::utility::verdict const decided = classify_acquire(open);
        if (auto const* const fatal_case = std::get_if<deren::utility::fatal>(&decided)) {
            deren::utility::panic(deren::utility::vessel::format("frame open is fatal ({}): {}", std::to_underlying(fatal_case->reason.code),
                                                                 fatal_case->reason.message.empty() ? std::string_view{"no message"} : fatal_case->reason.message),
                                  fatal_case->reason.where);
        }
        if (auto const* const failure = std::get_if<deren::utility::pass_failure>(&decided)) {
            if (failure->failure.code == rhi::error::out_of_date) {
                // e.g. the window was resized: rebuild the swapchain and let the caller retry on the
                // next iteration - the same skip the zero-extent guard above returns.
                deren::utility::log("swapchain out of date, recreating");
                // THE REBUILD IS THE SWAPCHAIN'S OWN VERB (abi 14): the engine asks the presentation
                // surface it renders into, and only a generation that was ACTUALLY rebuilt
                // (`ok`; `not_ready` is a zero-sized window's deferred state) invalidates the
                // per-image state - the same condition the raw bool used to spell.
                // THE ENGINE'S PRESENTATION-IMAGE VIEWS GO FIRST (see release_frame_swapchain_views):
                // the rebuild destroys the images they were made over.
                this->release_frame_swapchain_views();
                rhi::swapchain* const surface = this->rhi_face().frame_swapchain();
                if (surface != nullptr && surface->recreate() == rhi::error::ok) {
                    this->on_swapchain_recreated();
                }
                return frame_status::skipped;
            }
            return frame_status::acquire_failed;
        }
        // WHAT STAYS ENGINE-SIDE IS THE POLICY (the classifier routes, the actions stay here):
        // rebuilding the swapchain means recompiling the renderer's targets, which is this object's
        // business and not the backend's.
        // THE SLOT THE FRAME RECORDS INTO, from the open's own report - the engine's local name for
        // the cursor the backend answered (every use below reads this one, not a second counter).
        uint32_t const frame_slot = open.frame.frame_index;
        this->current_image_index = open.frame.image_index;
        // wait_and_acquire succeeded: only this completed slot's former generation can be released/reset.
        this->submitted_resource_views[frame_slot] = {};
        this->frame_resource_clusters[frame_slot].clear();
        this->frame_graph_retention[frame_slot].clear();
        this->cluster_retention_slot = frame_slot;
        this->submitted_fallback_views[frame_slot] = {};
        // ONE LINE, ONCE, SAYING HOW THE MESHLET DISPATCHES REACHED THE GPU (docs/mesh_shaders.md step 3): a frame
        // has been recorded by now, so the two counters answer the question the frame itself cannot - "is the
        // indirect seam the one in use, or did something fall back to the direct call". That is the exact hole a
        // whole round fell through once (entry point resolved, buffer never bound), so it is printed rather than
        // inferred. Both zero means no meshlet dispatch has been recorded at all.
        {
            static bool logged_mesh_indirect = false;
            uint64_t const indirect_dispatches = this->mesh_indirect_dispatches.load(std::memory_order_relaxed);
            uint64_t const direct_fallbacks = this->mesh_indirect_direct_fallbacks.load(std::memory_order_relaxed);
            if (!logged_mesh_indirect && (indirect_dispatches != 0u || direct_fallbacks != 0u)) {
                logged_mesh_indirect = true;
                deren::utility::log("mesh indirect: {} meshlet dispatch(es) went through the INDIRECT entry point, {} through the direct call", indirect_dispatches, direct_fallbacks);
            }
        }

        // Write this frame's camera UBO into the paced slot's per-slot buffer. The heap's
        //    per-slot camera slot points at that slot's own buffer, so one memcpy is the whole
        //    camera update - there is no per-frame descriptor write to make.
        // The DISPLAY aspect, from the presentation extent the contract reports (the engine's cached
        // output extent, refreshed on every rebuild): the projection is not a per-pixel quantity, so
        // the render scale cancels out of it - but the output's own extent is the honest source (a scale
        // that rounds the two axes differently would otherwise tilt the projection by a fraction of a
        // pixel's worth of aspect).
        rhi::image_extent const output = this->presentation_extent();
        this->current_aspect = static_cast<float>(output.width) / static_cast<float>(output.height);
        this->current_ubo = make_orbit_camera_ubo(this->camera.yaw, this->camera.pitch, this->camera.distance, this->camera.target, this->scene_extent_radius, this->current_aspect);
        // Motion-vector support: the G-buffer computes its vectors from the UNJITTERED pair, and the
        // previous matrix is the one THIS swapchain image's history was rendered with (not simply the
        // last frame's: several images are in rotation, so the last frame's camera is not what that
        // history shows).
        this->current_ubo.view_proj_unjittered = this->current_ubo.proj * this->current_ubo.view;
        this->current_proj_unjittered = this->current_ubo.proj; // before the jitter below
        this->current_ubo.prev_view_proj = this->current_image_index < this->image_view_proj.size() ? this->image_view_proj[this->current_image_index] : this->current_ubo.view_proj_unjittered;
        // TAA jitter: offset the projection by a sub-pixel amount so consecutive frames sample the
        // image at different positions. The offset lands in the projection's z-row (the only place a
        // perspective matrix carries an NDC translation), in NDC units derived from pixels, and it is
        // part of `proj` - so geometry AND the lighting stage's depth reconstruction agree about where
        // each sample is. Both unjittered matrices above are already taken, so the jitter cannot leak
        // into a motion vector.
        //
        // ONE PIXEL HERE IS A RENDER PIXEL, which is why this divides by the render extent and not by the
        // swapchain's: the halton sequence is a sub-pixel pattern in the raster the samples land in, so at
        // half scale the same NDC offset would cover two output pixels and the resolve would be handed a
        // jitter twice the size of the pixel it is cancelling.
        if (this->taa_active()) {
            glm::vec2 const jitter_pixels = taa_jitter_offset(this->taa_jitter_index);
            this->current_ubo.proj[2][0] += jitter_pixels.x * 2.0f / static_cast<float>(this->render_extent().width);
            this->current_ubo.proj[2][1] += jitter_pixels.y * 2.0f / static_cast<float>(this->render_extent().height);
            this->taa_jitter_index = (this->taa_jitter_index + 1) % taa_jitter_count;
        }
        // The deferred lighting stage reconstructs world positions from the G-buffer depth, so its
        // inverse must be the projection actually used to render that depth (jitter included).
        this->current_inv_view_proj = glm::inverse(this->current_ubo.proj * this->current_ubo.view);
        // ... and only now is the frame's camera UBO complete: upload it.
        if (this->upload_scene_constant(frame_slot, render_resource::resource_id::camera_ubo, std::as_bytes(std::span(&this->current_ubo, 1))) != rhi::error::ok)
            deren::utility::panic("managed camera constants: upload to completed slot failed");
        // Same for the light UBO: copy the CPU-side light_state into THIS slot's own light
        // buffer. The slot was just paced (its previous submission completed) and the other
        // in-flight slot's set points at its own buffer, so this host write can never race a GPU
        // read - which is why set_shadow_enabled() / enable_shadows() only touch light_state and
        // never mapped memory directly.
        if (this->light_buffers.size() > static_cast<std::size_t>(frame_slot)) {
            // the light_count lane's y carries the exposure scale (pbr.frag / skybox.frag apply it
            // in linear space right before the tonemapper)
            // shadows follow the camera: refit the light frustum to this frame's camera before
            // the UBO upload (16 points + a few matrix multiplies per frame)
            this->update_shadow_frustum();
            this->light_state.light_count.y = this->exposure_scale;
            this->light_state.light_count.z = this->toon_steps;
            this->light_state.light_count.w = this->toon_softness;
            // Ray-traced sun shadows: composed HERE rather than in set_rt_shadows, because the light UBO
            // is rebuilt from light_state every frame and a later enable_shadows() (main.cpp calls it
            // after the settings are applied, which is where this flag was first lost) resets fields of
            // this struct. Recomposing it makes the flag authoritative - the lighting stage reads exactly
            // what the passes below will do this frame.
            this->light_state.rt_shadows = (this->rt_shadows && this->pass_ready("rt_shadow") && runtime_detail::ray_query_available_of(this->rhi_face())) ? 1.0f : 0.0f;
            // the furnace mode still wins: it is a verification mode that turns the sun off entirely
            this->light_state.sun_intensity = (this->furnace ? 0.0f : 1.0f) * this->sun_intensity;
            this->light_state.furnace_level = this->furnace ? 1.0f : 0.0f;

            // ---- clustered light culling (M5): this frame's grid + the view-depth range its
            //      exponential slices span, and the host-side clear of the per-cluster counters.
            //      The grid is derived from the swapchain extent (capped to the allocated tiles) and
            //      the depth range from the same scene bound the shadow fit uses, so every visible
            //      fragment lands in a real cluster. A degenerate projection (the first frames, before
            //      the swapchain has an extent) disables the pass for that frame: shade_surface() then
            //      loop every light, which is always correct - just slower.
            {
                this->cluster_tiles_x = std::min((this->render_extent().width + deren::engine::cluster_tile_size - 1) / deren::engine::cluster_tile_size, deren::engine::max_cluster_tiles_x);
                this->cluster_tiles_y = std::min((this->render_extent().height + deren::engine::cluster_tile_size - 1) / deren::engine::cluster_tile_size, deren::engine::max_cluster_tiles_y);
                glm::mat4 const base_proj = this->current_proj_unjittered;
                bool const degenerate = !(this->current_aspect > 0.0f) || std::abs(base_proj[2][2]) < 1e-6f;
                float cluster_near = 0.1f;
                float cluster_far = 1.0f;
                if (!degenerate) {
                    float const camera_near = base_proj[3][2] / base_proj[2][2];
                    float const camera_far = base_proj[2][2] * camera_near / (1.0f + base_proj[2][2]);
                    float const scene_far = glm::distance(glm::vec3(this->current_ubo.camera_pos), this->shadow_scene_center) + this->scene_extent_radius;
                    cluster_near = std::max(camera_near, 0.05f);
                    cluster_far = std::max(std::min(camera_far, scene_far), cluster_near * 2.0f);
                }
                bool const clustered = this->clustered_lights && this->pass_ready("cluster") && !degenerate && this->cluster_tiles_x > 0 && this->cluster_tiles_y > 0;
                this->light_state.cluster_grid = glm::vec4(static_cast<float>(this->cluster_tiles_x),
                                                           static_cast<float>(this->cluster_tiles_y),
                                                           static_cast<float>(deren::engine::cluster_slice_count),
                                                           clustered ? 1.0f : 0.0f);
                this->light_state.cluster_depth = glm::vec4(cluster_near,
                                                            cluster_far,
                                                            static_cast<float>(this->render_extent().width),
                                                            static_cast<float>(this->render_extent().height));
            }
            if (this->upload_scene_constant(frame_slot, render_resource::resource_id::light_ubo, std::as_bytes(std::span(&this->light_state, 1))) != rhi::error::ok)
                deren::utility::panic("managed light constants: upload to completed slot failed");
        }
        // Reset managed scratch after the completed-slot wait and after updating this frame's light facts.
        // No clustered dispatch means no count clear, matching the former feature-gated memset.
        deren::utility::vessel::array<render_resource::resource_id, 1> const preserved_counts = {render_resource::resource_id::cluster_counts};
        std::span<render_resource::resource_id const> const preserved = this->active_features().clustered ? std::span<render_resource::resource_id const>{} : std::span<render_resource::resource_id const>{preserved_counts};
        if ((this->frame_chain != nullptr && this->frame_chain->begin_frame(frame_slot, preserved) != rhi::error::ok) ||
            this->shared_resource_fallback.begin_frame(frame_slot, preserved) != rhi::error::ok)
            deren::utility::panic("managed resources: acquired frame slot is outside the prepared resource set");
        // THE HEAD FRAME RIDES THE SAME SLOT RULE, and for the same reason: it can change every frame on a model
        // whose head turns, so the paced slot gets its own copy and a frame in flight keeps reading the one it
        // started with. `head_state` is only ever written by set_head_basis, which is an arbitrary-time call -
        // this is where that value reaches the GPU, exactly as the light's does.
        if (this->head_buffers.size() > static_cast<std::size_t>(frame_slot)) {
            if (this->upload_scene_constant(frame_slot, render_resource::resource_id::head_ubo, std::as_bytes(std::span(&this->head_state, 1))) != rhi::error::ok)
                deren::utility::panic("managed head constants: upload to completed slot failed");
        }
        // Remember the paced slot: the caller's per-frame host writes (set_skin_matrices /
        // morph_scratch) land in this slot's buffers and are safe to make now that the slot's
        // previous submission has completed.
        this->active_slot = frame_slot;
        // ... and the frame's shared constants are THIS frame's from here on: the camera record, the light
        // UBO and the fitted scene bounds above are all final at this point, and every pass of the frame reads
        // the same numbers the shaders see (see deren.engine.frame_constants).
        this->update_frame_constants();
        this->cluster_retention_open = true;
        return frame_status::proceed;
    }

    void runtime::advance_motion_transforms() {
        if (this->bound_scene == nullptr || this->motion_mapped.empty()) {
            return;
        }
        uint32_t const slot = this->frame_ring().position();
        if (slot >= this->motion_mapped.size()) {
            return; // no buffer for this slot: nothing to publish, and the shader reads slot 0's set
        }
        auto* const published = static_cast<glm::mat4*>(this->motion_mapped[slot]);
        if (published == nullptr) {
            return;
        }
        for (scene_tree::scene_node const& root : this->bound_scene->roots) {
            scene_tree::visit_primitives(root, this->scene_transform, [this, published](scene_tree::scene_node const& node, glm::mat4 const& world) {
                uint32_t const index = node.primitive_leaf->motion_slot();
                if (index == scene_tree::no_motion_slot || index >= deren::engine::scene_motion_capacity) {
                    return; // instanced (filled at setup) or out of range: nothing to advance
                }
                // Publish what this leaf looked like one frame ago, THEN remember this frame's world
                // matrix - so the next frame publishes the matrix this frame is about to draw with.
                published[index] = this->motion_previous[index];
                this->motion_previous[index] = world;
            });
        }
    }

    void runtime::advance_motion_deformations() {
        if (this->skin_previous_mapped.empty() || this->skin_current.empty()) {
            return;
        }
        uint32_t const slot = this->frame_ring().position();
        if (slot >= this->skin_previous_mapped.size() || slot >= this->skin_current.size()) {
            return; // no buffer for this slot: nothing to publish, and the shader reads slot 0's set
        }
        auto* const published = static_cast<glm::mat4*>(this->skin_previous_mapped[slot]);
        auto const& current = this->skin_current[slot];
        if (published == nullptr) {
            return;
        }
        // History follows rendered frames, not the last use of this ring slot. Upload allocations
        // are write-oriented: both history and the upload fingerprint use CPU sources, never mapped reads.
        // Include a larger preceding block when this slot has a shorter one. Missing matrices are zero
        // after the first frame, matching the initially zero-filled current buffers; first-frame history
        // remains identity. A scene that never uploads skin matrices does no buffer copy at all.
        size_t const count = std::max(this->skin_previous.size(), current.size());
        this->skin_previous.resize(count, this->skin_history_started ? glm::mat4(0.0f) : glm::mat4(1.0f));
        if (count != 0) {
            std::memcpy(published, this->skin_previous.data(), count * sizeof(glm::mat4));
        }
        this->skin_previous.assign(current.begin(), current.end());
        this->skin_history_started = true;
    }

    frame_status runtime::begin_recording() {
        deren::engine::profiling::cpu_phase_timer const phase_timer{this->cpu_timings, deren::engine::profiling::cpu_phase::begin};
        rhi::api_core& vk = this->vulkan_core; // the heap calls below take the contract face itself, not a native handle
        if (this->bound_scene == nullptr) {
            deren::utility::panic("runtime::begin_recording() called before set_scene() bound a scene");
        }
        // THIS FRAME's resources, in the declaration's vocabulary: published before anything resolves, because
        // the per-swapchain-image views are this generation's, an alias (`scene_color`) is decided per frame, and
        // the lazily created shadow map exists only after a scene set does - so this is the first point where
        // "what exists right now" has an answer (see pass::resource_table and runtime::publish_frame_resources).
        this->publish_frame_resources();
        // Record the frame into this slot's command buffer (inline recording: no inheritance).
        // Open the frame's borrowed recording target through the portable lifecycle.
        std::shared_ptr<rhi::command_buffer> const recording_commands = this->frame_command_buffer();
        if (!recording_commands || recording_commands->begin_recording({}) != rhi::error::ok) {
            return frame_status::begin_recording_failed;
        }
        // THE HEAP IS NOT BOUND HERE, AND THE MEASUREMENT IS WHY: binding it takes the WHOLE command buffer, not
        // only the stages that map from it. With every heap mapping switched OFF - so nothing but these two calls
        // was heap state - all nine gate scenarios came back as the SAME frame (hash DC5F6D66428C26D8, mean 0.00
        // against the 88.1 of the unlit reference), validation SILENT: a set-based stage under a bound heap does
        // not read the set it was handed, it reads the heap, where its bindings were never mapped. So the heap
        // turns on for a frame whose EVERY stage is heap-based, and not one stage earlier - a pass cannot be
        // migrated on its own. EVERY STAGE IS HEAP-BASED NOW, which is why the bind below is unconditional and why
        // the mapping shim it was waiting for is gone rather than switched on.
        // THE HEAPS, BOUND ONCE FOR THE WHOLE FRAME (see descriptor_heap::record_bind): every stage is heap-native
        // now, so this is command-buffer state taken at the earliest point the frame has commands. THERE IS NO
        // PUSH HERE, and its absence is the design: the frame slot and the swapchain image travel INSIDE each
        // stage's own push block (see source/shaders/heap_slots.glsl), which is what replaced the mapping shim's pushed
        // index - the binding is per frame, the indices are per stage.
        if (contract_heap_ready(vk)) {
            contract_record_heap_bind(vk, *recording_commands);
        }
        // GPU pass timing: open this frame's timestamp range and take the first mark. Marks are
        // written in gpu_mark_id order from here on (see gpu_mark); opening the range outside any
        // rendering instance is required, and this is the first point of the frame where the
        // command buffer exists.
        // THE RANGE IS OPENED THROUGH THE CONTRACT'S RECORDING LIST (abi 14): the timing range and
        // its marks belong to the list that records the frame now, the device's capability is the
        // backend's answer (`unsupported`, the same one the profiler face gives), and a frame that is
        // not in flight has no list at all. The answer is read rather than dropped: `unsupported` is
        // the device's honest state and is already reported by gpu_timing_summary(), while any other
        // refusal means the range did not open and is worth a line.
        if (rhi::command_buffer* const frame_commands = this->rhi_face().begin_commands(); frame_commands != nullptr) {
            rhi::error const opened = frame_commands->begin_gpu_timing();
            if (opened != rhi::error::ok && opened != rhi::error::unsupported) {
                deren::utility::log("runtime: opening the GPU timing range was refused ({})", static_cast<std::uint32_t>(opened));
            }
        }
        this->gpu_mark(gpu_mark_id::frame_begin);

        // The constant 1x1x6 environment cube, written here and ONCE per target generation: this is the
        // frame's first command buffer, and the cube is sampled by the SKYBOX - which runs long before
        // the lighting stage - so any later point would leave the background of every
        // frame reading the previous contents. Two features share it, which is why the clear is NOT
        // gated on `furnace` any more: the furnace verification mode points the IBL bindings at it as
        // the constant environment (level 1.0, the same value the light UBO's furnace lane carries, so
        // the analytic answer and the environment agree by construction), and the unloaded-IBL path
        // uses it as the type-correct CUBE placeholder (see write_ibl_bindings) - a descriptor pointing
        // at an image nothing ever initialized is worse than one pointing at a neutral value.
        // THE CUBE IS THE ENGINE'S OWN (③-D/E A1.6): the handle is the contract's, and the guard is the
        // manager's validity - a fixed-size engine handle cannot answer "how many images do I hold", so the
        // vector-length test that stood here has no subject any more.
        if (!this->furnace_cube_ready && static_cast<bool>(this->furnace_cube_image)) {
            if (rhi::command_buffer* const frame_commands = this->frame_command_buffer().get(); frame_commands != nullptr) {
                // THE THREE STEPS RECORD THROUGH THE CONTRACT (undefined -> transfer_destination, the clear over
                // all six faces, transfer_destination -> shader_read): the layer count is 6 rather than the
                // recipes' single layer, and the handle is the CONTRACT image the escape used to derive a native
                // one from.
                rhi::subresource_range const six_faces{.base_mip = 0u, .mip_count = 1u, .base_layer = 0u, .layer_count = 6u};
                bool const to_transfer = frame_commands->barrier(rhi::image_barrier{.resource = this->furnace_cube_image.get(),
                                                                                    .from = rhi::image_use::undefined,
                                                                                    .to = rhi::image_use::transfer_destination,
                                                                                    .range = six_faces}) == rhi::error::ok;
                bool const cleared = to_transfer && frame_commands->clear_color_image(*this->furnace_cube_image, deren::utility::vessel::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}, six_faces) == rhi::error::ok;
                bool const to_sampling = cleared && frame_commands->barrier(rhi::image_barrier{.resource = this->furnace_cube_image.get(),
                                                                                               .from = rhi::image_use::transfer_destination,
                                                                                               .to = rhi::image_use::shader_read,
                                                                                               .range = six_faces}) == rhi::error::ok;
                if (!to_sampling) {
                    // NAMED AND NOT FATAL, exactly like the raw sequence this replaced: the cube keeps its
                    // DESCRIPTOR (write_ibl_bindings republishes the same address every frame), so a refusal
                    // leaves a stale environment rather than skipping the frame.
                    deren::utility::log("runtime: the furnace cube's clear was refused through the contract; the environment cube stays as it was");
                }
            }
            this->furnace_cube_ready = true;
        }
        // Debug overlay: begin a fresh ImGui frame once per rendered frame (after the acquire,
        // before any UI content is built; the actual draw is recorded at the end of
        // record_main_drawcalls() while the main rendering instance is still open).
        if (this->debug_gui_shown && this->debug_overlay != nullptr && this->debug_overlay->is_active()) {
            this->debug_overlay->new_frame();
        }

        // Accumulate scene-tree world transforms: every leaf's push.model = scene_transform *
        //     identity * local. With the default scene_transform (identity) this reproduces the
        //     old flat-list world matrices exactly; the scene transform adds programmatic
        //     whole-scene grouping on top.
        for (scene_tree::scene_node& root : this->bound_scene->roots) {
            scene_tree::update_world(root, this->scene_transform);
        }
        // ... and, in the same walk's shadow, the previous-frame world matrices that give TAA its
        // object motion: see the declaration for why this has to happen here and not at draw time.
        this->advance_motion_transforms();
        // ... and the joint matrices ONE FRAME AGO, which are the other half of a motion vector for a
        // DEFORMING mesh: the rigid walk above cannot see that a skinned vertex moved inside its own object
        // space, because that movement is not in any node's matrix. Runs here, immediately after the rigid
        // publication and therefore after the animation controller's own write window.
        this->advance_motion_deformations();
        // Collect the primitive leaves once (DFS over the whole scene): the shadow pass draws all
        // of them, the main pass draws the subset bound to each pipeline
        this->frame_leaves_buffer.clear();
        for (scene_tree::scene_node const& root : this->bound_scene->roots) {
            collect_leaf_primitives(root, this->frame_leaves_buffer);
        }
        // ---- AND THE OVERLAY LEAVES COME STRAIGHT BACK OUT OF THAT SET ----
        //
        // This is the ONE place the article's two framebuffer multiplies are separated from every ordinary
        // surface, and it is here rather than at draw time because of what the separation MEANS: an overlay
        // surface must be absent from the opaque list, the transparent list AND the shadow caster list, not
        // merely ignored by them. A mask left in the opaque set is shaded as a surface (it is drawn with
        // whatever pipeline the pass binds and occludes what is behind it), and one left in the caster set
        // throws a shadow of its own onto the face it exists to darken. Both are the same mistake - a mask
        // treated as geometry.
        //
        // HOW MUCH THAT COSTS, MEASURED WITH A CONTROL rather than quoted: `chars\chen_full2_control.glb` is
        // the same asset with the two overlay materials renamed so neither classifier claims them
        // (`make_overlay_control.py`), i.e. exactly the frame this port replaces. Against the port's own frame
        // it differs by 591 px / 0.06% at the standard pose, 846 / 0.08% at 35 degrees of yaw and 8444 / 0.81%
        // at the 0.45 close-up - small, because these two quads are shaped to the features they darken and
        // almost all of each one lands on the face, and the largest part of what differs is where a quad
        // protrudes past the silhouette onto the background. The magnitude is therefore NOT the argument for
        // this split; the three states above are, and the magnitude is what says the split is not a rewrite of
        // the frame either.
        //
        // THE ORDER INSIDE EACH GROUP IS PRESERVED, which is why this is a stable removal and not a sort: the
        // leaves' order is the scene tree's, and two overlay quads that overlap must keep composing the way the
        // asset authored them.
        this->frame_overlay.clear();
        for (primitive const* const leaf : this->frame_leaves_buffer) {
            if (leaf->overlay_kind != 0u) {
                this->frame_overlay.push_back(leaf);
            }
        }
        std::erase_if(this->frame_leaves_buffer, [](primitive const* const leaf) { return leaf->overlay_kind != 0u; });

        // Frustum culling for the main pass: build a BVH over every leaf that has a single
        //     world AABB (normal draw primitives, whose bounds follow push.model), then keep only
        //     the leaves inside the camera frustum. Instanced primitives spread over many
        //     transforms (no single AABB) and primitives without bounds are never culled. The
        //     shadow pass below still draws the full frame_leaves_buffer set so no caster is lost.
        //
        //     Two-level reuse: the BVH is rebuilt only when the scene changed (bvh_dirty), and
        //     when the camera also did not move the culled result is reused as-is (no rebuild, no
        //     frustum_cull). update_world() above rewrites the same world matrices each frame, so
        //     a non-dirty scene keeps identical world AABBs and the cached BVH stays valid.
        // local aliases into the per-frame state filled above (keeps the cull math unchanged)
        deren::utility::vessel::vector<primitive const*> const& frame_leaves = this->frame_leaves_buffer;
        float const aspect = this->current_aspect;
        camera_ubo const& ubo = this->current_ubo;
        deren::utility::vessel::vector<primitive const*> visible_leaves = this->frame_leaves_buffer; // fallback: no culling
        // THE CASTER SET, MINUS THE FRAME'S STATIC SURROUND (`primitive::environment`, i.e. the
        // `[render] background_glb` import - see `shadow_casters` in the class docs for why it must never
        // cast). Written once here and used at every place the list is filled, because the caster set is
        // built in four different branches below and a filter applied at three of them is a bug waiting for
        // the fourth.
        auto const fill_shadow_casters = [this](deren::utility::vessel::vector<primitive const*> const& source) {
            this->shadow_casters.clear();
            this->shadow_casters.reserve(source.size());
            for (primitive const* const leaf : source) {
                if (!leaf->environment) {
                    this->shadow_casters.push_back(leaf);
                }
            }
        };
        if (this->frustum_culling) {
            // camera identity: the orbit state that shapes the frustum
            bool const scene_changed_this_frame = this->bvh_dirty;
            deren::utility::vessel::array<float, 7> const key = {
                this->camera.yaw,
                this->camera.pitch,
                this->camera.distance,
                this->camera.target.x,
                this->camera.target.y,
                this->camera.target.z,
                aspect,
            };
            this->camera_moved = key != this->camera_key;

            if (scene_changed_this_frame || !this->cull_bvh.has_value()) {
                // scene changed: rebuild the BVH from current world AABBs (and drop stale leaves)
                deren::utility::vessel::vector<deren::utility::aabb_box<primitive>> boxes;
                boxes.reserve(frame_leaves.size());
                for (primitive const* leaf : frame_leaves) {
                    if (leaf->has_bounds) {
                        auto const [wmin, wmax] = leaf->world_aabb();
                        boxes.push_back(deren::utility::aabb_box<primitive>{.min = wmin, .max = wmax, .extra_data = const_cast<primitive*>(leaf)});
                    }
                }
                if (!boxes.empty()) {
                    this->cull_bvh.reset(); // destroy the old tree first (its leaves reference this scene)
                    auto make_result = deren::utility::bvh<primitive>::make(boxes);
                    if (make_result) {
                        this->cull_bvh = std::move(make_result).value();
                    }
                } else {
                    this->cull_bvh = std::nullopt;
                }
                this->bvh_dirty = false;
            }

            if (this->camera_moved || scene_changed_this_frame || this->cull_visible.empty()) {
                // camera moved or the scene changed: re-run frustum cull against the current BVH
                deren::utility::vessel::vector<primitive const*> visible;
                visible.reserve(frame_leaves.size());
                // leaves without bounds (instanced etc.) are always drawn
                for (primitive const* leaf : frame_leaves) {
                    if (!leaf->has_bounds) {
                        visible.push_back(leaf);
                    }
                }
                if (this->cull_bvh.has_value()) {
                    // the cull frustum is built from the UNJITTERED view-projection: a jittered one
                    // would re-cull (and occasionally flicker a leaf in or out) on every TAA frame
                    deren::utility::frustum const view_frustum = deren::utility::make_frustum(ubo.view_proj_unjittered);
                    auto const inside = this->cull_bvh->frustum_cull(view_frustum);
                    for (auto const* node : inside) {
                        visible.push_back(node->extra_data);
                    }
                }
                this->cull_visible = std::move(visible);
                this->camera_key = key;

                // ---- Shadow caster set (see shadow_casters in the class docs) ----
                // Built only when the camera moved or the scene changed (reusing the cached
                // cull_visible otherwise, like the main pass does). Two regimes split on scene
                // size:
                //  - SMALL scenes: every leaf goes into the shadow map. Caster culling is only an
                //    approximation (a caster can sit arbitrarily far up-light and its PARALLEL
                //    shadow column still lands in the view) and with few leaves the full depth
                //    render is cheap - take the exact path.
                //  - HEAVY scenes (tens of thousands of leaves): the camera-visible set plus the
                //    BVH culled against the SHADOW frustum itself. That frustum is refitted every
                //    frame to the camera view AND already merges every leaf whose shadow column can
                //    reach the view (update_shadow_frustum), so it contains the off-screen casters -
                //    like the wall behind the camera - that the previous "camera frustum shifted
                //    up-light by shadow_caster_extent" heuristic dropped, while staying far smaller
                //    than the whole scene.
                constexpr std::size_t full_scene_shadow_leaf_limit = 1500;
                if (frame_leaves.size() <= full_scene_shadow_leaf_limit) {
                    fill_shadow_casters(this->frame_leaves_buffer);
                } else if (!this->shadows_enabled) {
                    // no shadow frustum yet (enable_shadows() not called): the set is unused until
                    // the shadow pass records, so take the exact path instead of culling against a
                    // zeroed light matrix. enable_shadows() runs before the first frame in
                    // practice; if it does not, the camera moving refreshes this set.
                    fill_shadow_casters(this->frame_leaves_buffer);
                } else {
                    if (!this->shadow_heuristic_logged) {
                        this->shadow_heuristic_logged = true;
                        deren::utility::log("shadow caster culling: scene exceeds {} leaves - shadow casters are the camera-visible plus shadow-frustum sets",
                                            full_scene_shadow_leaf_limit);
                    }
                    fill_shadow_casters(this->cull_visible);
                    if (this->cull_bvh.has_value()) {
                        // every cascade's frustum: a caster that only shadows the far range must still
                        // be drawn, so the union over the cascades is the exact caster set (a cull per
                        // cascade is cheap against the BVH)
                        for (uint32_t cascade = 0; cascade < std::clamp(this->shadow_cascades, 1u, deren::engine::max_shadow_cascades); ++cascade) {
                            deren::utility::frustum const light_frustum = deren::utility::make_frustum(this->light_state.light_view_proj[cascade]);
                            auto const in_light = this->cull_bvh->frustum_cull(light_frustum);
                            this->shadow_casters.reserve(this->shadow_casters.size() + in_light.size());
                            for (auto const* node : in_light) {
                                primitive const* const leaf = node->extra_data;
                                if (!leaf->environment) {
                                    this->shadow_casters.push_back(leaf);
                                }
                            }
                        }
                        std::ranges::sort(this->shadow_casters);
                        this->shadow_casters.erase(std::ranges::unique(this->shadow_casters).begin(), this->shadow_casters.end());
                    }
                }
            }
            visible_leaves = this->cull_visible;
        } else {
            // culling disabled: the shadow pass draws every scene leaf (see shadow_casters)
            fill_shadow_casters(this->frame_leaves_buffer);
        }

        // Split the visible set into OPAQUE leaves (frame_visible: drawn first, depth write on,
        // in the parallel segments) and TRANSPARENT leaves (frame_transparent: alpha-blended,
        // depth write off, drawn last). Transparent leaves are sorted FAR -> NEAR from the
        // camera so overlapping blends compose back-to-front. The split re-runs every frame
        // (cheap: one pass over the visible set + sort of the usually-few transparent leaves);
        // with a static camera the input and thus the result are identical, so no extra cache.
        this->frame_visible.clear();
        this->frame_transparent.clear();
        this->frame_character.clear();
        {
            glm::vec3 const eye = ubo.camera_pos;
            auto const distance_to = [&eye](primitive const* const m) -> float {
                // squared world-space distance of the leaf to the eye. Use the center of the
                // world AABB (transform of the local bounds by push.model) so large or
                // skinned/instanced-with-world leaves sort by where they actually occupy
                // space, not by an arbitrary origin point.
                glm::vec3 center;
                if (m->has_bounds) {
                    auto const [wmin, wmax] = m->world_aabb();
                    center = (wmin + wmax) * 0.5f;
                } else {
                    // no single world AABB (e.g. instanced primitive: one leaf, many world
                    // transforms, push.model is identity): fall back to the model origin.
                    // The resulting order is a no-op for instanced leaves, which is fine -
                    // instances spread over space have no meaningful per-leaf depth anyway.
                    center = glm::vec3(m->push.model[3]);
                }
                return glm::dot(center - eye, center - eye);
            };
            for (primitive const* const m : visible_leaves) {
                (m->transparent ? this->frame_transparent : this->frame_visible).push_back(m);
                // ... AND THE TOON CHARACTER STAGE'S SUBSET, which is the opaque leaves minus the frame's
                // static surround: the character chain re-shades the same surfaces the scene pass drew, and
                // the background is the one thing in the frame that is NOT a subject (see `frame_character`).
                if (!m->transparent && !m->environment) {
                    this->frame_character.push_back(m);
                }
            }
            std::ranges::sort(this->frame_transparent,
                              [&distance_to](primitive const* const a, primitive const* const b) {
                                  return distance_to(a) > distance_to(b); // far first
                              });
        }
        return frame_status::proceed;
    }

    // Everything that decides whether a slot's shadow maps are still valid, folded into one 64-bit
    // fingerprint: the caster count (the set itself can change - heavy scenes BVH-cull the casters
    // per frame), each caster's world matrix, the uploaded skin matrices (a skinned mesh keeps a
    // constant push.model; its pose lives only in those) and the morph-scratch revision. It is XXH3
    // rather than a byte loop because it runs on every frame, the reused ones included: 0.99 us
    // against 15.5 us for Sponza's 300 casters. The fold is boost::hash_combine's mix - the values
    // are hashed rather than compared, so the mix carries the collision resistance.
    uint64_t runtime::shadow_geometry_signature() const {
        auto const fold = [](uint64_t const accumulator, uint64_t const value) {
            return accumulator ^ (value + 0x9e3779b97f4a7c15ull + (accumulator << 6) + (accumulator >> 2));
        };
        uint64_t signature = static_cast<uint64_t>(this->shadow_casters.size());
        for (primitive const* caster : this->shadow_casters) {
            uint64_t const matrix = deren::utility::xxh3_64bits({reinterpret_cast<uint8_t const*>(&caster->push.model), sizeof(glm::mat4)});
            signature = fold(signature, matrix);
        }
        return fold(fold(signature, this->skin_matrix_hash), this->morph_revision.load(std::memory_order_relaxed));
    }

    void runtime::record_main_drawcalls() {
        deren::engine::profiling::cpu_phase_timer const phase_timer{this->cpu_timings, deren::engine::profiling::cpu_phase::scene};
        if (!this->frame_command_buffer()) {
            return; // no frame is open (see begin_recording): there is nothing to record into
        }
        uint32_t const frame_slot = this->frame_ring().position();
        // FREEZE THE GRAPH FRAME'S ELIGIBILITY HERE, before the first recording decision that depends on it: the
        // cluster sort is recorded by the composed graph on every frame;
        // and that decision comes before record_scene() (which used to be the only place the predicate ran).
        // Every later reader - record_scene(), the fallback log - uses this frozen value.
        this->main_graph_frame = this->main_graph_candidate();
        this->graph_cluster_frame = this->main_graph_frame && this->cluster_resources_available();
        // THE SHADOW PASS'S OWN RECORDING DECISION, measured here because the graph lane may hold it: the pass runs
        // only when a shading stage reads the map AND this slot's maps are stale (the reuse test below). The
        // signature and the reuse flag are what the code later uses, so they are computed once here, before the
        // freeze, and the freeze itself only adds the graph's conditions.
        render_features const features = this->active_features();
        uint64_t const geometry_signature = this->shadow_geometry_signature();
        bool const shadow_reuse =
            this->shadow_rendered_version[frame_slot] == this->shadow_content_version && this->shadow_rendered_models[frame_slot] == geometry_signature;
        this->graph_shadow_frame = this->main_graph_frame && features.shadow && !shadow_reuse && this->shadow_allocated_layers != 0 &&
                                   this->shadow_pass[0] != nullptr && frame_slot < this->shadow_images.size() &&
                                   static_cast<bool>(this->shadow_images[frame_slot]);
        // THE TAA RESOLVE'S LANES: reachable once the eligibility predicate lets a TAA frame through (that flip is
        // the next step); the resolve and its history copy are two graph passes because the model allows one
        // access per resource per pass, and the history's cross-frame state rides taa_history_in_transfer_state.
        this->graph_taa_frame = this->main_graph_frame && this->taa_active() && this->taa_pass[0] != nullptr;
        // THE OPAQUE LANE'S FREEZE (visibility-buffer v0.5): the switch, this frame's admission clause and the two
        // pipelines, folded ONCE so the record step and the fallback line cannot disagree about THIS frame. The
        // verdict is the pure vocabulary of deren::engine::visibility (the runtime's scan answers the facts); the
        // lane is then one function of (switch, verdict). A refused or disabled frame records the GEOMETRY lane for
        // every leaf - there is no partial frame.
        this->visibility_admission_frame = this->visibility_frame_admission(this->main_graph_frame);
        this->visibility_lane_frame = deren::engine::visibility::choose_opaque_lane(this->visibility_buffer_on, this->visibility_admission_frame);
        if (this->visibility_buffer_on && this->visibility_lane_frame == deren::engine::visibility::opaque_lane::geometry && !this->visibility_fallback_logged) {
            this->visibility_fallback_logged = true;
            deren::utility::log("visibility buffer: frame NOT admitted ({}), so the geometry lane records it - the fallback is whole-frame and announced once per session",
                                deren::engine::visibility::describe(this->visibility_admission_frame));
        }
        // THE G-BUFFER DEBUG VIEW'S LANE: a debug frame records through the graph with the debug view INSTEAD of the
        // lighting stage (the view wins over lighting), so the eligibility asks for the G-buffer pass and this flag
        // says which producer the graph records.
        this->graph_debug_frame = this->main_graph_frame && !this->deferred_lit_active() && this->gbuffer_pass_active() &&
                                  this->gbuffer_debug_stage[0] != nullptr;
        // THE RAY-TRACED SHADOW LANE (RT stage 2b): the lane declares the visibility image's storage write from the
        // ray-tracing stage and the lighting lane declares the sampled read, so the graph owns the write -> hand-off
        // pair the removed legacy record issued. The pass records only when its feature is on and its pipeline
        // exists.
        this->graph_rt_frame = this->main_graph_frame && this->active_features().rt_shadow && this->rt_shadow_pass[0] != nullptr;
        // THE GRAPH IS THE ONLY RECORDER NOW (cleanup after the flip): an ineligible frame records NOTHING
        // here - no legacy cluster sort, no legacy shadow pass, no legacy scene - and record_post_process
        // reports the refusal once (see main_graph_candidate's readiness conditions).
        if (!this->main_graph_frame) {
            return;
        }

        // ---- Clustered light culling (M5): one compute dispatch before anything renders. It sorts
        //      the punctual lights into the frame's cluster grid, and the shading stages (forward
        //      inside the main instance, deferred later in the frame) then loop only their own
        //      cluster's list. Runs before the shadow pass so the barrier that publishes its buffers
        //      is as early as possible; nothing before it reads the lists.
        // ON A GRAPH FRAME THE COMPOSED GRAPH OWNS IT (adoption stage 2G): its buffers are imported and
        // declared there, the graph orders the sort against the lighting pass that reads the bins, and its
        // recorder uses the pass's barrier-free entry, and this is the only site that records the sort.
        // whose cluster buffers did not resolve keeps the pre-migration split (this stage records it).

        // ---- Ray-traced shadows: build the acceleration structures once, before the passes that will
        //      trace against them. It happens HERE (inside the frame's command buffer, before any
        //      rendering instance opens) because a build is a transfer/compute-class command that must
        //      not be recorded inside vkCmdBeginRendering, and because the caster set is only complete
        //      now that the scene is loaded and culled. Nothing reads the structures yet, so a frame
        //      with the flag on renders exactly like one with it off - what this records is the input
        //      the ray-traced pass will need, not a change to the image.
        //
        // THE WORK IS THE STRUCTURE SET'S (deren.engine.ray_tracing) and the POLICY is this loop's: WHETHER the
        // structures are wanted at all (`rt_structures_wanted`), and that they are built and re-instanced HERE -
        // before any rendering instance opens. What the set returns is why it gave up; what that means for a knob
        // (the refit's own failure turns `rt_skin_bake` off) is decided here.
        if (this->rt_structures_wanted()) {
            ray_tracing::build_inputs const inputs = this->make_structure_inputs();
            // structures_frame_slot, not frame_slot: the outer frame_slot is in scope; MSVC /W4 C4456, an error under /WX.
            uint32_t const structures_frame_slot = this->frame_ring().position();
            // THE CONTRACT BUFFER IS WHAT THE STRUCTURE SET RECORDS THROUGH (the tenth batch): its two
            // build-ordering barriers are the contract's role pairs now, so it takes the contract buffer and
            // derives the native handle it still needs for the allocated build entry points itself.
            if (auto const built = this->structures.build(*this->frame_command_buffer(), inputs); !built) {
                deren::utility::log("ray-traced shadows disabled: {}", built.error().message);
                if (built.error().disable_skin_bake)
                    this->rt_skin_bake = false;
            }
            // ... and the top level structure, which is rebuilt EVERY frame: the instance set is culled per
            // frame and a caster's world matrix can change (animation, a moved node), so the instance list
            // is frame data like any other. On the frame that builds the bottom levels it runs right after them;
            // from the next frame on it is the structure a shadow ray will traverse.
            if (auto const updated = this->structures.update(*this->frame_command_buffer(), structures_frame_slot, inputs); !updated) {
                deren::utility::log("runtime: {}", updated.error().message);
                if (updated.error().disable_skin_bake) {
                    this->rt_skin_bake = false;
                }
            }
            // THE TOP LEVEL STRUCTURE'S HEAP SLOT follows the slot's structure, which is why this write is here and
            // not in the pass that reads it: a heap range must carry a real size and the structure's device address,
            // and both exist only once the structure for that slot does.
            // ONLY WHEN THE HANDLE CHANGES (see rt_binding_written): the unconditional write invalidated a frame that
            // was still in flight, which the validation layer reports as "VkDescriptorSet ... was destroyed or
            // updated without UPDATE_AFTER_BIND" followed by every later call on that command buffer failing.
            // THE HANDLE IS A TIER-1 OBJECT NOW (abi 26), which is also what replaces the entry point that used to
            // be resolved here to read the TLAS address out of it (see write_rt_structure_binding: it asks
            // `device_address()` / `size_bytes()`).
            deren::promise::rhi::acceleration_structure* const tlas = this->structures.handle(structures_frame_slot);
            if (structures_frame_slot < this->rt_binding_written.size() && this->rt_binding_written[structures_frame_slot] != tlas) {
                this->write_rt_structure_binding(tlas, structures_frame_slot);
                this->rt_binding_written[structures_frame_slot] = tlas;
            }
            // THE BUILD -> TRAVERSAL EDGE (RT stage 1): a structure the BUILD wrote must be visible to the shaders
            // that traverse it. Everything the structures' own recorders emit is a BUILD -> BUILD ordering (see
            // ray_tracing.cpp's `bake_order` / `build_order`); neither edge orders the build against the RT shadow
            // pass's traceRays launch, which reads THIS frame's top-level structure. The hint is left `none` on
            // purpose - the conservative shader mask covers the ray-tracing stage, and a batch that names no
            // reader costs one no-op memory dependency.
            deren::promise::rhi::acceleration_structure_barrier const traversable{
                .resource = tlas,
                .from = deren::promise::rhi::acceleration_structure_use::build_write,
                .to = deren::promise::rhi::acceleration_structure_use::shader_read,
            };
            deren::promise::rhi::barrier_group const traversable_group{
                .struct_size = sizeof(deren::promise::rhi::barrier_group),
                .images = {},
                .buffers = {},
                .stage = deren::promise::rhi::stage_hint::none,
                .has_memory = false,
                .memory = {},
                .acceleration_structures = std::span<deren::promise::rhi::acceleration_structure_barrier const>{&traversable, 1},
            };
            if (this->frame_command_buffer()->barrier(traversable_group) != deren::promise::rhi::error::ok) {
                deren::utility::log("ray-traced shadows: the build-to-traversal barrier was refused; shaders may read an unpublished structure");
            }
        }
        // GPU timing: the structures' builds end here. Written UNCONDITIONALLY, like every other mark -
        // a frame that skips a pass still writes its mark next to the previous one (0 ms interval), and
        // the report's labels are positional: leaving a gap here relabeled the whole frame ("marks
        // recorded out of order", which the harness caught on all seven scenarios at once).
        this->gpu_mark(gpu_mark_id::rt_build_end);

        // ---- Shadow pass: render the scene's depth from the light into this slot's shadow map.
        //      Drawn before the main pass; the depth-only pipeline shares the flat scene layout
        //      and the primitive draw() path (same vertex buffers / push constants), so the shadow
        //      pass is just "bind the shadow pipeline, then draw the same models".
        //      Depth-only rendering (no color attachment) needs dynamic rendering, which is core
        //      1.3 - the only path the engine supports.
        //
        //      Stage 2 of parallel recording: the shadow content is recorded into this slot's
        //      shadow SECONDARY command buffer first, then executed from the primary inside the
        //      shadow rendering instance (VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT).
        //      The recording is still sequential on the primary thread - rendering is identical
        //      to inline; stage 3 fans the recording out over the task pool.
        // Feature registry (runtime::active_features): the pass runs only when a shading stage will
        // actually read the map - the flat render mode skips it entirely, which is worth ~45% of the
        // frame (see the measured numbers in the struct's documentation).
        // (`features`, the geometry signature and `shadow_reuse` are computed with the frame's eligibility freeze:
        // the graph lane's membership depends on the same reuse test.)
        // Skip the whole pass when this slot's maps are still valid: unchanged fit (content version)
        // AND unchanged caster geometry (hash). The maps stay in SHADER_READ_ONLY and the descriptor
        // set still points at them, so there is nothing to record - not even the read barriers.
        if (features.shadow && !shadow_reuse) {
            deren::engine::profiling::cpu_phase_timer const shadow_timer{this->cpu_timings, deren::engine::profiling::cpu_phase::shadow}; // the sub-phase of scene that records every cascade
            if (static_cast<bool>(this->shadow_images[static_cast<std::size_t>(frame_slot)])) {
                // Secondary: inherit only the depth attachment (dynamic rendering 1.3). The
                // shadow map is single-sampled; viewMask 0 = no multiview. The rendering
                // inheritance struct hangs off VkCommandBufferInheritanceInfo::pNext (NOT the
                // begin-info pNext), and a secondary buffer must always provide inheritance info.
                // One secondary PER CASCADE: the caster content is identical, but each cascade pushes
                // its own index (a secondary records that itself - state is not inherited from the
                // primary), and a buffer without SIMULTANEOUS_USE may not be executed twice in one
                // primary anyway. The content is only the leaves/pipelines; the per-cascade difference
                // is that single push.
                // ---- THE SHADOW PASS RECORDS IT (deren.engine.pass.shadow) ----
                // What stays HERE is what the pass cannot know: the reuse test above (which is why this whole block
                // is inside it), the map's OWN image and its layer count (the hand-back below covers every
                // allocated layer, including the spare ones), and the bookkeeping that makes the next frame's reuse
                // test true. The frame itself - the per-cascade secondaries, the map's edge and the two callbacks -
                // is built HERE and handed to the pass by whoever owns it (see make_shadow_frame/prepare_stage).
                // ON A GRAPH FRAME THE COMPOSED GRAPH OWNS THE RECORDING (adoption stage 2H): its lane declares the
                // active cascade layers as written and the lighting read as the consumer, so the graph emits the
                // per-layer transitions AND the hand-back this block would issue - and one submission never runs
                // both barrier paths. The bookkeeping below still runs: it is what makes the NEXT frame's reuse
                // test true, graph-recorded or not.
                this->shadow_rendered_version[frame_slot] = this->shadow_content_version;
                this->shadow_rendered_models[frame_slot] = geometry_signature;
            }
        } else if (features.shadow) {
            // Reuse: the maps were rendered into this slot by an earlier frame and nothing that feeds
            // them has changed since, so the sampling layout they are already in is the one the
            // lighting pass needs. Deliberately does nothing.
        } else if (this->pass_ready("shadow") && this->shadow_images.size() > static_cast<std::size_t>(frame_slot)) {
            // The pass does not run this frame (shadows toggled off, or no light setup yet), but the
            // scene set still binds the shadow map to binding 8 - pbr.frag uses it statically and only
            // decides at runtime whether to sample it - and a sampled descriptor must point at an
            // image that is in the layout the descriptor declares. Leaving the map in UNDEFINED made
            // every shadow-off frame a VUID ("expects ... SHADER_READ_ONLY_OPTIMAL ... current layout
            // is UNDEFINED" - the message is quoted from a run made before VK_KHR_unified_image_layouts
            // collapsed every layout to GENERAL, which is what the driver expects now). Contents do not
            // matter (the shader returns "fully lit"), hence UNDEFINED as the old layout.
            if (static_cast<bool>(this->shadow_images[static_cast<std::size_t>(frame_slot)])) {
                if (rhi::command_buffer* const frame_commands = this->frame_command_buffer().get(); frame_commands != nullptr) {
                    // undefined -> depth_read. Every layer the image OWNS, not just layer 0: the constant's
                    // range is single-layer and the whole array view the descriptor covers must be sampleable.
                    // The count is shadow_allocated_layers - the layer count the image was actually created
                    // with - and NOT max_shadow_cascades: the two differ whenever fewer cascades are active
                    // than were ever allocated, and a range reaching past the image's own layer count is a
                    // VUID on every shadow-off frame.
                    (void)frame_commands->barrier(rhi::image_barrier{.resource = this->shadow_images[static_cast<std::size_t>(frame_slot)].get(),
                                                                     .from = rhi::image_use::undefined,
                                                                     .to = rhi::image_use::depth_read,
                                                                     .range = rhi::subresource_range{.base_mip = 0u, .mip_count = 1u, .base_layer = 0u, .layer_count = this->shadow_allocated_layers}});
                }
            }
        }

        // GPU timing: the shadow pass (and its hand-back barrier) ends here. The pass is optional,
        // so a frame without shadows just writes this mark next to frame_begin and reports ~0 ms.
        this->gpu_mark(gpu_mark_id::shadow_end);

        // The scene pass: the opaque leaves write the G-buffer, and everything else follows from it
        // in record_scene_tail (lighting, the transparent pass over the shaded image, TAA).
    }

    // =============================================================================================
    // THE SHADOW PASS's content callback and its scheduler (deren.engine.pass.shadow)
    // =============================================================================================
    //
    // THE RESOLVER THAT USED TO BE HERE IS GONE (S3.8): the layers this frame renders were a hand-written
    // `resolve_shadow_pass` because the declaration could name only ONE layer of the map, and the vocabulary that
    // replaced it is a RUN of elements (`render_target::count`) - the declaration now claims every cascade layer
    // the map can hold and the resource table answers how many it HAS this frame, which is the cascade knob's
    // (`create_shadow_resources`). What is left here is what the pass genuinely cannot know: which secondaries to
    // record into, and what a caster's draw state is (the scene block, the live depth-bias state, the two-sided
    // policy, the secondary's own begin info) - so that arrives as a callback, and the map's edge with it.
    bool runtime::record_shadow_cascade(void* const owner, rhi::mergeable_command_buffer_ref const& secondary, uint32_t const cascade_index, rhi::pipeline* const pipeline, bool const mesh_stage, bool const meshlets) {
        runtime* const self = static_cast<runtime*>(owner);
        // The secondary inherits ONLY the depth attachment (no colour one): dynamic rendering 1.3, single-sampled,
        // viewMask 0 - and the DESCRIPTOR HEAPS, which the BACKEND derives from its own heap state when it
        // begins the recorder (see core::owned_command_buffer::begin_recording). That derivation is why the
        // hand-written `VkCommandBufferInheritanceDescriptorHeapInfoEXT` that stood here is gone: this
        // secondary is validated on its own, and the shadow shaders read the light matrices and the shadow map
        // straight out of the heaps, but the heap is the BACKEND's state and this layer has no business
        // repeating it (VUID-vkCmdDrawIndexed-None-11308 is what a missing one produces).
        // THE HEADER IS THE STRUCTURE'S OWN DEFAULT (no `.header = {}`): an explicit `= {}` would VALUE-INITIALISE
        // the header and take `structure_header`'s own defaults (`s_type = unknown`, its own size), which is not
        // the tagged type this chain entry must declare - the backend refuses a chain whose `s_type` is not
        // `vulkan_command_buffer_inheritance` (validate_structure), and the symptom is a secondary that never
        // begins. Omitting the member is what keeps the NSDI's `{vulkan_command_buffer_inheritance, sizeof(...)}`.
        rhi::command_buffer_inheritance_info const inheritance = {
            .color_format_count = 0,
            .color_formats = nullptr, // depth-only: the shadow map is the one attachment
            .depth_format = rhi::image_format::depth,
            .samples = 1u,
            .view_mask = 0,
        };
        rhi::command_buffer_begin_info const begin = {
            .struct_size = sizeof(rhi::command_buffer_begin_info),
            .usage = rhi::to_bits(rhi::command_buffer_usage::render_pass_continue),
            .next = &inheritance.header,
        };
        if (secondary->begin_recording(begin) != rhi::error::ok) {
            deren::utility::log("runtime: shadow secondary command buffer begin failed - cascade {} skipped this frame", cascade_index);
            return false;
        }
        // which cascade these casters are projected into: the vertex stage indexes the light UBO's matrix array with
        // it, and a secondary records that itself (state is not inherited from the primary). The offset is the
        // declaration's own - where the scene's push block ends.
        uint32_t const index = cascade_index;
        // PUSHED AS DATA, not as a push constant: the pipeline has no layout any more (see
        // render_environment::push_block). This is ONE 4-byte slice of the shadow stage's block - the cascade
        // index's own offset - while the rest of the block is the material's, pushed per caster below. A
        // secondary records its own state (nothing is inherited from the primary), which is why it is set here.
        // THE ABILITY'S OWN VERB (abi 22's escape-shrink batch): the push was the ONE thing this function derived a
        // native handle for, so that derivation is gone with it - `descriptor_heap::push_data` takes the CONTRACT
        // buffer the cascade is being recorded into.
        rhi::descriptor_heap* const heap = rhi::query_extension<rhi::descriptor_heap>(self->rhi_face());
        rhi::heap_push_info const push_info{.offset = render_resource::shadow_io.push->offset, .data = std::as_bytes(std::span(&index, 1)), .mergeable_commands = secondary.get()};
        [[maybe_unused]] bool const pushed = heap != nullptr && heap->push_data(push_info) == rhi::error::ok;
        // THE SESSION'S BUFFER IS THE CASCADE'S OWN SECONDARY, handed over as a NON-OWNING view: the runtime owns
        // those buffers (`shadow_recording`) and this callback only records into one of them for the duration of the
        // call, so the control block must drop nothing.
        self->record_shadow_content(recording_target(secondary), pipeline, mesh_stage, meshlets);
        // THE END IS THE CONTRACT'S MATCHING HALF, and it is checked: `end_recording()` on this buffer is the
        // contract's verb (the buffer is CALLER-owned, unlike the frame's own), and a failed end means the
        // secondary is not executable - which is a cascade this frame must not execute (that is a VUID), so
        // the false answer skips it rather than recording a broken execution.
        if (secondary->end_recording() != rhi::error::ok) {
            deren::utility::log("runtime: shadow secondary command buffer end failed - cascade {} is not executable this frame", cascade_index);
            return false;
        }
        return true;
    }

    void runtime::run_shadow_tasks(void* const owner, std::span<std::function<void()>> const tasks) {
        static_cast<runtime*>(owner)->run_tasks(tasks, deren::engine::task_priority::recording);
    }
    bool runtime::main_graph_candidate() const noexcept {
        // THE FRAME IS GRAPH-RECORDED WHENEVER THERE IS A SCENE TO RECORD. Everything else the graph needs - the post
        // chain, the overlay lanes, the ray-traced shadow's pipeline - is the graph's OWN readiness question:
        // record_post_graph refuses a lane whose pass did not build and end_recording fails the frame on that error.
        // The one condition that leaves nothing to record is a missing G-buffer pipeline, and that frame is NOT failed:
        // record_post_process defines it with the background colour (a pipeline that failed to build is a startup log
        // line, not an undefined frame - and a headless runtime with no chain at all must still advance frames). The
        // per-feature exclusions that used to live here existed only because the legacy tail could record those lanes
        // differently; that path is gone.
        return this->gbuffer_pass_active();
    }
    bool runtime::cluster_resources_available() noexcept {
        if (this->cluster_pass[0] == nullptr)
            return false; // the active chain has no cluster pass (a stub chain, or the pass was not resolved)
        auto const counts = this->cluster_resource_view.find(render_resource::resource_id::cluster_counts);
        auto const indices = this->cluster_resource_view.find(render_resource::resource_id::cluster_indices);
        auto const slot = this->frame_ring().position();
        return counts && indices && counts->device.get() == this->core_owner.get() && indices->device.get() == this->core_owner.get() &&
               slot < counts->objects.size() && slot < indices->objects.size() && counts->objects[slot].buffer && indices->objects[slot].buffer;
    }

    // Move the scene pass's attachments into their render layouts; see the declaration for why this
    // cannot be left to a render pass.

    // Resync the cached viewport/scissor of every pipeline that draws this frame.
    void runtime::update_pass_geometry() {
        // Pipelines cache a fullscreen viewport/scissor at creation; after a resize the swapchain
        // extent changed, so resync them from the current extent before drawing (begin_pipeline
        // applies the stored values). Done here on the primary thread (it mutates the cached
        // pipeline state), before the scene content below is recorded - inline or in secondaries.
        //
        // THE RENDER EXTENT, and this is complete rather than approximate: the pipelines that read these
        // stored values are the ones that declared `resync_viewport = false` (the scene, transparent,
        // character, shadow and cluster stages), and every one of them draws into a target the ENGINE
        // created at the render extent (`create_render_chain_targets`, ③-D/E A1). A pass that DID declare
        // the resync - the
        // deferred lighting, TAA, the post composite, FXAA and anything that resolves to the output - gets
        // its viewport from its own declaration's extent at record time, so a render scale below 1.0 cannot
        // leave a scene pipeline and a fullscreen pipeline disagreeing about how big a pixel is.
        // (abi 14: the extent is THIS object's own number now, computed from the contract's swapchain
        // extent - see runtime::refresh_frame_extents.)
        // THE CACHED DYNAMIC STATE SPEAKS THE CONTRACT'S VOCABULARY (this batch): `pipeline_handle` stores
        // `rhi::viewport` / `rhi::rect` and `begin_pipeline` re-emits them through the record series, so these
        // two locals are the contract's PODs rather than `VkViewport`/`VkRect2D`.
        rhi::viewport const full_viewport = {
            .x = 0.0f,
            .y = 0.0f,
            .width = static_cast<float>(this->render_extent().width),
            .height = static_cast<float>(this->render_extent().height),
            .min_depth = 0.0f,
            .max_depth = 1.0f,
        };
        rhi::rect const full_scissor = {.offset_x = 0, .offset_y = 0, .width = this->render_extent().width, .height = this->render_extent().height};
        {
            // unique lock: mutating every cached pipeline's viewport/scissor while parallel
            // recording workers may read them through their environments
            std::unique_lock const lock(this->access_mutex);
            for (auto& pipeline : this->pipelines | std::views::values) {
                pipeline.viewport = full_viewport;
                pipeline.scissor = full_scissor;
            }
            // ... and every MESH form of them, for the same reason one at a time: a pipeline object stores what
            // begin_pipeline() applies.
            for (auto& pipeline : this->mesh_pipelines | std::views::values) {
                pipeline.viewport = full_viewport;
                pipeline.scissor = full_scissor;
            }
            // ... and every MESHLET form of them, which needs it for the same reason and is the form the named
            // sessions actually bind (docs/mesh_shaders.md step 3)
            for (auto& pipeline : this->meshlet_pipelines | std::views::values) {
                pipeline.viewport = full_viewport;
                pipeline.scissor = full_scissor;
            }
        }
        // the post-process pipelines are NOT in the named cache above, and begin_pipeline() always
        // re-emits the stored viewport/scissor: leaving them at the creation-time default made every
        // post pass set a 0-wide viewport (validation: "pViewports[0].width (0.000000) is not
        // greater than zero"). The per-pass viewport is set explicitly right after the bind anyway -
        // this keeps the stored values valid. The shadow pipeline is deliberately excluded: its
        // viewport is the fixed shadow-map size, which the shadow pass's content callback sets (see the pass).
        // ... and the post chain's pipelines are NOT resynced here any more: they belong to the post composite
        // PASS (deren.engine.pass.post), which declares `resync_viewport` for the composite and derives each bloom
        // level's extent from its own declaration - so the viewport comes from the pass's declaration instead of
        // from a list this class maintains. The two blocks that used to be here are what that field replaced.
        // ... and FXAA's pipeline is NOT resynced here either: it is the FXAA PASS's now (deren.engine.pass.fxaa),
        // which declares `resync_viewport` like the rest of the post chain.
        // ... and the same for the G-buffer pair: gbuffer_pipeline is the opaque pass's default
        // pipeline (begin_pipeline applies the stored viewport) and gbuffer_debug_pipeline is a
        // fullscreen pass. Neither lives in the named cache above.
        if (this->gbuffer_pipeline) {
            this->gbuffer_pipeline->viewport = full_viewport;
            this->gbuffer_pipeline->scissor = full_scissor;
        }
        // ... and the MESH form of the same pipeline carries its OWN cached viewport, because a pipeline object
        // stores what begin_pipeline() applies: leaving this one at its creation-time zero is the zero-width
        // viewport this project has already paid for once (the same reason the line above exists).
        if (this->gbuffer_pipeline_mesh) {
            this->gbuffer_pipeline_mesh->viewport = full_viewport;
            this->gbuffer_pipeline_mesh->scissor = full_scissor;
        }
        // ... and the MESHLET form, for the same reason: a pipeline object stores what begin_pipeline() applies.
        if (this->gbuffer_pipeline_meshlet) {
            this->gbuffer_pipeline_meshlet->viewport = full_viewport;
            this->gbuffer_pipeline_meshlet->scissor = full_scissor;
        }
        // ... and the debug view's pipeline is not resynced here either: it is its PASS's now (vulkan.pass.
        // gbuffer_debug), which declares resync_viewport like every other fullscreen pass in this chain.
        // ... and the deferred lighting stage's is not here either, for a stronger reason than the TAA
        // resolve's below: it is a PASS (deren.engine.pass.deferred), it declares `resync_viewport = true`, so the
        // record step sets the viewport and scissor from the extent its own declaration produced. The old
        // `deferred_pipeline->viewport = ...` line existed because a pipeline object caches what
        // begin_pipeline() applies; the pass never calls begin_pipeline, the record step binds the pipeline.
        // The TAA resolve's pipeline and viewport are NOT resynced here: the record step sets a fullscreen pass's
        // viewport and scissor from the extent its declaration produced, which is what `resync_viewport`
        // means once a pass states it (see deren.engine.pass's behaviour). The hazard this list used to guard - a
        // fullscreen pass setting a zero-width viewport because it was left out - is gone by construction.
    }

    // Depth-only shadow-pass content: bind the shared scene block (the light UBO binding 7) +
    // the shadow pipeline, apply the live depth bias and draw every scene-tree leaf (the whole
    // scene casts shadows). Pure bind/push/draw commands - the caller owns the barriers and
    // the depth-only rendering instance around it. Recorded inline today; stage 2 records the
    // same content into a per-slot secondary command buffer for parallel pass recording.
    void runtime::record_shadow_content(recording_target secondary, deren::promise::rhi::pipeline* const pipeline, bool const mesh_stage, bool const meshlets) const {
        // THE SESSION'S BUFFER IS THE CONTRACT HANDLE (abi 20/21), and here it is the CASCADE'S OWN secondary: the
        // shadow pass hands this callback that buffer and everything this session records goes into it - which is
        // why the handle has to be the same object the pass began and will execute. THE PIPELINE IS THE CONTRACT
        // HANDLE TOO, and every command below is a contract verb, so no native handle is derived here at all.
        if (!static_cast<bool>(secondary)) {
            return; // no session, nothing to record into
        }
        // NO SET IS BOUND (see the heap bind in begin_recording): the light matrices, the camera and the shadow map
        // are heap slots, and the shadow stage's push block carries the two indices that pick this frame's
        // generation. A secondary records its own state, and the heap bind is made on the buffer it records into.
        [[maybe_unused]] uint32_t const frame_slot = this->frame_ring().position();
        // depth bias is dynamic state on the shadow pipeline: record the live-tunable values
        // (gui-adjustable) before the depth-only draw. THE CONTRACT'S VERB (abi 21), which is `void` - there is
        // nothing for the frame to check - and so the native handle is no longer derived here at all.
        secondary.set_depth_bias(this->shadow_depth_bias_constant, this->shadow_depth_bias_slope, this->shadow_depth_bias_clamp);
        // Shadow pass render_environment: every leaf draws into the DEPTH-ONLY shadow map, so
        // the binder ignores the requested pipeline name and always binds the shadow pipeline -
        // whatever a custom leaf would draw in the main pass, its geometry still casts the same
        // shadow. Default-semantics leaves hit bind_default() and land here too.
        render_environment env;
        // THE SESSION'S BUFFER IS THE CONTRACT HANDLE (abi 21), and here it is the CASCADE'S OWN secondary:
        // the shadow pass hands this callback that buffer, and everything this session records goes into it
        // - which is why the handle has to be the same object the pass began and will execute.
        env.target = std::move(secondary);
        env.default_name = "shadow"; // binder ignores the name; kept for in_default_pipeline()
        // THE BIND, THE VIEWPORT AND THE SCISSOR ARE CONTRACT VERBS (abi 21): the pipeline arrives as the
        // CONTRACT handle the pass resolved (`shadow_frame::record_cascade`), so this session binds it the same
        // way every other session does, and the map-sized viewport/scissor is set from the numbers this frame
        // already holds.
        env.bind = [this, pipeline](recording_target const& session, std::string_view const /*name*/) {
            if (pipeline == nullptr || session.bind_pipeline(*pipeline) != deren::promise::rhi::error::ok) {
                deren::utility::log("runtime: the shadow session could not bind its pipeline - this cascade's casters are skipped");
                return;
            }
            constexpr float origin = 0.0f;
            session.set_viewport(deren::promise::rhi::viewport{
                .x = origin, .y = origin, .width = static_cast<float>(this->shadow_map_size), .height = static_cast<float>(this->shadow_map_size), .min_depth = origin, .max_depth = 1.0f});
            session.set_scissor(deren::promise::rhi::rect{.offset_x = 0, .offset_y = 0, .width = this->shadow_map_size, .height = this->shadow_map_size});
        };
        // The shadow pass MUST write depth for every caster: its env always records depth-write
        // ENABLED (VK_TRUE) regardless of what a leaf requests - the first leaf's
        // set_depth_write() emits the one required vkCmdSetDepthWriteEnable and later leaves
        // dedupe against it. (The pipeline declares depth-write as dynamic state, so it must be
        // set at least once even though the value matches the default.)
        env.set_depth_write_fn = [](recording_target const& session, bool const) {
            // THE CONTRACT'S VERB, and the value is the point of this lambda: the shadow env always forces
            // depth-write ENABLED whatever a leaf asked for, so `true` needs no flag mapping.
            session.set_depth_write(true);
        };
        // ... and it draws every caster TWO-SIDED: a caster is never culled for facing away from
        // the light. A single-sided wall plane whose only face points into the room is back-facing
        // as seen from the sun, so a back-face-culled depth pass never records it and the sunlight
        // pours straight through a wall the camera sees as solid - the classic "the wall behind the
        // camera is transparent" leak. See render_environment::two_sided.
        env.two_sided = true;
        // THE CONTRACT'S OWN ENUM CROSSES HERE (plan X5 B3.4): the lambda used to take `VkCullModeFlags` and
        // map NONE/FRONT/BACK/FRONT_AND_BACK by hand; the environment decides in `rhi::cull_mode` now, so the
        // mapping is gone and `set_cull_mode` gets exactly what was decided.
        env.set_cull_mode_fn = [](recording_target const& session, rhi::cull_mode const mode) {
            session.set_cull_mode(mode);
        };
        // The shadow session's endpoint (see render_environment::push_block). `this` is const here because the
        // method is; the endpoint only records into the command buffer, so the cast is a formality.
        env.push_owner = const_cast<runtime*>(this);
        env.push_block = &runtime::push_stage_block;
        // ---- ... AND HOW THIS SESSION FEEDS ITS GEOMETRY: a mesh pipeline has no input assembler, so a caster is
        //      DISPATCHED and its buffers travel as addresses in the stage block instead of being bound (see
        //      primitive::mesh_dispatch). `mesh_stage` is the pass's answer - it is the side that chose which
        //      pipeline to bind - and the three endpoints are this class's because the device is. ----
        env.mesh_stage = mesh_stage;
        // ... AND WHETHER THIS SESSION DRAWS MESHLETS (docs/mesh_shaders.md step 3).
        env.meshlets = meshlets;
        // THE SHADOW BLOCK'S OWN END: its cascade lane is declared after the three heap index lanes, so the geometry
        // lanes start one word later than the scene block's (see mesh_geometry_offset_shadow). Set for BOTH forms
        // of the pass, because both stage blocks declare the lanes - the vertex entry simply never reads them, and
        // a descriptor-heap pipeline requires every declared byte to have been pushed before the draw.
        env.mesh_geometry_push_offset = deren::engine::mesh_geometry_push_offset_shadow;
        env.buffer_address = &runtime::mesh_buffer_address;
        env.push_at = &runtime::push_geometry_block;
        if (mesh_stage) {
            env.draw_mesh_tasks = &runtime::draw_mesh_tasks;
            // ... and the INDIRECT twin the meshlet session dispatches through (docs/mesh_shaders.md step 3): the
            // counts live in a command table at the primitive's own slot, which is what a compute culling pass
            // will rewrite. The casters' form decides whether it is used, which is why it is set for both.
            env.draw_mesh_tasks_indirect = &runtime::draw_mesh_tasks_indirect;
            // ONE LINE, ONCE, SAYING HOW THE CASTERS ARE FED - because a mesh stage produces the SAME picture as
            // the vertex stage by construction, so "which one drew this frame" is deliberately invisible in the
            // output and must not be inferred from it. (Measured both ways: the capture gate is byte-identical
            // with either pipeline, and forcing the mesh entry to emit nothing changes it - see
            // docs/mesh_shaders.md step 1.)
            static bool logged = false;
            if (!logged) {
                logged = true;
                deren::utility::log("shadow: the casters are DISPATCHED (mesh stage) - {} casters, {} triangles per workgroup",
                                    this->shadow_casters.size(),
                                    85u);
            }
        }
        // draw only the casters that can throw a shadow into the camera frustum (see
        // shadow_casters in begin_recording); the whole scene only when culling is disabled.
        // BLEND (transparent) leaves are skipped: a depth-only pass has no sensible way to blend
        // an alpha-blended shadow, so "no shadow" stays the correct fallback. MASK leaves ARE
        // drawn - shadow.frag runs the same alpha-cutoff discard as pbr.frag, on the same texture
        // array and material record, so a cut-out caster (foliage, a curtain) casts a cut-out
        // shadow instead of a solid one or, as before, none at all.
        for (primitive const* m : this->shadow_casters) {
            if (m->transparent) {
                continue;
            }
            m->draw(env); // depth-only: shadow.vert transforms into light space
        }
    }

    // ---- post-processing: HDR scene target -> exposure + ACES + gamma -> swapchain ----
    // The post chain's PIPELINES are the composite PASS's (deren.engine.pass.post, built by `pipelines::build_post`
    // inside its create step), and its DESCRIPTOR SETS are gone with every other set in this renderer: the
    // composite, the four bloom levels and FXAA read their images through the frame's heap (the HDR target, the
    // bloom levels, the LDR image, the G-buffer depth and normal are all grid slots the shaders name themselves).
    // The samplers stay here only because the pass context hands every pass the five a declaration may choose
    // between (see core::create_samplers).
    // THE FXAA PASS'S RESOLVER IS GONE (S3): its target (the swapchain), the one image it transitions (the LDR
    // image the composite wrote) and its extent all come from its own declaration against the frame's resource
    // table, and its pipeline from the pass. Its push block it composes itself out of the frame's settings and the
    // surface's format, which it cached at create.
    // ---- G-buffer / deferred path (M1: the write pass + its debug view) ----
    // The G-buffer pass is the forward opaque pass with a different fragment stage: same vertex
    // stage, same primitives, same scene set, same instancing/skinning/morphing. What changes is
    // where the fragments go (three 1x targets + a 1x depth image instead of the scene color)
    // and that nothing is lit - see source/shaders/scene/gbuffer.slang.
    std::expected<void, deren::utility::vessel::string> runtime::make_gbuffer_pipeline(std::span<uint8_t const> const fragment_shader_code,
                                                                                       std::span<uint8_t const> const mesh_vertex_shader_code, std::span<uint8_t const> const meshlet_vertex_shader_code) {
        // ---- THE VERTEX FORM IS GONE (docs/mesh_shaders.md step 4): the G-buffer pass's surface write is fed by a
        // MESH stage, which is why the mesh module is required here and its refusal is an ERROR rather than a log
        // line - with no vertex stage there is nothing to fall back to. (The caller disables the deferred path and
        // says so; a device without the extension never gets this far, the runtime refuses to start.)
        if (mesh_vertex_shader_code.empty()) {
            return std::unexpected(deren::utility::vessel::string("no mesh stage for the G-buffer pass, and its vertex form is gone (docs/mesh_shaders.md step 4)"));
        }
        if (!this->evaluate_mesh_shaders(nullptr)) {
            return std::unexpected(deren::utility::vessel::string("the device cannot run the mesh stage the G-buffer pass now requires"));
        }
        // 保留原来的五目标配方：四张数据图覆盖写入，HDR 目标累加自发光。
        // 深度写入仍由绘制时的动态状态控制，mesh/meshlet 共用同一份创建配方。
        auto const build_gbuffer = [this, fragment_shader_code](std::span<uint8_t const> const stage_code) {
            deren::utility::vessel::array<rhi::image_format, gbuffer_pass_attachment_count> const formats = {
                deren::engine::render_layout::gbuffer_formats[0],
                deren::engine::render_layout::gbuffer_formats[1],
                deren::engine::render_layout::gbuffer_formats[2],
                deren::engine::render_layout::gbuffer_velocity_format,
                deren::engine::render_layout::hdr_format,
            };
            deren::utility::vessel::array<rhi::blend_mode, gbuffer_pass_attachment_count> const blends = {
                rhi::blend_mode::opaque,
                rhi::blend_mode::opaque,
                rhi::blend_mode::opaque,
                rhi::blend_mode::opaque,
                rhi::blend_mode::additive,
            };
            auto built = pipelines::make_graphics_pipeline(this->rhi_face(), formats, rhi::image_format::depth,
                                                           stage_code, fragment_shader_code, 1u, true,
                                                           0.0f, 0.0f, 0.0f, blends, rhi::shader_stage::mesh,
                                                           rhi::depth_compare::less_or_equal, "G-buffer");
            if (built) {
                rhi::image_extent const extent = this->render_extent();
                built->viewport = rhi::viewport{.x = 0.0f, .y = 0.0f, .width = static_cast<float>(extent.width), .height = static_cast<float>(extent.height), .min_depth = 0.0f, .max_depth = 1.0f};
                built->scissor = rhi::rect{.offset_x = 0, .offset_y = 0, .width = (extent).width, .height = (extent).height};
            }
            return built;
        };
        auto mesh_result = build_gbuffer(mesh_vertex_shader_code);
        if (!mesh_result) {
            return std::unexpected(deren::utility::vessel::string(mesh_result.error()));
        }
        this->gbuffer_pipeline_mesh = std::move(mesh_result).value();
        deren::utility::log("SUCCESS: G-buffer MESH pipeline created (the surface write, fed by mesh dispatches)");
        // ---- ... and the MESHLET form (docs/mesh_shaders.md step 3): one workgroup per meshlet, its window read
        //      out of the table and each meshlet culled against the camera before it emits anything. Preferred over
        //      the mesh form when it exists, and a refusal is again a log line rather than a failure.
        if (!meshlet_vertex_shader_code.empty()) {
            auto meshlet_result = build_gbuffer(meshlet_vertex_shader_code);
            if (meshlet_result) {
                this->gbuffer_pipeline_meshlet = std::move(meshlet_result).value();
                deren::utility::log("SUCCESS: G-buffer MESHLET pipeline created (one workgroup per meshlet, camera-culled)");
            } else {
                deren::utility::log("G-buffer MESHLET pipeline refused ({}), so the pass keeps the mesh form", meshlet_result.error());
            }
        }
        return {};
    }

    // THE DEFERRED LIGHTING PASS'S FRAME (deren.engine.pass.deferred). The pass owns the barrier, the two per-image
    // input transitions, the LOAD instance, the push and the draw; what the host resolves is the
    // FRAME - which image is lit and the values the push carries - plus deciding that this frame cannot run at all.
    //
    // It was `record_lighting_pass`'s prologue, unchanged, including the order of the two checks: the pipeline
    // first, then the frame's target. The second one is why this returns false instead of recording something, and
    // the caller is what clears the target in that case.
    // THE DEFERRED LIGHTING STAGE'S RESOLVER AND ITS `ensure_inputs` CALLBACK ARE GONE (S3). Its declaration
    // resolves generically - its own pipeline and a
    // full-frame extent - and its TARGET is the frame's alias `scene_color`, which the per-frame resource table
    // answers (the TAA input while the resolve runs, the HDR image otherwise). Its push block it composes itself
    // out of its own SSAO parameters, the frame's inverse view-projection and the frame's answer to what the traced
    // chain is doing. The two per-image transitions the frame used to carry are the frame's ORDERING rule about the
    // images the G-buffer pass wrote, so they now run in the deferred STAGE's preamble in `record_main_drawcalls` -
    // the same move the ray-traced shadow stage's identical pair made, and the same command-stream position.

    // The G-buffer declarations' SAMPLERS: what is left of make_gbuffer_debug_pipeline in the renderer, because the
    // view pipeline is the debug view's PASS's now. They have to exist
    // before create_passes(), since the pass context hands every pass the five a declaration may choose between.
    // The deferred path's transparent pass. Everything about its position is load-bearing:
    //  - after the lighting stage, because a blended surface composites over SHADED pixels, and the
    //    G-buffer instance has no shaded image to composite over;
    //  - not INSIDE the lighting instance, because that one samples the G-buffer depth as a texture
    //    while this one needs the same image as its depth ATTACHMENT, and an image cannot be both in
    //    one instance;
    //  - before the TAA resolve, so the resolve sees the composited frame.
    // ---- temporal anti-aliasing (M3) ----
    glm::vec2 runtime::taa_jitter_offset(uint32_t const index) noexcept {
        // Halton(2,3): the two-radical sequence has the low-discrepancy property that matters here -
        // consecutive samples fill the pixel evenly instead of clustering, so 8 frames of a static
        // scene already resolve close to a 4x4 grid. The offsets are centred on the pixel and given in
        // PIXELS; the caller converts them to a projection offset.
        auto const halton = [](uint32_t position, uint32_t base) {
            float result = 0.0f;
            float fraction = 1.0f;
            while (position > 0) {
                fraction /= static_cast<float>(base);
                result += fraction * static_cast<float>(position % base);
                position /= base;
            }
            return result;
        };
        uint32_t const i = index + 1; // Halton starts at 1 (position 0 gives 0 for every base)
        return {halton(i, 2) - 0.5f, halton(i, 3) - 0.5f};
    }

    bool runtime::taa_active() const noexcept {
        // TAA is
        // the engine's answer to aliasing: a 1x G-buffer cannot be multisampled, so there is no MSAA to fall back on.
        return this->taa_on && this->pass_ready("taa") && this->deferred_lit_active();
    }

    bool runtime::set_taa_enabled(bool const enabled) noexcept {
        // THE FLAG IS THE RENDERER'S and the two WEIGHTS are the pass's (the demo sets them): TAA is not only a
        // pass here - the projection is jittered from `taa_active()` and the scene target is chosen with it - so the
        // renderer has to know whether it is on, while the values only the pass reads.
        bool const was_on = this->taa_on;
        this->taa_on = enabled;
        if (enabled) {
            if (!this->pass_ready("taa")) {
                this->warn_missing_feature("taa", "TAA has no effect: the taa pipeline was not created (see the startup log)");
            } else if (!this->deferred_lit_active()) {
                this->warn_missing_feature("taa", "TAA has no effect: the G-buffer pass or its lighting stage was not created (see the startup log)");
            }
        }
        bool const turned_on = enabled && !was_on;
        if (turned_on) {
            // A fresh history - but only on the off -> on EDGE. The caller mirrors the GUI/config state
            // into the runtime every frame (see main.cpp), so resetting unconditionally here would
            // invalidate the history on every frame: the resolve would fall back to the current
            // (jittered, aliased) frame forever, which looks like TAA running while doing nothing.
            std::size_t const image_count = static_cast<std::size_t>(rhi::max_swapchain_images);
            this->image_view_proj.assign(image_count, this->current_ubo.view_proj_unjittered);
            this->taa_jitter_index = 0;
        }
        // ... and the PASS's half of that edge (whether each image's history holds anything) is the caller's to
        // apply, which is what the return value says: it is the one thing the flag's owner cannot do for the pass.
        return turned_on;
    }

    // The TAA resolve's factory is gone: `deren.engine.pass.taa` builds its own pipeline and owns its per-image
    // history flags in its create step, from its own declaration and its own shaders (the app registers those).

    bool runtime::gbuffer_pass_active() const noexcept {
        // THE MESH FORMS ARE THE PIPELINE NOW (docs/mesh_shaders.md step 4): `gbuffer_pipeline` is the vertex form,
        // which no longer exists, so the question is whether EITHER mesh form was built.
        if (!this->gbuffer_pipeline_mesh.has_value() && !this->gbuffer_pipeline_meshlet.has_value()) {
            return false;
        }
        if (this->gbuffer_debug) {
            return this->pass_ready("gbuffer-debug");
        }
        return this->pass_ready("deferred");
    }

    bool runtime::deferred_lit_active() const noexcept {
        // the debug view wins when both are available: looking at the stored data is an inspection,
        // not a render mode (and the two write the HDR target in incompatible ways)
        return this->gbuffer_pass_active() && !this->gbuffer_debug;
    }

    // `set_gbuffer_channel` IS GONE (the demo's now): it was a pure forwarder to the debug view's own parameter
    // (which pass owns the channel and its clamp), and nothing in this file read it - the demo sets it on the pass
    // it found by declaration name.

    // WHAT IS NOT HERE ANY MORE: no per-pass resolver, and no G-buffer descriptor family either. The G-buffer
    // images are HEAP slots now (publish_frame_resources writes each image's grid slot once per frame), and the
    // rule every pass follows is the one this space records: a pass resolves its OWN declaration (its own bindings,
    // its pipeline, the extent rule), and its push block is composed by the PASS from `io.constants` plus its own
    // parameters. A frame fact is likewise the frame's (`make_frame_facts`) rather than the renderer's copy of a
    // pass's state.

    void runtime::set_furnace(bool const enabled) noexcept {
        this->furnace = enabled;
    }

    void runtime::register_shader(std::string_view const name, std::span<uint8_t const> const bytecode) {
        // The app loads shaders (it knows the directory and the file names) and hands them over here; a pass
        // asks for its own by name at create time. A COPY, because the caller's buffer is a startup local.
        for (auto& [registered_name, registered_bytes] : this->registered_shaders) {
            if (registered_name == name) {
                registered_bytes.assign(bytecode.begin(), bytecode.end());
                return;
            }
        }
        this->registered_shaders.emplace_back(deren::utility::vessel::string(name), deren::utility::vessel::vector<uint8_t>(bytecode.begin(), bytecode.end()));
    }

    void runtime::create_passes() {
        // The record step's create step for every stage this runtime wires. A pass builds what it owns from its own
        // declaration and its own shader, so this is also where a pass that could not build itself says so -
        // and a pass that says so stays INACTIVE (its feature predicate is false), which is what makes a
        // startup failure here a log line rather than a broken frame.
        //
        // IT IS HANDED A CONTEXT, NOT A HOST: building a pass needs a device and the shared lookups, and it
        // needs no frame. ANY owner can fill this struct - that is what makes a pass usable outside this
        // renderer - and this runtime is one such owner, filling the device from the core it owns.
        //
        // The SHARED samplers must exist before the context is filled, because a pass caches the five it may
        // choose between at create time (a declaration picks one by hint, and a null sampler in a set is a
        // validation error rather than a skipped fetch). They are the device root's (`core::create_samplers`),
        // which is what `shared_samplers` below reads; two of them were once made inside the pipeline builders
        // that first needed them, which is the naming accident that comment records.
        // THE CHAIN IS AN INPUT, and there is deliberately no fallback: with the passes constructed outside this
        // class, "no chain was handed over" means there is nothing to create or record, so the one honest answer is
        // to say so and return rather than to record a frame of this class's own empty stage sequence.
        if (this->frame_chain == nullptr) {
            deren::utility::log("no pass chain was handed over (see set_pass_chain): nothing to create or record");
            return;
        }
        // ... and what the passes may NAME, before any of them is created: the registry the pass filter answers
        // `resource()` from (see publish_pass_resources). It is the renderer's half of the channel; the passes'
        // half is that they ask for what their own declaration lists instead of being handed it.
        this->publish_pass_resources();
        // THE FRAME'S STRUCTURE, FROM THE CHAIN BY DECLARATION NAME: the stage arrays and the two GI halves (see
        // bind_frame_chain). The chain is the application's (see set_pass_chain) - this class owns no passes, so
        // there is no chain of its own for this to bind.
        this->bind_frame_chain(*this->frame_chain);
        // ---- MESH SHADERS: MAY A PASS BUILD ONE ON THIS DEVICE? (docs/mesh_shaders.md) ----
        // THE THREE CONDITIONS ARE MEASURED HERE, once, before any pass is created - because a pass that already
        // built a mesh pipeline on a device that cannot run one is a validation ERROR at vkCreateShaderModule, and
        // "the pass asked and was refused" is too late. The answer is logged either way: whether the shadow pass
        // drew its casters as dispatches must not be something a reader has to infer from a picture.
        //  1. the extension and its `meshShader` feature (core::mesh_shader_available), and
        //  2. the device's push-constant budget, and the heap's push-data window, both >= the stage block a mesh
        //     stage needs (`mesh_stage_block_size`): a mesh stage is handed the draw's whole geometry window as
        //     data because it has no input assembler to take it from, and that block is larger than the 128 bytes
        //     the spec guarantees - so the path is available where the device reports room and the VERTEX path
        //     stays where it does not.
        {
            uint32_t const push_constants = runtime_detail::max_push_constants_of(this->rhi_face());
            uint32_t const push_data = runtime_detail::heap_max_push_data(this->rhi_face());
            this->mesh_shaders = this->evaluate_mesh_shaders(&this->mesh_shaders_unavailable_reason);
            if (!this->mesh_shaders) {
                // ---- THE EXTENSION IS A REQUIREMENT NOW (docs/mesh_shaders.md step 4): the vertex geometry path is
                // gone, so there is no second rasterizer to fall back to and a device that cannot run a mesh stage
                // cannot draw the scene at all. Saying so here, once, with the reason, is the whole of the fallback
                // that is left - and it happens BEFORE any pass is created.
                deren::utility::panic(std::source_location::current(),
                                      "mesh shaders are required and this device does not offer them: {} (docs/mesh_shaders.md step 4)",
                                      this->mesh_shaders_unavailable_reason);
            }
            deren::utility::log("mesh shaders: REQUIRED and available - every geometry stage is dispatched (push block {} B, within {} B push constants / {} B push data)",
                                deren::engine::mesh_stage_block_size,
                                push_constants,
                                push_data);
        }
        pass::pass_context const build = this->make_pass_context();
        // ONE CREATE STEP OVER EVERY PASS, in the order the OWNING chain holds them (see the member block in the
        // header): the chain owns the passes and its `init` IS the whole create step, in the order the application
        // emplaced them. A pass that could not build itself reports its own name in `rejected` and stays INACTIVE
        // (its feature predicate is false), which is what makes a startup failure a log line rather than a broken
        // frame.
        pass::pass_chain& recorded = *this->frame_chain; // the chain the application handed over (see set_pass_chain)
        pass::run_report const created = recorded.init(build);
        if (!created.rejected.empty()) {
            deren::utility::log("pass '{}': its declaration was refused by the validator, so it does not run", created.rejected);
        }
        // THE TWO JOBS, and they are created HERE rather than by the application: neither is a frame pass (see
        // their headers - one runs once inside the structure-build command buffer, the other per frame from a
        // caster list), but both are GPU-owning objects constructed from the same context, so they belong in the
        // same step. Before the pass filter existed each had its own entry point in this class, because a pass
        // could not name a resource the renderer owns; now it asks (see publish_pass_resources).
        // mask_bake_result, not created: `created` above is in scope and MSVC /W4 reports C4456 (an error under /WX).
        if (auto const mask_bake_result = this->mask_bake.create(build); !mask_bake_result) {
            deren::utility::log("alphaMode MASK bake unavailable: {} (the any-hit stage still cuts masked geometry per hit)", mask_bake_result.error());
        }
        // compute_skin_result, not created: same C4456 rule as the mask-bake job above - the outer `created` is in scope.
        if (auto const compute_skin_result = this->compute_skin.create(build); !compute_skin_result) {
            deren::utility::log("skinned shadow refit unavailable: {} (a traced shadow keeps the bind pose)", compute_skin_result.error());
        }
    }

    render_resource::shared::sampler_set runtime::shared_samplers() const noexcept {
        // The runtime owns all six objects. Passes borrow their contract pointers by hint;
        // no backend handle is needed to identify a sampler or to write it through the heap.
        auto borrowed = [](std::shared_ptr<rhi::sampler> const& owned) {
            return static_cast<rhi::sampler*>(owned.get());
        };
        return {.gbuffer = borrowed(this->gbuffer_sampler),
                .taa = borrowed(this->taa_sampler),
                .post = borrowed(this->post_sampler),
                .nearest = borrowed(this->post_nearest_sampler),
                .shadow = borrowed(this->shadow_sampler),
                // The bindless texture array's sampler, which only hand-written set code used until the MASK
                // bake had to write the scene layout's binding 1 itself (see sampler_set's doc).
                .textures = borrowed(this->texture_sampler)};
    }

    std::span<uint8_t const> runtime::registered_shader(std::string_view const name) const noexcept {
        for (auto const& [registered_name, registered_bytes] : this->registered_shaders) {
            if (registered_name == name) {
                return registered_bytes;
            }
        }
        return {}; // a pass whose shader was never registered builds nothing and says so
    }

    bool runtime::evaluate_mesh_shaders(deren::utility::vessel::string* const reason) const noexcept {
        // THE GATE, as one question asked in two places (create_passes, for the log and the pass context; and the
        // G-buffer pipeline's builder, which runs earlier). In both cases the answer is the same three facts, and
        // the reason string is only filled in for the one caller that logs it.
        uint32_t const push_constants = runtime_detail::max_push_constants_of(this->rhi_face());
        uint32_t const push_data = runtime_detail::heap_max_push_data(this->rhi_face());
        auto const unavailable = [reason](deren::utility::vessel::string text) {
            if (reason != nullptr) {
                *reason = std::move(text);
            }
            return false;
        };
        if (!runtime_detail::mesh_shader_available_of(this->rhi_face())) {
            return unavailable("the device has no mesh shader feature");
        }
        if (push_constants < deren::engine::mesh_stage_block_size) {
            return unavailable(deren::utility::vessel::format("maxPushConstantsSize is {} B, under the {} B block a mesh stage needs", push_constants, deren::engine::mesh_stage_block_size));
        }
        if (push_data < deren::engine::mesh_stage_block_size) {
            return unavailable(deren::utility::vessel::format("the heap's maxPushDataSize is {} B, under the {} B block a mesh stage needs", push_data, deren::engine::mesh_stage_block_size));
        }
        if (reason != nullptr) {
            reason->clear();
        }
        return true;
    }

    pass::pass_context runtime::make_pass_context() noexcept { // THE ONE CONSTRUCTION SITE for a pass's create-time context, and it is a method rather than a block
        // because there were two of them: `create_passes()` built one for the stages, and the two jobs that are
        // not frame passes (the MASK bake, the compute-skinning job) each built their own copy. A second copy of
        // this struct is how a per-pass entry point per job appears, which is what the pass filter exists to
        // remove - so there is one builder now, and everything that constructs a pass uses it.
        return pass::pass_context{
            // NO DEVICE LANE (abi 21): the builders are contract factories and take `face` alone; the pass layer
            // reaches a native device through the escape when it needs an allocated entry point (pass::native_device).
            .face = &this->rhi_face(),
            .swap_chain_format = this->swap_chain_image_format,
            .shader = [](void* owner, std::string_view const name) { return static_cast<runtime*>(owner)->registered_shader(name); },
            // The SBT numbers a tracing pass builds its table against, straight from the capability query (see
            // device_capabilities): zeroed here on a device that has no ray-tracing pipeline.
            .ray_tracing_properties = runtime_detail::ray_tracing_properties_of(this->rhi_face()),
            // ... and the owner's buffer factory: the pass gets a handle and a device address, the runtime keeps
            // the allocation for the generation (see `pass_upload_buffers`).
            .create_upload_buffer =
                [](void* owner, void const* data, uint64_t const bytes, rhi::buffer_flags const flags, uint64_t* const out_address) -> rhi::buffer* {
                runtime* const self = static_cast<runtime*>(owner);
                // THE CALLER'S FLAGS GO STRAIGHT THROUGH (abi 24): the hook speaks the contract's bit set now, so
                // the Vulkan-usage switch that stood here - and the named panic for a bit its mapping did not
                // cover - has nothing left to translate. `device_address` is ORed in because the ADDRESS is what
                // the caller comes for (a shader binding table's whole purpose); the caller's own bits decide the
                // rest. The pass framework's rule still holds: the owner either builds what was asked for or
                // answers nullptr, and a pass that gets nullptr records nothing rather than reading zeros.
                if (out_address != nullptr)
                    *out_address = 0;
                if (data == nullptr || bytes == 0)
                    return nullptr;
                self->pass_upload_buffers.push_back(self->create_shared_upload(
                    std::span<std::byte const>(static_cast<std::byte const*>(data), static_cast<std::size_t>(bytes)),
                    flags | rhi::to_bits(rhi::buffer_flag::device_address)));
                // A FACTORY THAT REFUSED LEAVES THE MANAGER EMPTY, and that is the nullptr this hook documents:
                // checked BEFORE the address is read, because `buffer_address` dereferences the manager.
                if (!self->pass_upload_buffers.back()) {
                    return nullptr;
                }
                if (out_address != nullptr) {
                    *out_address = self->buffer_address(*self->pass_upload_buffers.back());
                }
                // the CONTRACT handle, which the pass borrows and never destroys (`object_ptr::get()` is the
                // pointer it holds): `pass_upload_buffers` is this class's and lives with the generation (see its
                // own note).
                return self->pass_upload_buffers.back().get();
            },
            // The surface's format: a SESSION-STABLE device fact a pipeline that renders into the swapchain must
            // be created with (see pass_context). The post chain needs it today; the graphics passes being
            // extracted need it tomorrow.
            .swap_chain_image_format = this->swap_chain_image_format,
            // ... and the DEPTH format, which the shadow pass`s pipeline needs (it has a depth attachment and no
            // colour one): the same kind of session-stable device fact, and the second one a context carries.
            .depth_format = rhi::image_format::depth,
            // ... and whether the device can run a MESH pipeline at all, which a pass must not try to find out by
            // attempting it (see the metric above and pass_context::mesh_shaders).
            .mesh_shaders = this->mesh_shaders,
            // The channel a pass uses to build what it owns over resources the RENDERER holds: the handles of the
            // resources this runtime published (`pass_resources`). It forwards to the filter, which is the object
            // that knows what a pass may reach - the context itself stays a plain struct of callbacks, so the
            // framework still does not depend on `deren.vulkan.core`.
            .resource =
                [](void* owner, render_resource::resource_id const id, uint32_t const element) {
                    runtime* const self = static_cast<runtime*>(owner);
                    resource_handles const handles = self->pass_resources.resource(id, element);
                    return pass::resolved_binding{.image_handle = handles.image, .view_handle = handles.view, .buffer_handle = handles.buffer};
                },
            .frames_in_flight = this->frame_ring().slot_count(),
            .owner = this,
        };
    }

    void runtime::publish_pass_resources() {
        // WHAT THE RENDERER PUBLISHES FOR ITS PASSES, in the declaration layer's own vocabulary: a pass asks for
        // `resource_id::material_table` or `resource_id::skin_matrices` + a slot, not for a member of this class.
        // Everything here is SESSION-STABLE by the filter's contract (the material table and the texture array
        // are created once and only rewritten; the skin matrix buffers are created once and rewritten per slot),
        // which is what makes them safe for a pass to name at create time.
        if (this->material_mapped != nullptr) {
            this->pass_resources.register_resource(render_resource::resource_id::material_table, 0, resource_handles{.buffer = &*this->material_buffer});
        }
        if (!this->owned_texture_views.empty()) {
            this->pass_resources.register_resource(render_resource::resource_id::scene_textures, 0, resource_handles{.view = &*this->owned_texture_views[0]});
        }
        for (uint32_t slot = 0; slot < this->skin_buffers.size(); ++slot) {
            this->pass_resources.register_resource(render_resource::resource_id::skin_matrices, slot, resource_handles{.buffer = &*this->skin_buffers[slot]});
        }
    }

    void runtime::update_frame_constants() noexcept {
        // THE ONE PLACE THE FACTS TYPE AND THE UBOs HAVE TO AGREE. `deren.engine.frame_constants` deliberately does not
        // include `camera_ubo`/`light_ubo` (the pass framework must not depend on deren.engine.primitive, which imports
        // core), so the fields are copied here - one site to check, instead of a type relationship spread across
        // two modules that only a reader of both would notice breaking.
        this->frame_facts.view = this->current_ubo.view;
        this->frame_facts.proj = this->current_ubo.proj; // JITTERED: the geometry is sampled at these offsets
        this->frame_facts.view_proj_unjittered = this->current_ubo.view_proj_unjittered;
        this->frame_facts.inv_view_proj = this->current_inv_view_proj;
        this->frame_facts.camera_pos = glm::vec3(this->current_ubo.camera_pos);
        this->frame_facts.scene_center = this->shadow_scene_center;
        this->frame_facts.scene_radius = this->scene_extent_radius;
        // The sun, normalized here because the shader wants a
        // direction, the UBO's own lane stays as the app set it). A zero direction - nothing has set a light yet
        // - is kept as zero rather than turned into a NaN by normalize().
        glm::vec3 const sun = glm::vec3(this->light_state.light_dir);
        this->frame_facts.light_dir = glm::dot(sun, sun) > 0.0f ? glm::vec4(glm::normalize(sun), 0.0f) : glm::vec4(0.0f);
        // ... and the renderer's settings that MORE THAN ONE pass reads (see render_settings for the rule): the
        // post chain's exposure and bloom weights, FXAA's two thresholds, and the GI upsample's silhouette test.
        // The members stay the app-facing ones (`set_exposure` and friends) - this is the frame's copy of them.
        this->frame_facts.settings.exposure = this->exposure_scale;
        this->frame_facts.settings.bloom_intensity = this->bloom_intensity;
        this->frame_facts.settings.bloom_threshold = this->bloom_threshold;
        this->frame_facts.settings.fxaa_subpixel = this->fxaa_subpixel;
        this->frame_facts.settings.fxaa_edge_threshold = this->fxaa_edge_threshold;
    }

    void runtime::publish_frame_resources() {
        pass::resource_table& table = this->frame_resources;
        table.clear();

        // NO FAMILY-SPAN PUBLICATION IS LEFT HERE (3-D/E step 2): the one family that used the run form was
        // `resource_id::swapchain_image`, and the contract has no contiguous run of the presentation images to
        // publish - the engine names ONE of them per frame (`frame_image()`) and publishes that instance below.
        // Every other per-image group is engine-held and published instance by instance (`owned_family`).
        // One resource that exists once (a device-wide image): the instance is still the schema's (0 for
        // device-wide, the slot for a per-frame-slot buffer that happens to exist once).
        auto const single = [&table](render_resource::resource_id const id, uint32_t const element, uint32_t const instance, pass::resolved_binding const binding) {
            table.publish(id, element, instance, binding);
        };
        // One buffer: the RAII wrapper holds a handle and the descriptor needs the VkBuffer behind it, so the
        // lookup goes through vma exactly where the renderer's own binding writes do.
        auto const buffer = [&table](render_resource::resource_id const id, uint32_t const instance, auto const& owned) {
            table.publish(id, 0, instance, pass::resolved_binding{.buffer_handle = &*owned});
        };
        for (size_t i = 0; i < this->scene_table_resource_views.size(); ++i) {
            auto const id = pass::scene_tables_pass::ids[i];
            if (auto const generation = this->scene_table_resource_views[i].find(id))
                buffer(id, 0, generation->objects[0].buffer);
        }
        // One image, in the contract lane ONLY (plan X5 B2): the raw `VkImage` the binding used to carry is
        // gone, and the handle is what `image()` answers with.
        auto const image = [](auto const& owned) -> pass::resolved_binding {
            return static_cast<bool>(owned) ? pass::resolved_binding{.image_handle = &*owned} : pass::resolved_binding{};
        };
        // ONE ENGINE-OWNED PER-IMAGE FAMILY (③-D/E A1): the engine holds the generation's handles, so the
        // run the table needs is assembled here, instance by instance, each handle through the contract's
        // escape. `find()` answers exactly what the family form answered; the family form itself needs a
        // CONTIGUOUS run of `VkImageView`s, which an array of `object_ptr`s is not - and a local run
        // would dangle, because the table stores the span rather than copying it.
        auto const owned_family = [&table](render_resource::resource_id const id, uint32_t const element, auto const& images, auto const& views) {
            for (std::size_t instance = 0; instance < rhi::max_swapchain_images; ++instance) {
                if (!static_cast<bool>(images[instance]) || !static_cast<bool>(views[instance])) {
                    continue;
                }
                table.publish(id, element, static_cast<uint32_t>(instance),
                              pass::resolved_binding{.image_handle = &*images[instance], .view_handle = &*views[instance]});
            }
        };

        // ---- the render-target chain: the engine's own families, and the ONE backend-owned image ----
        // THE PRESENTATION IMAGE IS THE BACKEND'S AND IS PUBLISHED ONE INSTANCE AT A TIME (③-D/E step 2):
        // the family form the legacy runtime used (`swap_chain_image_views` / `swap_chain_images`) named a
        // backend member this half may not import, and it published every image of the generation when
        // only ONE can be rendered into per frame. The contract's answer is the frame's own image
        // (`frame_image()`, BORROWED) and an OWNED view made from it (`image::make_view()`, abi 16 item
        // B), published at the index acquire returned - which is exactly what `resolve_declaration` looks
        // a render target up by (`instance_for(per_swapchain_image, frame)` is `frame.image_index`). The
        // view is kept in this slot's member until this slot comes around again (see that member's note).
        if (rhi::image* const frame_image = this->rhi_face().frame_image(); frame_image != nullptr) {
            std::size_t const slot = static_cast<std::size_t>(this->frame_ring().position());
            if (slot < this->frame_swapchain_views.size()) {
                this->frame_swapchain_views[slot] = rhi::object_ptr<rhi::image_view>{frame_image->make_view(rhi::image_view_desc{})};
                if (this->frame_swapchain_views[slot]) {
                    single(render_resource::resource_id::swapchain_image, 0, this->current_image_index,
                           pass::resolved_binding{.image_handle = frame_image, .view_handle = &*this->frame_swapchain_views[slot]});
                }
            }
        }
        // THE HDR AND DISPLAY (LDR) TARGETS ARE THE ENGINE'S OWN (③-D/E A1.3) and, like the TAA history pair
        // above, published instance by instance BECAUSE THEY HAVE NO FAMILY-SPAN CONSUMER: neither is ever an
        // `own` per-image binding (the scene/debug passes name `hdr` as a render TARGET, the FXAA/upscale
        // passes name `ldr` as a BARRIER image), and `views_of` - the only reader of a family span - is
        // consulted by `resolve_declaration` for own per-swapchain-image bindings alone. `find()` is what
        // every one of those sites uses, and it answers exactly what the family form answered.
        owned_family(render_resource::resource_id::hdr, 0, this->hdr_images, this->hdr_image_views);
        // THE VISIBILITY BUFFER AND ITS CAPTURE-ONLY DEPTH (visibility-buffer v0.3b): the raster pass names both as
        // render TARGETS, so the table has to carry the current image or the declaration cannot resolve.
        owned_family(render_resource::resource_id::visibility_buffer, 0, this->visibility_images, this->visibility_image_views);
        owned_family(render_resource::resource_id::ldr, 0, this->ldr_images, this->ldr_image_views);
        // THE G-BUFFER CLUSTER IS THE ENGINE'S OWN (③-D/E A1.4), published the same way and for the same
        // reason: NO FAMILY-SPAN CONSUMER. `gbuffer_depth` and `velocity` ARE `own` per-image bindings of the
        // TAA pass, so `resolve_declaration` does fill `own_per_image` for them - and NO PASS READS IT (the
        // framework is the only writer, and every pass names its own image's view through `find`). The span
        // form would need a parallel contiguous `VkImage`/`VkImageView` run beside each manager array; with
        // no reader, the instance-by-instance form answers the same `find` with no second copy to drift.
        owned_family(render_resource::resource_id::gbuffer_depth, 0, this->gbuffer_depth_images, this->gbuffer_depth_image_views);
        owned_family(render_resource::resource_id::velocity, 0, this->velocity_images, this->velocity_image_views);
        // THE TAA HISTORY PAIR IS THE ENGINE'S OWN (③-D/E A1.1), and the rest of this chain follows it group
        // by group - which is why these calls have a different shape from the backend-owned ones.
        owned_family(render_resource::resource_id::taa_history, 0, this->taa_history_images, this->taa_history_image_views);
        // THE STOCHASTIC-CHAIN TRIO AND THE FOUR BLOOM LEVELS ARE THE ENGINE'S OWN (③-D/E A1.5), published the
        // same way and for the same reason as the groups above: NO FAMILY-SPAN READER. None of the seven is an
        // `own` per-image binding, so `resolve_declaration` never even fills `own_per_image` for them - each pass
        // names the image it reads through `find`.
        // The three stored surface targets are a RUN of targets (`count = 3`), never an `own` binding: no
        // `own_per_image` is filled for them, so the instance-by-instance form is exact here too.
        for (std::size_t target = 0; target < this->gbuffer_images.size(); ++target) {
            owned_family(render_resource::resource_id::gbuffer_targets, static_cast<uint32_t>(target), this->gbuffer_images[target], this->gbuffer_image_views[target]);
        }
        for (uint32_t level = 0; level < deren::engine::render_layout::bloom_level_count; ++level) {
            owned_family(render_resource::resource_id::bloom, level, this->bloom_images[level], this->bloom_image_views[level]);
        }
        // `scene_color` is an ALIAS rather than a family of its own: the scene-side passes write the TAA input
        // while the resolve runs and the HDR target otherwise (see scene_target_view), so WHAT THE ID MEANS is
        // decided here, once per frame, in the same place that answers it for the resolvers - which is also why
        // the table is refreshed per frame rather than per generation. (It has no family at all, before or
        // after A1.4: the single entries below are the whole of what this id publishes.)
        // THE COUNT IS THE GENERATION'S OWN: every per-image group is an engine-held fixed-size array now, so
        // the swapchain's own image count is the one authority for how many images exist.
        std::size_t const scene_images = static_cast<std::size_t>(rhi::max_swapchain_images);
        for (std::size_t i = 0; i < scene_images; ++i) {
            // THE CONTRACT LANES RIDE BESIDE THE RAW ONES, and here they are the reason the frame renders at
            // all: the scene pass's FIVE colour attachments take `view_handle` (its rendering scope is refused
            // whole if one is null), and this alias is the one publication whose target THIS FRAME's TAA switch
            // chooses - so the branch is repeated (never a handle for an image the frame did not choose), and
            // the same managers `scene_target_image` / `scene_target_view` read answer the contract side.
            deren::promise::rhi::image* color_image = nullptr;
            deren::promise::rhi::image_view* color_view = nullptr;
            if (this->taa_active()) {
                if (i < this->scene_color_images.size() && static_cast<bool>(this->scene_color_images[i])) {
                    color_image = &*this->scene_color_images[i];
                }
                if (i < this->scene_color_image_views.size() && static_cast<bool>(this->scene_color_image_views[i])) {
                    color_view = &*this->scene_color_image_views[i];
                }
            } else {
                if (i < this->hdr_images.size() && static_cast<bool>(this->hdr_images[i])) {
                    color_image = &*this->hdr_images[i];
                }
                if (i < this->hdr_image_views.size() && static_cast<bool>(this->hdr_image_views[i])) {
                    color_view = &*this->hdr_image_views[i];
                }
            }
            single(render_resource::resource_id::scene_color, 0, static_cast<uint32_t>(i),
                   pass::resolved_binding{.image_handle = color_image, .view_handle = color_view});
        }

        // ---- per FRAME SLOT: the ray-traced visibility image, the shadow map's cascades, the buffers ----
        // THE ENGINE'S OWN IMAGE NOW (③-D/E A1.2), and published instance by instance because THIS GROUP HAS
        // NO FAMILY-SPAN CONSUMER: `rt_shadow_visibility` is scoped PER FRAME SLOT, and `resolve_declaration`
        // only fills `own_per_image` for `resource_scope::per_swapchain_image` - so no `views_of` run is owed
        // here, and the engine's `deren::utility::vessel::array<object_ptr>` needs no parallel contiguous native array.
        for (std::size_t slot = 0; slot < rhi::max_frames_in_flight; ++slot) {
            if (!static_cast<bool>(this->rt_shadow_image_views[slot]) || !static_cast<bool>(this->rt_shadow_images[slot])) {
                continue;
            }
            single(render_resource::resource_id::rt_shadow_visibility, 0, static_cast<uint32_t>(slot),
                   pass::resolved_binding{.image_handle = &*this->rt_shadow_images[slot], .view_handle = &*this->rt_shadow_image_views[slot]});
        }
        // The shadow map's family elements are the CASCADES (see render_resource::shadow_io): the image is one
        // layered depth array per slot and the pass renders one layer at a time, so every layer the image
        // currently HAS is published, by cascade index, with the image behind it for the layer's own barrier.
        for (std::size_t slot = 0; slot < this->shadow_images.size() && slot < this->shadow_layer_views.size(); ++slot) {
            if (!static_cast<bool>(this->shadow_images[slot])) {
                continue;
            }
            for (std::size_t layer = 0; layer < this->shadow_layer_views[slot].size(); ++layer) {
                single(render_resource::resource_id::shadow_map, static_cast<uint32_t>(layer), static_cast<uint32_t>(slot),
                       pass::resolved_binding{.image_handle = &*this->shadow_images[slot], .view_handle = &*this->shadow_layer_views[slot][layer]});
            }
        }
        // The per-slot buffers, each into its own instance: a frame in flight reads its own copy, which is the
        // whole reason those resources exist per slot (see the member docs).
        uint32_t const slots = this->frame_ring().slot_count();
        for (uint32_t slot = 0; slot < slots; ++slot) {
            if (slot < this->camera_buffers.size()) {
                buffer(render_resource::resource_id::camera_ubo, slot, this->camera_buffers[slot]);
            }
            if (slot < this->light_buffers.size()) {
                buffer(render_resource::resource_id::light_ubo, slot, this->light_buffers[slot]);
            }
            if (slot < this->head_buffers.size())
                buffer(render_resource::resource_id::head_ubo, slot, this->head_buffers[slot]);
            if (slot < this->motion_buffers.size()) {
                buffer(render_resource::resource_id::motion_vectors, slot, this->motion_buffers[slot]);
            }
            if (slot < this->skin_buffers.size()) {
                buffer(render_resource::resource_id::skin_matrices, slot, this->skin_buffers[slot]);
            }
            if (slot < this->skin_buffers_previous.size()) {
                buffer(render_resource::resource_id::skin_matrices_previous, slot, this->skin_buffers_previous[slot]);
            }
            if (slot < this->morph_buffers.size()) {
                buffer(render_resource::resource_id::morph_targets, slot, this->morph_buffers[slot]);
            }
            if (slot < this->cluster_count_buffers.size()) {
                buffer(render_resource::resource_id::cluster_counts, slot, this->cluster_count_buffers[slot]);
            }
            if (slot < this->cluster_index_buffers.size()) {
                buffer(render_resource::resource_id::cluster_indices, slot, this->cluster_index_buffers[slot]);
            }
        }

        // ---- device-wide: the probe grid's eight elements, its geometry, the cubes and the textures ----
        if (static_cast<bool>(this->furnace_cube_image) && static_cast<bool>(this->furnace_cube_view)) {
            single(render_resource::resource_id::furnace_cube, 0, 0,
                   pass::resolved_binding{.image_handle = &*this->furnace_cube_image, .view_handle = &*this->furnace_cube_view});
        }
        // The white fallback and the array it is element 0 of. The array is BINDLESS (one binding, N descriptors),
        // which the declaration vocabulary cannot index element by element yet - so what is published is the one
        // element that always exists, which is also the one every declaration can name (element 0).
        if (this->white_texture_index < this->owned_textures.size() && this->white_texture_index < this->owned_texture_views.size()) {
            pass::resolved_binding white = image(this->owned_textures[this->white_texture_index]);
            single(render_resource::resource_id::white_texture, 0, 0, white);
            single(render_resource::resource_id::scene_textures, 0, 0, white);
        }
        // The IBL triple, in the order set_ibl uploads it: prefiltered environment, irradiance, BRDF LUT.
        if (this->ibl_views.size() >= 3 && this->ibl_images.size() >= 3) {
            // THE CONTRACT LANE, ONE CALL PER IMAGE (plan X5 B2): the raw `view` assignments that stood here
            // were filling a lane that no longer exists - the `image()` lambda publishes the contract handle,
            // which is what a pass's recording verbs take.
            single(render_resource::resource_id::ibl_env, 0, 0, image(this->ibl_images[0]));
            single(render_resource::resource_id::ibl_irradiance, 0, 0, image(this->ibl_images[1]));
            single(render_resource::resource_id::brdf_lut, 0, 0, image(this->ibl_images[2]));
        }
        for (uint32_t slot = 0; slot < this->frame_ring().slot_count(); ++slot) {
            single(render_resource::resource_id::top_level_structure, 0, slot,
                   pass::resolved_binding{.structure_handle = this->structures.handle(slot)});
        }
        if (this->post_lut_image && this->post_lut_view)
            single(render_resource::resource_id::post_lut, 0, 0, {.image_handle = this->post_lut_image.get(), .view_handle = this->post_lut_view.get()});
        if (this->goo_fgd_image && this->goo_fgd_view)
            single(render_resource::resource_id::goo_fgd_lut, 0, 0, {.image_handle = this->goo_fgd_image.get(), .view_handle = this->goo_fgd_view.get()});
    }

    void runtime::verify_resource_table(pass::frame_pass const& pass, pass::resolved_io const& io) {
        render_resource::pass_io const& decl = pass.io();
        pass::resource_table const& table = this->frame_resources;
        uint32_t checked = 0;
        uint32_t mismatched = 0;
        auto const as_pointer = [](auto const handle) { return static_cast<void const*>(handle); };
        auto const check = [&](render_resource::resource_id const id, uint32_t const element, pass::resolved_binding const& resolved, std::string_view const channel) {
            render_resource::resource_info const* const info = render_resource::find(id);
            if (info == nullptr) {
                return;
            }
            ++checked;
            pass::resolved_binding const published = table.find(id, element, pass::instance_for(info->scope, io.frame));
            // THE CHECK COMPARES THE CONTRACT LANE (plan X5 B2): it is the lane the table publishes and the lane
            // the passes record through, so a comparison is a statement about what a pass actually receives.
            if (published.view_handle == resolved.view_handle && published.buffer_handle == resolved.buffer_handle && published.image_handle == resolved.image_handle && published.structure_handle == resolved.structure_handle) {
                return;
            }
            ++mismatched;
            // Bounded, and only while the check's window is open: a wrong entry is a finding to fix, not a reason
            // to fill the log of every frame.
            if (this->resource_check_frames < 3 && this->resource_check_mismatched + mismatched <= 10) {
                deren::utility::log("resource table: pass '{}' {} (resource {}, element {}) resolved as [view {}, buffer {}, image {}] but published as [view {}, buffer {}, image {}]",
                                    decl.name, channel, static_cast<int32_t>(id), element,
                                    as_pointer(resolved.view_handle), as_pointer(resolved.buffer_handle), as_pointer(resolved.image_handle),
                                    as_pointer(published.view_handle), as_pointer(published.buffer_handle), as_pointer(published.image_handle));
            }
        };
        // The own bindings, indexed by their own binding number: the validator requires those to be contiguous
        // from zero, so `own[binding]` IS this binding's handle (see resolved_io).
        for (render_resource::pass_binding const& binding : decl.bindings) {
            if (binding.owner != render_resource::binding_owner::own || binding.binding >= io.own.size()) {
                continue;
            }
            check(binding.resource, binding.element, io.own[binding.binding], "own binding");
        }
        // The targets, walked the way `resolve_declaration` walks them: a target claiming a RUN of elements
        // (`render_target::count`) expands to one slot per element, so the declaration's t-th entry is NOT always
        // the t-th slot in `io.targets` - the shadow pass's four cascade layers are one entry there and four here.
        uint32_t target_slot = 0;
        for (render_resource::render_target const& target : decl.targets) {
            for (uint16_t i = 0; i < target.count && target_slot < io.targets.size(); ++i, ++target_slot) {
                check(target.resource, static_cast<uint32_t>(target.element) + i, io.targets[target_slot], "render target");
            }
        }
        for (std::size_t i = 0; i < decl.barrier_images.size() && i < io.barrier_images.size(); ++i) {
            check(decl.barrier_images[i].resource, decl.barrier_images[i].element, io.barrier_images[i], "barrier image");
        }
        for (std::size_t i = 0; i < decl.barrier_buffers.size() && i < io.barrier_buffers.size(); ++i) {
            check(decl.barrier_buffers[i].resource, decl.barrier_buffers[i].element, io.barrier_buffers[i], "barrier buffer");
        }
        this->resource_check_checked += checked;
        this->resource_check_mismatched += mismatched;
        // One line per pass, inside the same window: it says WHICH passes the running scenario put under the
        // check, which is what tells a reader which resolvers the table has been proven against and which ones a
        // different scenario has to exercise.
        if (this->resource_check_frames < 3 && checked > 0 &&
            std::find(this->resource_check_passes.begin(), this->resource_check_passes.end(), &pass) == this->resource_check_passes.end()) {
            this->resource_check_passes.push_back(&pass);
            deren::utility::log("resource table: verified {} declaration handle(s) for pass '{}'", checked, decl.name);
        }
    }

    /// the scene pass's per-frame input: the leaves, the segments, and the three things only the renderer can
    /// answer (see scene_frame). Built here rather than stored, because every field is this frame's.
    // ---- THE VISIBILITY RASTER'S FRAME (visibility-buffer v0.3b-3) -----------------------------------
    // Rebuilt per frame for the reason every frame here is: the recorder walks THIS frame's visible leaves and
    // writes THIS frame slot's item/command lanes, so a frame value that outlived its frame would address the
    // wrong slot. `item_count` is left zero here - the candidate scan answers whether the frame is capturable at
    // all, and the recorder counts what it actually recorded.
    pass::visibility_frame runtime::make_visibility_frame() noexcept {
        pass::visibility_frame frame{};
        frame.record_items = [](void* owner, deren::promise::rhi::command_buffer& command_buffer, std::uint32_t& recorded) {
            return static_cast<runtime*>(owner)->record_visibility_items(command_buffer, recorded);
        };
        frame.owner = this;
        frame.extent = this->render_extent();
        return frame;
    }

    pass::scene_frame runtime::make_scene_frame() noexcept {
        uint32_t const frame_slot = this->frame_ring().position();
        // the pass's view of the per-slot secondary buffers (members, so the span it holds outlives the stage)
        auto const& segments = this->main_segments[static_cast<std::size_t>(frame_slot)];
        this->scene_segment_view.clear();
        this->scene_segment_view.reserve(segments.size());
        for (auto const& buffer : segments) {
            // NO POOL TRAVELS WITH THE BUFFER ANY MORE (the wrapper owns it), and the pass never read
            // one: `segment_buffer` used to carry a VkCommandPool field that no pass touched.
            // abi 21: the secondary is a CALLER-OWNED contract handle, so the frame carries the BORROWED
            // pointer to it (`scene_frame::segment_buffer::buffer` is `rhi::command_buffer*`); the pass
            // derives the native handle it records with through the escape, exactly as this layer does.
            this->scene_segment_view.push_back(pass::segment_buffer{.buffer = buffer});
        }
        // the secondaries inherit the instance's attachments: the three surface targets in order, the velocity
        // target, and the scene colour - the same order the pass's declaration lists them in
        this->scene_color_formats = {deren::engine::render_layout::gbuffer_formats[0], deren::engine::render_layout::gbuffer_formats[1], deren::engine::render_layout::gbuffer_formats[2], deren::engine::render_layout::gbuffer_velocity_format, deren::engine::render_layout::hdr_format};
        return pass::scene_frame{
            .leaves = this->frame_visible,
            .segments = this->scene_segment_view,
            .make_environment = &runtime::make_scene_environment,
            .run_tasks = &runtime::run_scene_tasks,
            .owner = this,
            .color_formats = this->scene_color_formats,
            .depth_format = rhi::image_format::depth,
            .samples = 1u,
            .gbuffer = true,
            .extent = this->render_extent(),
        };
    }

    /// build ONE segment's draw state; a fresh one per segment, because the pipeline-dedup state is per
    /// recording session (see scene_frame::make_environment)
    render_environment runtime::make_scene_environment(void* owner, recording_target target, bool const gbuffer) {
        runtime& self = *static_cast<runtime*>(owner);
        render_environment env;
        // THE SESSION'S BUFFER IS THE CONTRACT HANDLE (abi 21): the caller hands over the same borrowed
        // shared_ptr the pass records into (`resolved_io::cmd`, or the scene/transparent frame's own
        // secondary). A primitive that needs the raw command buffer - the heap push and the mesh entry
        // points are the two today - derives it through `self.native_handle(*env.command_buffer)`.
        env.target = std::move(target);
        // The G-buffer pass binds its own pipeline as the pass default (see gbuffer_pipeline_name): same
        // leaves, same draw path, but the fragment stage writes the surface instead of shading.
        // ... AND WHICH FORM OF IT: the MESH pipeline when the device built one (docs/mesh_shaders.md step 2). The
        // decision is the SESSION's rather than the draw's, because the G-buffer pass draws every leaf with this
        // one pipeline - and a mesh session feeds its leaves differently (see the endpoints below), which is why
        // the two facts travel together here.
        bool gbuffer_mesh = false;
        bool gbuffer_meshlet = false;
        {
            std::shared_lock const lock(self.access_mutex);
            env.default_name = gbuffer ? gbuffer_pipeline_name : self.default_pipeline_name;
            gbuffer_mesh = gbuffer && self.gbuffer_pipeline_mesh.has_value();
            gbuffer_meshlet = gbuffer && self.gbuffer_pipeline_meshlet.has_value();
        }
        env.bind = [&self, &env, gbuffer, gbuffer_mesh, gbuffer_meshlet](recording_target const& session, std::string_view const name) {
            // NO NATIVE HANDLE IS DERIVED HERE ANY MORE (this batch): the bind and the stored viewport/scissor are
            // the record series' own verbs (`pipeline_handle::begin_pipeline`), and the session IS the contract
            // buffer they take.
            if (gbuffer) {
                if (name == gbuffer_pipeline_name) {
                    if (gbuffer_meshlet) {
                        // ONE WORKGROUP PER MESHLET, each culled against the camera before it emits anything
                        // (docs/mesh_shaders.md step 3): the lanes carry the primitive's meshlet run, which is
                        // why the session flag travels with the bind rather than being decided up front.
                        bind_pipeline_on(session, *self.gbuffer_pipeline_meshlet);
                        env.mesh_stage = true;
                        env.meshlets = true;
                    } else if (gbuffer_mesh) {
                        bind_pipeline_on(session, *self.gbuffer_pipeline_mesh);
                        env.mesh_stage = true;
                        env.meshlets = false;
                    } else {
                        bind_pipeline_on(session, *self.gbuffer_pipeline);
                        env.mesh_stage = false;
                        env.meshlets = false;
                    }
                    return;
                }
                // a leaf with explicit pipeline semantics cannot draw in the G-buffer instance (the named
                // pipelines declare the single HDR attachment): say so once per leaf instead of issuing a draw
                // that would be a validation error
                deren::utility::log("runtime: leaf requests pipeline '{}' during the G-buffer pass - draw skipped (only default-semantics leaves write the G-buffer)", name);
                return;
            }
            // THE FORWARD/TRANSPARENT SESSIONS BIND BY NAME, so WHICH FORM was bound is decided HERE rather than per
            // session: a leaf that names a pipeline with a MESHLET form gets one workgroup per meshlet (culled
            // against the camera), failing that a MESH form, and failing that the vertex path - and the session's
            // flags follow the bind, which is what makes a session that mixes the forms correct rather than merely
            // likely.
            if (auto const meshlet = self.meshlet_pipelines.find(name); meshlet != self.meshlet_pipelines.end()) {
                bind_pipeline_on(session, meshlet->second);
                env.mesh_stage = true;
                env.meshlets = true;
                static deren::utility::vessel::array<std::string_view, 8> logged_meshlet = {};
                static std::size_t logged_meshlet_count = 0;
                bool seen_meshlet = false;
                for (std::size_t i = 0; i < logged_meshlet_count; ++i) {
                    seen_meshlet = seen_meshlet || logged_meshlet[i] == name;
                }
                if (!seen_meshlet && logged_meshlet_count < logged_meshlet.size()) {
                    logged_meshlet[logged_meshlet_count++] = name;
                    deren::utility::log("scene: the leaves of '{}' are DISPATCHED per meshlet (meshlet stage)", name);
                }
                return;
            }
            if (auto const mesh = self.mesh_pipelines.find(name); mesh != self.mesh_pipelines.end()) {
                bind_pipeline_on(session, mesh->second);
                env.mesh_stage = true;
                // the meshlet form is the answer above; a session that lands here draws one workgroup per 85
                // triangles, so the flag that says "the lanes carry a meshlet run" has to be cleared, not left over
                // from a previous bind in the same session
                env.meshlets = false;
                // ONE LINE PER NAME, SAYING THE LEAVES ARE DISPATCHED - the same evidence rule the shadow pass's
                // session follows: a mesh stage produces the same picture as the vertex one by construction, so
                // which path drew a frame is deliberately invisible in it (see docs/mesh_shaders.md).
                static deren::utility::vessel::array<std::string_view, 8> logged = {};
                static std::size_t logged_count = 0;
                bool seen = false;
                for (std::size_t i = 0; i < logged_count; ++i) {
                    seen = seen || logged[i] == name;
                }
                if (!seen && logged_count < logged.size()) {
                    logged[logged_count++] = name;
                    deren::utility::log("scene: the leaves of '{}' are DISPATCHED (mesh stage)", name);
                }
                return;
            }
            // A NAME THAT IS NEITHER A MESHLET NOR A MESH FORM IS NOT A PIPELINE ANY MORE (docs/mesh_shaders.md
            // step 4): every geometry name is dispatched, so there is no vertex form left to bind, and the only
            // thing to do with an unknown name is to say so once rather than record a draw with no pipeline.
            env.mesh_stage = false;
            env.meshlets = false;
            if (auto const it = self.pipelines.find(name); it != self.pipelines.end()) {
                bind_pipeline_on(session, it->second); // a non-geometry named pipeline (none today: kept for the registry's sake)
            } else {
                deren::utility::log("runtime: main pass references unknown pipeline '{}' - draw skipped", name);
            }
        };

        // transparent leaves toggle depth writes off via this (core dynamic state, 1.3)
        env.set_depth_write_fn = [](recording_target const& session, bool const enabled) {
            session.set_depth_write(enabled); // the contract's verb takes the bool the environment decided
        };
        // single-sided materials keep back-face culling here (the shadow pass overrides it with env.two_sided;
        // the main pass must not, or double-sided handling would cost fill rate)
        env.set_cull_mode_fn = [](recording_target const& session, rhi::cull_mode const mode) {
            session.set_cull_mode(mode);
        };
        // THE HEAP PUSH (see render_environment::push_block): every draw in this session sends its block through
        // the same endpoint a converted pass uses; that endpoint is what appends the two heap indices, which is
        // why a primitive's own push struct never had to grow a field.
        env.push_owner = &self;
        env.push_block = &runtime::push_stage_block;
        // ... and how a session feeds its leaves: a mesh pipeline has no input assembler, so every leaf is dispatched
        // with its geometry window pushed instead of bound (see primitive::mesh_dispatch). The WINDOW itself is
        // pushed by every session - a vertex pipeline's stages declare the same lanes and a heap pipeline requires
        // them written - so `mesh_stage` is the only per-session flag, and the bind callback above sets it per draw.
        env.mesh_geometry_push_offset = deren::engine::mesh_geometry_push_offset_scene;
        env.buffer_address = &runtime::mesh_buffer_address;
        env.push_at = &runtime::push_geometry_block;
        env.draw_mesh_tasks = &runtime::draw_mesh_tasks;                   // only ever called when mesh_stage is true
        env.draw_mesh_tasks_indirect = &runtime::draw_mesh_tasks_indirect; // a meshlet session's dispatch (see above)
        // ---- ... AND WHERE THE CULLING HAPPENS (docs/mesh_shaders.md step 3, the culling's cheapest stage): the
        //      CAMERA's sessions are culled by the HOST while they are recorded - it already holds the primitive's
        //      meshlets, the draw's model matrix and the camera - so the dispatch asks for the survivors only and a
        //      rejected meshlet costs no workgroup launch at all. The shadow pass deliberately does NOT get this: its
        //      frustum is per cascade, and it dispatches its casters once per cascade.
        env.meshlet_culled = true;
        env.meshlet_view_proj = &runtime::meshlet_view_proj;
        env.meshlet_culled_write = &runtime::meshlet_culled_write;
        env.mesh_stage = gbuffer_mesh;
        return env;
    }

    /// the renderer's scheduler, handed to the pass so the segment fan-out stays the frame loop's policy
    void runtime::run_scene_tasks(void* owner, std::span<std::function<void()>> const tasks) {
        static_cast<runtime*>(owner)->run_tasks(tasks, deren::engine::task_priority::recording);
    }

    // THE SCENE PASS'S RESOLVER IS GONE (S3): its six targets, its shared set and its extent are resolved from
    // its own declaration against the frame's resource table (see pass::resolve_declaration), and the gate that
    // used to be the first line here - "there is no surface pipeline, or no target generation to draw into" - is
    // now the pass's FEATURE ("scene", answered by `gbuffer_pass_active()` in `feature_active`). The generation
    // half of it is what the table already says: a frame whose targets do not exist has no entry to resolve, so
    // the pass does not run. What is NOT resolved here anymore is `out.own = {}` and `out.push = {}` either -
    // this pass declares neither, and the generic resolver leaves both empty for exactly that case.

    pass::transparent_frame runtime::make_transparent_frame() noexcept {
        auto const& secondaries = this->secondary_command_buffers[static_cast<std::size_t>(this->frame_ring().position())];
        // abi 21: the secondary is a CALLER-OWNED contract handle, and the frame hands the SAME shared_ptr
        // over (the pass begins, records and executes it through the contract); no native handle is derived
        // here at all any more - the pass has the escape and does that itself, which is what keeps this
        // frame free of the recording mechanism.
        rhi::mergeable_command_buffer_ref const& transparent = secondaries[static_cast<std::size_t>(secondary_pass::transparent)];
        return pass::transparent_frame{
            .leaves = this->frame_transparent,
            .secondary = transparent,
            .make_environment = &runtime::make_scene_environment,
            .owner = this,
            .color_format = deren::engine::render_layout::hdr_format,
            .depth_format = rhi::image_format::depth,
            .extent = this->render_extent(),
        };
    }

    void runtime::set_gaussian_asset(std::shared_ptr<gaussian_splatting::raster_asset const> asset, glm::mat4 const& model) noexcept {
        this->gaussian_asset = std::move(asset);
        this->gaussian_instances.clear();
        this->gaussian_gpu_unavailable = false;
        this->gaussian_path_reported = false;
        if (this->gaussian_asset) {
            // The application supplies the cloud's placement; CPU and GPU preparation share this instance.
            this->gaussian_instances.push_back(gaussian_splatting::raster_instance{
                .asset = this->gaussian_asset,
                .model = model,
                .stable_id = 0u,
            });
            deren::utility::log("gaussian: the cloud is set for every frame that follows ({} instance(s), {} particle(s))",
                                this->gaussian_instances.size(), this->gaussian_asset->source().particles.size());
        } else {
            deren::utility::log("gaussian: the cloud was cleared; the splat stage now records nothing");
        }
    }

    std::shared_ptr<gaussian_splatting::raster_frame const> runtime::make_gaussian_frame() noexcept {
        if (auto const* mode = std::getenv("DEREN_GAUSSIAN_PATH"); mode && std::string_view{mode} == "gpu")
            return {};
        if (!this->gaussian_path_reported && this->gaussian_asset) {
            deren::utility::log("gaussian path: CPU");
            this->gaussian_path_reported = true;
        }
        // THIS SLOT'S SNAPSHOT, and the slot is the ring's position - which is the only index under which retaining
        // one snapshot per frame is correct (see the member's note: the swapchain image index is NOT it).
        std::shared_ptr<gaussian_splatting::raster_frame>& slot = this->gaussian_frames[static_cast<std::size_t>(this->frame_ring().position()) % this->gaussian_frames.size()];
        if (!this->gaussian_asset) {
            slot.reset();
            return {};
        }
        // THE CAMERA IS THE RENDERER'S OWN, unjittered: the splats are projected from the same view-projection the
        // mesh passes use, and the TAA jitter is deliberately not applied - a splat is not resampled by the
        // temporal resolve (the design turns TAA off while a cloud renders).
        gaussian_splatting::raster_camera camera;
        camera.view_projection = this->current_ubo.view_proj_unjittered;
        camera.position = glm::vec3(glm::inverse(this->current_ubo.view)[3]);
        camera.extent = this->render_extent();
        // THE SLOT'S OWN FRAME IS HANDED BACK FOR REUSE, so a steady-state frame writes into the buffers it already
        // has (4.5 MB at the acceptance cloud, created and released every frame before this) and allocates nothing:
        // the contract's `mapped()` is the write for a `storage_coherent` buffer, and this slot's own frame has
        // completed by the time the ring comes round again, which is the rule that comment states.
        auto const started = std::chrono::steady_clock::now();
        auto const frame = gaussian_splatting::prepare_raster_frame(this->rhi_face(), this->gaussian_instances, camera, gaussian_splatting::foundation_limits{}, slot.get(), this->core_owner);
        this->gaussian_cpu_prepare_ms[static_cast<std::size_t>(this->frame_ring().position())] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (!frame) {
            // THE FRAME IS SKIPPED, NOT FAKED, and the reason is the error's: the pass records nothing this frame
            // and the mesh rendering that surrounds it is untouched (the design's own failure rule).
            deren::utility::log("gaussian: this frame's snapshot was refused (error {}); the splats are skipped and the meshes render as usual",
                                static_cast<int32_t>(frame.error().code));
            slot.reset();
            return {};
        }
        // THE SLOT'S PREVIOUS SNAPSHOT IS DROPPED HERE AND NOWHERE ELSE: this slot's frame has completed, so its
        // buffers are safe to release - and every OTHER slot's snapshot is still held, which is the whole point of
        // the array.
        // ON THE FIRST USE THE SLOT TAKES OWNERSHIP; afterwards it already owns this same frame, and reassigning
        // would release the old value while the returned alias still named it. That is why the assignment is gated.
        if (!slot) {
            slot = std::const_pointer_cast<gaussian_splatting::raster_frame>(*frame);
        }
        return slot;
    }
    std::shared_ptr<gaussian_splatting::gpu_frame const> runtime::make_gaussian_gpu_frame(bool pipelines_ready) noexcept {
        auto const index = static_cast<std::size_t>(this->frame_ring().position()) % this->gaussian_gpu_frames.size();
        auto& slot = this->gaussian_gpu_frames[index];
        // This provider runs after pace_and_acquire completed this slot's previous submission.
        if (slot) {
            auto const stats = slot->completed_stats(this->gaussian_gpu_frame_ids[index]);
            if (stats.errors)
                deren::utility::log("gaussian GPU frame {} skipped: {} numeric errors", stats.frame_id, stats.errors);
        }
        if (!this->gaussian_asset) {
            slot.reset();
            return {};
        }
        auto const* requested = std::getenv("DEREN_GAUSSIAN_PATH");
        std::string_view const mode = requested ? std::string_view{requested} : "auto";
        bool const forced = mode == "gpu";
        if (mode == "cpu" || (!forced && !gaussian_gpu_auto_enabled))
            return {};
        if (this->gaussian_gpu_unavailable)
            return {};
        if (!pipelines_ready) {
            deren::utility::log("gaussian GPU initialization: {} preparation pipelines unavailable", forced ? "FAIL" : "CPU fallback:");
            this->gaussian_gpu_unavailable = true;
            return {};
        }
        gaussian_splatting::raster_camera camera;
        camera.view_projection = this->current_ubo.view_proj_unjittered;
        camera.position = glm::vec3(glm::inverse(this->current_ubo.view)[3]);
        camera.extent = this->render_extent();
        auto const started = std::chrono::steady_clock::now();
        auto prepared = gaussian_splatting::prepare_gpu_frame(this->rhi_face(), this->gaussian_instances, camera, {}, slot.get(), this->core_owner);
        this->gaussian_cpu_prepare_ms[index] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (!prepared) {
            deren::utility::log("gaussian GPU initialization: {} {}", forced ? "FAIL" : "CPU fallback:", prepared.error().message);
            slot.reset();
            this->gaussian_gpu_unavailable = true;
            return {};
        }
        slot = *prepared;
        this->gaussian_gpu_frame_ids[index] = ++this->gaussian_gpu_serial;
        if (!this->gaussian_path_reported) {
            deren::utility::log("gaussian path: GPU ({} candidates)", slot->candidate_count());
            this->gaussian_path_reported = true;
        }
        if (this->gaussian_reported_slot_bytes[index] != slot->byte_size()) {
            this->gaussian_reported_slot_bytes[index] = slot->byte_size();
            std::uint64_t total_bytes = 0;
            for (auto const& frame : this->gaussian_gpu_frames)
                if (frame)
                    total_bytes += frame->byte_size();
            deren::utility::log("gaussian GPU memory: slot {} bytes {}, total completed-slot resources {} bytes", index, slot->byte_size(), total_bytes);
        }
        return slot;
    }

    // THE TRANSPARENT PASS'S RESOLVER IS GONE TOO (S3), for the same reason as the scene pass's: two declared
    // targets, the shared scene block and a full-frame extent, all of them resolved from the declaration. Its gate
    // - "nothing blended this frame" - is the pass's FEATURE ("transparent", answered by `frame_transparent`
    // being empty in `feature_active`), which is the frame's content rather than the declaration's shape.

    pass::character_forward_frame runtime::make_character_forward_frame() noexcept {
        // DOES THE OVERLAY PIPELINE EXIST? Asked HERE rather than left to the session's bind callback, and the
        // difference is a log line per frame rather than a nicety: a name that resolves to nothing is reported by
        // that callback as "unknown pipeline", which is right for a leaf that asked for one and wrong for a pass
        // that is about to discover a build failure it should simply not draw through. So the frame hands over an
        // EMPTY name when the pipeline is absent, and the pass reads that as "draw no overlay" - the documented
        // contract on `character_forward_frame::overlay_pipeline_name`.
        bool overlay_ready = false;
        {
            std::shared_lock const lock(this->access_mutex);
            overlay_ready = this->mesh_pipelines.contains(overlay_pipeline_name) || this->meshlet_pipelines.contains(overlay_pipeline_name);
        }
        // ---- WHICH SHADING MODEL THIS FRAME DRAWS WITH (see runtime::set_goo_toon) ----
        //
        // THE ONE PLACE THE TWO NAMES ARE CHOSEN BETWEEN, and it is here rather than in the pass for the reason
        // the name itself is handed over at all: the registry is the runtime's (see the note below). The choice
        // is made on TWO facts rather than one - the knob AND whether that pipeline exists - because a name that
        // was never registered would be reported by the session's bind callback as an unknown pipeline, once per
        // frame; falling back to the old name instead draws the old chain, which is a valid frame whose only
        // defect is that it is not the experiment that was asked for (and `set_goo_toon` says so, once, when the
        // knob is turned on).
        bool goo_toon_ready = false;
        {
            std::shared_lock const lock(this->access_mutex);
            goo_toon_ready = this->mesh_pipelines.contains(goo_toon_pipeline_name) || this->meshlet_pipelines.contains(goo_toon_pipeline_name);
        }
        std::string_view const toon_pipeline_name = (this->goo_toon_on && goo_toon_ready) ? goo_toon_pipeline_name : character_forward_pipeline_name;
        // DOES THE OUTLINE PIPELINE EXIST? Asked on exactly the same terms as the overlay one above, and for the
        // same reason: an empty name is the pass's documented "draw no outline", whereas a name that resolves to
        // nothing would be reported by the session's bind callback as "unknown pipeline" - a log line per frame
        // for a build failure the pass should simply not draw through.
        bool outline_ready = false;
        {
            std::shared_lock const lock(this->access_mutex);
            outline_ready = this->mesh_pipelines.contains(outline_pipeline_name) || this->meshlet_pipelines.contains(outline_pipeline_name);
        }
        // ---- AND THE OUTLINE LEAVES: THE PER-MATERIAL GATE, WHICH IS NOT THE MESH ----
        //
        // The article's ① 描边 is an inverted hull of a surface THIS PASS ALREADY DRAWS, so the list is a
        // SUBSET OF `frame_visible` and not a parallel list: a leaf whose material states an `_OutlineWidth`
        // is drawn once as the surface (the toon stage) and once as its hull (the outline group), and one that
        // states 0 is drawn only as the surface. `frame_visible` rather than `frame_leaves_buffer` because the hull's
        // outside-the-silhouette ring exists only where the surface it hugs was drawn and wrote depth - a hull
        // for a culled leaf would be an unoccluded shell over the whole frame (see `frame_outline`).
        //
        // THE WIDTH IS READ OFF THE PRIMITIVE (`primitive::outline_width`) rather than out of the material's
        // colour lane, because the lane lives in the GPU table (`deren::engine::render_layout::heap_slots::toon_colours`) and the frame
        // cannot read it without a readback. That mirror is filled from the same `toon_inputs` the shader's lane
        // is built from, so this gate and the mesh stage's own `w > 0` cannot drift apart.
        //
        // THE ORDER IS `frame_visible`'S, for the same reason `frame_overlay` keeps the scene tree's: two hulls
        // that overlap must compose in the order the asset authored them.
        this->frame_outline.clear();
        for (primitive const* const leaf : this->frame_visible) {
            // ... AND NEVER THE FRAME'S STATIC SURROUND: an environment leaf is not drawn by this stage at
            // all (see `frame_character`), and a hull is confined to the outside of the surface this pass
            // just re-shaded - with no re-shaded surface under it a hull would be an unlit shell over the
            // background. The material gate already excludes it (`outline_width` is 0 for the ground and the
            // backdrop), and stating it here keeps the rule in one place rather than in the asset.
            if (!leaf->environment && leaf->outline_width > 0.0f) {
                this->frame_outline.push_back(leaf);
            }
        }
        // THE OPAQUE LEAVES, which are the SCENE pass's - this stage re-shades the surfaces the scene pass
        // already drew, so it must see exactly that list. A leaf the culling dropped has no lit pixel to
        // overwrite, and one it kept but this frame omitted would keep the deferred shading while its
        // neighbours were re-shaded.
        //
        // ... AND IT IS `frame_character` RATHER THAN `frame_visible`, WHICH IS THE ONE DIFFERENCE: the
        // frame's static surround (`[render] background_glb`) is an opaque scene leaf drawn by the scene pass
        // and is deliberately NOT re-shaded here - see `frame_character` for why the reference has no toon
        // material for a ground and a backdrop. Everything else in the list is literally `frame_visible`.
        return pass::character_forward_frame{
            .leaves = this->frame_character,
            // ... AND THE OVERLAY LEAVES, which the collection step has already taken out of `frame_visible`
            // (see `frame_overlay`): they are not surfaces to re-shade, they are the article's two masks to
            // multiply over what this pass just wrote.
            .overlay_leaves = this->frame_overlay,
            // ... AND THE OUTLINE LEAVES (`primitive::outline_width > 0`), which are the SAME leaves this pass
            // re-shades, drawn a second time as their inverted hulls. A subset of `leaves` rather than a list of
            // its own, because an outline is not a different surface - see `frame_outline` for why it follows the
            // CULL as well as the material.
            .outline_leaves = this->frame_outline,
            .make_environment = &runtime::make_scene_environment,
            .owner = this,
            // The name the pipeline was registered under (chores.cpp). Handed over rather than hardcoded in
            // the pass, because the registry is the runtime's: a pass asserting a string another module chose
            // would be a name with no owner.
            //
            // ... AND IT IS THE OLD NAME OR THE REWRITTEN CHAIN'S, decided above from `set_goo_toon` and the
            // registry: the pass draws whatever it is handed, so this one expression IS the whole of the
            // rewrite's opt-in on the host side.
            .pipeline_name = toon_pipeline_name,
            // ... and the overlay group's, on the same terms - empty when it was never built, which the pass
            // reads as "there is no overlay to draw" (see `overlay_ready` above).
            .overlay_pipeline_name = overlay_ready ? overlay_pipeline_name : std::string_view{},
            // ... and the outline group's pipeline name, on the overlay's terms exactly: empty when it was never
            // built, which the pass reads as "there is no outline to draw" (see `outline_ready` above).
            .outline_pipeline_name = outline_ready ? outline_pipeline_name : std::string_view{},
            .color_format = deren::engine::render_layout::hdr_format, // one HDR target, and NOT the swapchain format - see the pass
            .depth_format = rhi::image_format::depth,
            .extent = this->render_extent(),
        };
    }

    // =============================================================================================
    // THE CHAIN OWNER'S SEAM (see runtime::frame_services)
    // =============================================================================================
    //
    // WHAT IS LEFT HERE IS THE THREE FRAMES THAT CARRY THIS RENDERER'S RECORDING MACHINERY, and that is the
    // whole reason to keep any of them: the scene's and the transparent's carry the per-slot secondary buffers,
    // the draw-state factory and the task-pool scheduling those record through, and the shadow's carry the
    // per-cascade secondaries the structure phase recorded. Every OTHER frame is the PASS's own now - the host
    // publishes the few values a pass cannot derive (see `make_frame_facts`) and the pass composes its frame
    // (see `frame_pass::prepare_frame`), which is why this file no longer has a builder per pass and no longer
    // imports the modules of the passes whose frames it stopped naming.

    pass::shadow_frame runtime::make_shadow_frame() noexcept {
        // The per-cascade secondaries live in per-slot vectors (each buffer owns its own command pool,
        // because a pool is not thread safe), so they are NOT contiguous with the frame's other buffers:
        // they are gathered into the scratch array the returned span points at, and that array is a
        // member because a span over a local would dangle the moment this function returned.
        uint32_t const slot = this->frame_ring().position();
        uint32_t const cascades = std::clamp(this->shadow_cascades, 1u, deren::engine::max_shadow_cascades);
        for (uint32_t cascade = 0; cascade < cascades; ++cascade) {
            // abi 21: the per-cascade buffer is a CALLER-OWNED contract handle, and the frame carries the
            // BORROWED POINTER to it (`shadow_frame::cascades` is `std::span<rhi::command_buffer* const>`):
            // the pass begins, records and executes the buffer through the contract, and the owner of the
            // shared_ptr stays this runtime's `shadow_recording`.
            this->shadow_secondaries_scratch[cascade] = this->shadow_recording[slot][cascade];
        }
        return pass::shadow_frame{.record_cascade = &runtime::record_shadow_cascade,
                                  .run_tasks = &runtime::run_shadow_tasks,
                                  .owner = this,
                                  .cascades = std::span<rhi::mergeable_command_buffer_ref const>(this->shadow_secondaries_scratch.data(), cascades),
                                  .map_size = this->shadow_map_size};
    }

    pass::draw_callback runtime::overlay_draw() const noexcept {
        // The trampoline takes the owner as an ARGUMENT rather than using `this`, so the address is a plain
        // function pointer and the hook is one value (see pass::draw_callback).
        return pass::draw_callback{.record = &runtime::draw_overlay_after, .owner = const_cast<runtime*>(this)};
    }

    pass::frame_facts runtime::make_frame_facts() const noexcept {
        // EVERY FIELD IS THE EXPRESSION THE BUILDER IT REPLACED USED - see frame_facts' own note, which names the
        // two where a similarly named feature fact would be a DIFFERENT value (`gi_specular` and `debug_view` are
        // composed, the feature facts of those names are the raw knobs).
        return pass::frame_facts{
            .fxaa_resolves = this->post_fxaa_active(),
            .upscale_resolves = this->post_upscale_active(),
            .debug_view = this->active_features().gbuffer_debug,
            .cluster_count = this->cluster_tiles_x * this->cluster_tiles_y * deren::engine::cluster_slice_count,
        };
    }

    runtime::frame_services runtime::make_frame_services() noexcept {
        // Capture-less lambdas: each one casts the owner back and calls the builder it names, so the owner never
        // sees a member function of this class - it sees the frame. THREE builders, not eleven: the rest of the
        // frames are composed by their own passes from the published facts (see frame_pass::prepare_frame).
        return frame_services{
            .owner = this,
            .make_shadow_frame = [](void* owner) { return static_cast<runtime*>(owner)->make_shadow_frame(); },
            .make_scene_frame = [](void* owner) { return static_cast<runtime*>(owner)->make_scene_frame(); },
            .make_transparent_frame = [](void* owner) { return static_cast<runtime*>(owner)->make_transparent_frame(); },
            .make_character_forward_frame = [](void* owner) { return static_cast<runtime*>(owner)->make_character_forward_frame(); },
            .make_gaussian_frame = [](void* owner) { return static_cast<runtime*>(owner)->make_gaussian_frame(); },
            .make_gaussian_gpu_frame = [](void* owner, bool ready) { return static_cast<runtime*>(owner)->make_gaussian_gpu_frame(ready); },
            .gaussian_timing = [](void* owner) { return gaussian_splatting::gpu_frame_timing{owner, [](void* context, std::uint32_t phase) {
                                                                                                 if (phase < 3u)
                                                                                                     static_cast<runtime*>(context)->gpu_mark(static_cast<gpu_mark_id>(static_cast<std::uint32_t>(gpu_mark_id::gaussian_prepare_end) + phase));
                                                                                             }}; },
        };
    }

    void runtime::bind_frame_chain(pass::pass_chain& chain) noexcept {
        // THE FRAME'S STRUCTURE, and the only thing in this renderer that still names individual passes: which stage
        // holds which pass IS the frame loop's order (it is what the marks, the per-stage preambles and the
        // renderer's own work between the stages are written against), so it is filled here BY DECLARATION NAME out
        // of whichever chain the application handed over. A name the chain does not declare leaves that stage empty,
        // which the record step treats as "no pass here" rather than as an error.
        this->frame_chain = &chain;
        auto const at = [&chain](std::string_view const name) -> pass::frame_pass* { return chain.find(name); };
        this->cluster_pass = {at("cluster")};
        this->shadow_pass = {at("shadow")};
        this->visibility_pass = {at("visibility raster")};
        this->visibility_raster = static_cast<pass::visibility_raster_pass*>(at("visibility raster"));
        // ... and the RESOLVE, the raster lane's declared consumer: bound by its own name, and a chain without one
        // simply leaves the capture lane consumerless (the graph then records it for its own sake, as before v0.4c).
        this->visibility_resolve_stage = {at("visibility resolve")};
        this->visibility_resolve = static_cast<pass::visibility_resolve_pass*>(at("visibility resolve"));
        this->scene_pass = {at("scene")};
        this->transparent_pass = {at("transparent")};
        this->gaussian_pass = {at("gaussian")};
        this->character_forward_pass = {at("character_forward")};
        this->toon_screen_rim_pass = {at("toon_screen_rim")};
        this->goo_rim_pass = {at("goo_rim")};
        this->gbuffer_debug_stage = {at("gbuffer-debug")};
        this->rt_shadow_pass = {at("rt_shadow")};
        this->deferred_pass = {at("deferred")};
        this->taa_pass = {at("taa")};
        this->post_composite_stage = {at("post_composite")};
        this->bloom_pass = {at("post_bloom_0"), at("post_bloom_1"), at("post_bloom_2"), at("post_bloom_3")};
        this->fxaa_pass = {at("fxaa")};
        // ... and the resolve that scales the render chain back up to the output (deren.engine.pass.upscale): the
        // frame's last writer whenever `render_scale < 1.0` and its pipeline exists. It is looked up like every
        // other stage here; the frame loop records it after the FXAA stage, with which it is mutually exclusive.
        this->upscale_pass = {at("upscale")};
        // ... and the stochastic lighting chain's two passes, in the order the FRAME records them: the tracer
        // and its temporal resolve. Two passes rather than one because the frame has an ordering rule to
        // run BETWEEN them - it publishes the G-buffer depth and the motion-vector target that the resolve is the
        // first sampler of - and a frame rule cannot run from inside a chain.
    }

    void runtime::set_pass_chain(pass::pass_chain& chain, chain_wiring const wiring) noexcept {
        // THE HANDOVER: the owner's chain, bound into the frame loop's own structure, plus the wiring that supplies
        // everything the renderer does not know about those passes (see chain_wiring).
        this->bind_frame_chain(chain);
        this->set_chain_wiring(wiring);
    }

    void runtime::set_chain_wiring(chain_wiring const wiring) noexcept {
        this->frame_wiring = wiring;
    }

    void runtime::prepare_stage(pass::stage const& stage) {
        // THE PASSES BUILD THEIR OWN FRAMES from the facts this renderer publishes for THIS STAGE: every field
        // that rebuilds DURING the frame (the structure phase) is the one this call must be late for.
        this->stage_facts = this->make_frame_facts();
        for (pass::frame_pass* const pass : stage.passes) {
            if (pass != nullptr) {
                pass->prepare_frame(this->stage_facts);
            }
        }
        if (this->frame_wiring.prepare == nullptr) {
            return; // no owner: the passes keep the frames they just built and record with them
        }
        this->frame_wiring.prepare(this->frame_wiring.owner, this->make_frame_services(), stage.name);
    }

    void runtime::collect_stage(std::string_view const stage) {
        frame_results results = {};
        if (this->frame_wiring.collect != nullptr)
            this->frame_wiring.collect(this->frame_wiring.owner, stage, results);
        if (results.gaussian_record_error != rhi::error::ok) {
            deren::utility::log("gaussian recording: FAIL error {}; this frame's splats were skipped", static_cast<std::uint32_t>(results.gaussian_record_error));
        }
        // WHAT THE FRAME LOOP DECIDES ON, once per stage that reports: the composite's GI weight is a frame
        // CONSTANT (the composite reads it while recording), and the two flags are the renderer's per-image
        // bookkeeping.
        if (results.taa_wrote_history && this->current_image_index < this->image_view_proj.size()) {
            this->image_view_proj[this->current_image_index] = this->current_ubo.view_proj_unjittered;
        }
    }

    // `runtime::pass_extent` IS GONE (S3.12), and it is the payoff of the last resolver: it was the bridge from a
    // declaration to a size the renderer owns, and it existed because a hand-written resolver had to apply the
    // declaration's own `extent_rule` itself. The framework applies it now (`pass::resolve_extent`, over the
    // `extent_of` callback below), so the bridge had no callers left - the rule is in ONE place, which is what the
    // function's own comment argued for while it was the second copy.

    rhi::image_extent runtime::render_target_extent(rhi::image const* image) const noexcept {
        // THE CREATED IMAGE IS THE ANSWER (③-D/E A1.7, see the declaration): no size arithmetic lives here,
        // because the arithmetic already happened ONCE - when `create_render_chain_targets()` created this
        // image at the frame's render extent or at a size derived from it. An empty manager answers the
        // frame's own extent: that state only exists before the generation is created (or after a creation
        // the backend refused), and answering the frame's extent there is honest rather than a second formula.
        if (!static_cast<bool>(image)) {
            return this->render_extent();
        }
        rhi::image_extent const extent = image->extent();
        return rhi::image_extent{extent.width, extent.height};
    }

    rhi::image_extent runtime::resolve_resource_extent(render_resource::resource_id const id, uint32_t const element) const noexcept {
        switch (id) {
        case pass::resource_id::bloom: {
            // A bloom level is HALF the previous one, created at `max(1, render >> (level + 1))` - and this
            // answers the level's OWN created image rather than computing that formula a second time. The
            // clamp is belt-and-braces rather than the contract: the schema's `count` (4) plus the validator's
            // element check is what limits the level, so the element is in range in every path that reaches
            // here; clamping it keeps an out-of-range answer an extent rather than an out-of-range read.
            std::size_t const level = std::min<std::size_t>(element, this->bloom_images.size() - 1u);
            return this->render_target_extent(this->bloom_images[level][0].get());
        }
        case pass::resource_id::swapchain_image:
            // THE ONE RESOURCE WHOSE EXTENT IS NOT THE FRAME'S. Every other entry here answers a size that is
            // part of the render chain, and the frame's own extent is what a `full` pass gets; this one is the
            // OUTPUT. A pass that declares `extent_rule::resource` over it is asking to cover the presented
            // image while the frame runs at the render extent - which is exactly what the resolve pass that
            // scales the render chain up to the swapchain has to do, and the reason its viewport has to come
            // from its own declaration rather than from the frame.
            // abi 14: the OUTPUT is the contract's own swapchain extent (`swapchain::extent()`), which the
            // engine caches as `presentation_extent()` - not the render extent and never scaled.
            return this->presentation_extent();
        default:
            return rhi::image_extent{}; // an element of a resource whose extent IS the frame's: nothing to answer
        }
    }

    pass::owned_pipeline runtime::resolve_pipeline(std::string_view const name) const noexcept {
        if (pipelines::pipeline_handle const* const pipeline = this->get_pipeline(name); pipeline != nullptr) {
            // ONE LANE, THE CONTRACT'S (abi 21): `pipeline_handle::contract` is set for EVERY pipeline this
            // renderer builds - the graphics recipes, the compute assemblies and the ray-tracing one all come
            // from `api_core::create_pipeline` - and a null contract here is a wiring bug, not a backend without
            // the ability (the record step's raw fallback is gone with the raw lane itself).
            return pass::owned_pipeline{.contract = pipeline->contract};
        }
        // ... AND THEN THE CHAIN'S OWN PASSES, because a chain's stages may SHARE one pipeline: the post chain's
        // four bloom levels record with the composite's R16F variant, and a copy per level would be five identical
        // pipelines. Asking every pass by NAME is what keeps this chain-agnostic - the renderer does not know, and
        // does not need to know, which pass owns what; a pass answers for the names it publishes and nothing else.
        if (this->frame_chain != nullptr) {
            for (pass::frame_pass const* const provider : this->frame_chain->as_stage().passes) {
                if (provider != nullptr) {
                    pass::owned_pipeline const found = provider->named_pipeline(name);
                    if (found.contract != nullptr) {
                        return found;
                    }
                }
            }
        }
        return {};
    }

    pass::resolve_context runtime::make_resolve_context() noexcept {
        return pass::resolve_context{
            .resources = &this->frame_resources,
            // THE FRAME'S IDENTITY: the resolve step needs each pass's extent rule applied to THIS frame
            // (the image index, the slot, the image count and the render extent), and a pass's own override reads
            // it too. Removing this line left the extent rule resolving against a zero extent.
            .frame = this->pass_frame(),
            // THE FRAME'S OWN BUFFER IN THE CONTRACT'S SPELLING (abi 21): the framework copies this value
            // into `resolved_io::cmd` for every pass it resolves (see resolve_declaration), so the two
            // lanes name the same borrowed handle by construction.
            .cmd = this->frame_command_buffer(),
            .list = this->rhi_face().begin_commands(),
            .extent_of = [](void* owner, render_resource::resource_id id, uint32_t element) { return static_cast<runtime*>(owner)->resolve_resource_extent(id, element); },
            .pipeline = [](void* owner, std::string_view name) { return static_cast<runtime*>(owner)->resolve_pipeline(name); },
            .owner = this,
        };
    }

    pass::frame_identity runtime::pass_frame() const noexcept {
        return pass::frame_identity{
            .image_index = this->current_image_index,
            .slot = this->frame_ring().position(),
            // THE CONTRACT'S BOUND, AND THAT IS A MEASURED DECISION RATHER THAN A SUBSTITUTION (③-D/E step
            // 2): `rhi::max_swapchain_images` is the number the contract lets a caller size a per-image
            // array by, and the backend REFUSES a driver that reports more (its own panic) rather than
            // clamping. The actual generation may hold fewer, which is why this is a bound and not a
            // count: it is what a pass must ALLOCATE for, never what it must render - `image_index` is the
            // frame's own image. The legacy runtime read the backend's `swap_chain_images.size()` here;
            // the contract deliberately exposes no such accessor (its note says the abi-17
            // `swapchain::image_count()` route is what that would cost), so the bound it DOES promise is
            // what an engine-held fixed-size array was always sized by (③-D/E A1.4/A1.5).
            .image_count = static_cast<uint32_t>(rhi::max_swapchain_images),
            // THE FRAME'S EXTENT IS THE RENDER EXTENT - this one line is where "the frame's resolution" is
            // defined for every pass (`pass::resolve_extent` reads it for the `full` and `half` rules). It
            // equals the swapchain extent at the default render scale, and below it every `full` pass follows
            // the render chain down together, which is what keeps a lower internal resolution one decision
            // rather than a per-pass one. A pass that has to write the OUTPUT - the resolve that scales the
            // chain back up to what is presented - declares `extent_rule::resource` over
            // `resource_id::swapchain_image` instead, and `resolve_resource_extent` answers with the
            // swapchain's extent.
            .extent = this->render_extent(),
        };
    }

    // ---- the pass host: the record step's callbacks, answered by the renderer ---------------------------------
    //
    // It is rebuilt per call because it is a struct of function pointers (it holds no state of its own); the
    // context is this runtime, which is what each callback casts back to. This is the RECORD STEP's half of the
    // interface and a pass never sees it: at create time a pass is given a `pass_context`, and while recording
    // it is handed `resolved_io`, so it cannot reach a resource its declaration did not name.
    pass::pass_host runtime::make_pass_host() noexcept {
        return pass::pass_host{
            // THE HEAP PUSH EVERY CONVERTED STAGE NEEDS (see resolved_io::push_block): a block sent through
            // vkCmdPushDataEXT, because no heap pipeline has a layout to hold push constants.
            .push_block = {.owner = this, .push = &runtime::push_pass_stage_block},
            .context = this,
            .resolve = [](void* context, pass::frame_pass const& pass, pass::resolved_io& out) { return static_cast<runtime*>(context)->resolve_pass(pass, out); },
            .apply_behaviour = [](void* context, pass::frame_pass const& pass, pass::resolved_io const& io) { static_cast<runtime*>(context)->apply_pass_behaviour(pass, io); },
        };
    }

    bool runtime::resolve_pass(pass::frame_pass const& pass, pass::resolved_io& out) {
        // TWO THINGS WRAP THE FRAMEWORK'S RESOLUTION, and neither belongs there: this frame's shared constants,
        // which every pass is handed whether or not it reads them yet, and the differential check that kept the
        // resource table honest while the resolvers were still here (see verify_resource_table).
        //
        // THE PER-PASS SWITCH IS GONE (S3.12): every pass in this renderer is resolved by its own DECLARATION now
        // (`frame_pass::resolve`), from the resource table the frame publishes - own bindings, targets, barrier
        // entries, the shared sets it names, its own pipeline and the extent from its behaviour's rule. What is
        // left of the renderer's knowledge is the table's CONTENTS (`publish_frame_resources`) and the frame's
        // constants, which is exactly the split this migration was for: a pass cannot reach a resource its
        // declaration does not name, and the renderer no longer knows which pass wants which image.
        out.constants = this->frame_facts; // THE PER-IMAGE TARGET DESCRIPTORS ARE (RE)WRITTEN EVERY FRAME, and that is a MEASURED requirement
        // rather than belt-and-braces: a heap IMAGE descriptor written while its image is still in
        // VK_IMAGE_LAYOUT_UNDEFINED - which is exactly what the creation loops do, in the same breath as
        // vkCreateImage - NEVER RESOLVES. Re-writing the SAME descriptor once the image has been transitioned
        // into a sampled layout samples correctly (measured: the lighting pass read zero from every G-buffer
        // slot until this rewrite existed, and read the real albedo the moment it was added). BUFFERS ARE
        // UNAFFECTED, which is why the material table and the camera/light UBOs worked all along, and why the
        // IBL images worked too - they are uploaded and transitioned before their descriptors are written.
        auto const write_sampled_target = [this](uint32_t const slot, rhi::image const* const image) {
            if (image == nullptr) {
                return;
            }
            [[maybe_unused]] bool const written = contract_write_heap_image(this->rhi_face(), deren::engine::render_layout::heap_slot_offset(slot),
                                                                            *image, rhi::image_view_desc{.layer_count = 0, .mip_count = 0}, rhi::descriptor_type::sampled_image);
        };
        auto const target_image = [](auto const& owned) -> rhi::image const* { return owned.get(); };
        std::size_t const heap_image = static_cast<std::size_t>(this->current_image_index);
        if (heap_image < rhi::max_swapchain_images) {
            uint32_t const image_slot = static_cast<uint32_t>(heap_image);
            // THE G-BUFFER CLUSTER IS THE ENGINE'S OWN (③-D/E A1.4), so every handle below comes through the
            // contract's escape; an empty manager writes nothing, which is what the null-image early return
            // inside `write_sampled_target` already means.
            write_sampled_target(deren::engine::render_layout::heap_slots::gbuffer_albedo + image_slot, target_image(this->gbuffer_images[0][heap_image]));
            write_sampled_target(deren::engine::render_layout::heap_slots::gbuffer_normal + image_slot, target_image(this->gbuffer_images[1][heap_image]));
            write_sampled_target(deren::engine::render_layout::heap_slots::gbuffer_material + image_slot, target_image(this->gbuffer_images[2][heap_image]));
            write_sampled_target(deren::engine::render_layout::heap_slots::gbuffer_depth + image_slot, target_image(this->gbuffer_depth_images[heap_image]));
            write_sampled_target(deren::engine::render_layout::heap_slots::gbuffer_velocity + image_slot, target_image(this->velocity_images[heap_image]));
            write_sampled_target(deren::engine::render_layout::heap_slots::visibility + image_slot, target_image(this->visibility_images[heap_image]));
            write_sampled_target(deren::engine::render_layout::heap_slots::taa_current + image_slot, target_image(this->scene_color_images[heap_image]));
            write_sampled_target(deren::engine::render_layout::heap_slots::post_color + image_slot, target_image(this->hdr_images[heap_image]));
            write_sampled_target(deren::engine::render_layout::heap_slots::display_color + image_slot, target_image(this->ldr_images[heap_image]));
            // ---- THE VISIBILITY RESOLVE'S FIVE STORAGE TARGETS (visibility-buffer v0.4c) ----
            // The SAME images the old G-buffer pass draws into, published as STORAGE descriptors because the resolve
            // is a compute stage and no heap descriptor is both sampled and storage. Written HERE, every frame, for
            // the sampled descriptors' own measured reason (a heap image descriptor written while its image was still
            // UNDEFINED never resolves - see the note above) - and because the SCENE COLOUR target is not one image:
            // on a TAA frame it is the TAA working image (family 5), otherwise the HDR image the lighting pass adds
            // into (family 0), so which image the resolve's fifth output names is this frame's answer.
            auto const write_storage_target = [this](uint32_t const slot, rhi::image const* const image) {
                if (image == nullptr) {
                    return;
                }
                rhi::image_view_desc range{};
                range.role = rhi::view_role::storage;
                [[maybe_unused]] bool const written = contract_write_heap_image(this->rhi_face(), deren::engine::render_layout::heap_slot_offset(slot),
                                                                                *image, range, rhi::descriptor_type::storage_image);
            };
            // ---- ONLY ON A FRAME THAT RUNS THE RESOLVE (item 4's cleanup half): five descriptor writes per
            // frame were issued on EVERY frame, but the only reader is the visibility resolve lane and the
            // images are otherwise untouched by these descriptors. On an OFF frame (and on a fallback frame)
            // the write cannot be observed, so it is the first thing the cleanup removes - measured: OFF and ON
            // pixels unchanged (the writes are descriptor state, not rendering), see the review record.
            if (this->visibility_lane_frame == deren::engine::visibility::opaque_lane::visibility) {
                write_storage_target(deren::engine::render_layout::heap_slots::gbuffer_albedo_storage + image_slot, target_image(this->gbuffer_images[0][heap_image]));
                write_storage_target(deren::engine::render_layout::heap_slots::gbuffer_normal_storage + image_slot, target_image(this->gbuffer_images[1][heap_image]));
                write_storage_target(deren::engine::render_layout::heap_slots::gbuffer_material_storage + image_slot, target_image(this->gbuffer_images[2][heap_image]));
                write_storage_target(deren::engine::render_layout::heap_slots::gbuffer_velocity_storage + image_slot, target_image(this->velocity_images[heap_image]));
                write_storage_target(deren::engine::render_layout::heap_slots::scene_color_storage + image_slot,
                                     target_image(this->graph_taa_frame ? this->scene_color_images[heap_image] : this->hdr_images[heap_image]));
            }
            // THE TAA HISTORY IMAGE IS THE ENGINE'S OWN (③-D/E A1.1): the raw handle comes through the
            // contract's escape, and the guard is the fixed-size array's own VALIDITY rather than a vector's
            // length - `heap_image` is the acquired image index, already inside the rhi::max_swapchain_images
            // bound the frame's end_recording checks.
            if (heap_image < rhi::max_swapchain_images && static_cast<bool>(this->taa_history_images[heap_image])) {
                write_sampled_target(deren::engine::render_layout::heap_slots::taa_history + image_slot, this->taa_history_images[heap_image].get());
            }
            for (uint32_t level = 0; level < deren::engine::render_layout::bloom_level_count; ++level) {
                write_sampled_target(deren::engine::render_layout::heap_slots::bloom_l0 + level * deren::engine::render_layout::heap_image_capacity + image_slot, target_image(this->bloom_images[level][heap_image]));
            }
        }

        bool const resolved = pass.resolve(this->make_resolve_context(), out);

        if (resolved) {
            this->verify_resource_table(pass, out);
        }
        return resolved;
    }

    // THE TAA RESOLVE'S RESOLVER IS GONE (S3), and it is the first pass whose PARAMETERS moved with it: its
    // four own bindings and its HDR target come from the frame's resource table, its pipeline from the pass (it
    // builds its own), its extent from the declaration's `full` rule, and its push block it composes itself out
    // of `resolved_io::constants` (the projection's two depth terms), `io.extent` (the texel size) and its own
    // two blend weights - which `set_taa` now forwards instead of caching them here. Its old gate is answered by
    // the table (the images), the pass's own pipeline (a null one fails the resolution) and the feature "taa".

    void runtime::apply_pass_behaviour(pass::frame_pass const& pass, pass::resolved_io const& io) {
        // The mechanical part of "how this pass is called", done by the record step so that a pass cannot forget
        // it: the pipeline is bound HERE, and the viewport/scissor are set HERE for a pass that asked for them
        // (which is what replaces the hand-kept pipeline list in update_pass_geometry - a pass cannot drop
        // itself from a list it does not maintain).
        //
        // What is deliberately NOT here: the rendering instance. EVERY graphics pass opens its own, over the
        // targets it declared, because the load op and the clear value are the PASS's knowledge. That is why
        // this function no longer has a "not a compute pass" branch: the three graphics kinds differ in how
        // many draws they issue, and only the pass knows that.
        pass::behaviour const& behaviour = pass.behaviour();
        // THE RECORD STEP'S LAST RAW STEP IS GONE (this batch): the viewport/scissor resync below is the record
        // series' own verbs on `io.cmd`, which is the frame's contract buffer - so this function derives no
        // native handle at all any more, and the three `vkCmd*` calls it used to make are two contract verbs.
        if (behaviour.resync_viewport) {
            // io.extent is the extent the declaration's rule produced (the frame's, half of it, or a
            // resource's), so a fullscreen pass gets a viewport that matches the target it declared.
            // BOTH HALVES ARE THE CONTRACT'S NOW (this batch): the two values are the contract's PODs and the two
            // commands are the record series' verbs on the frame's own buffer.
            rhi::viewport const viewport = {.x = 0.0f, .y = 0.0f, .width = static_cast<float>(io.extent.width), .height = static_cast<float>(io.extent.height), .min_depth = 0.0f, .max_depth = 1.0f};
            rhi::rect const scissor = {.offset_x = 0, .offset_y = 0, .width = io.extent.width, .height = io.extent.height};
            io.cmd->set_viewport(viewport);
            io.cmd->set_scissor(scissor);
        }
        // THE CONTRACT, ONE LANE (abi 21): every pipeline a pass resolves is the contract object
        // `command_buffer::bind_pipeline` takes - the graphics recipes, the compute assemblies and the
        // ray-tracing pipeline alike - and the record step has NO raw fallback any more. A declared pipeline that
        // resolved to a null handle is a wiring bug, and it is REPORTED rather than silently recorded without
        // its pipeline (the frame's bind is what makes a pass's draws legal).
        for (deren::promise::rhi::pipeline* const contract : io.pipelines) {
            if (contract == nullptr) {
                deren::utility::log("runtime: the pass declared a pipeline that resolved to nothing - its record may miss its pipeline");
                continue;
            }
            if (io.list->bind_pipeline(*contract) != deren::promise::rhi::error::ok) {
                deren::utility::log("runtime: bind_pipeline refused a contract pipeline the pass declared - its record may miss its pipeline");
            }
        }
    }

    bool runtime::pass_ready(std::string_view const name) const noexcept {
        // The chain's own answer, and it is deliberately the CHAIN's: the pass whose declaration carries that name,
        // asked whether it built what it records with (see frame_pass::ready). This is the first of the renderer's
        // questions moved into the declaration vocabulary - the direction the handover needs, because a renderer
        // handed a chain from outside holds no typed member to ask.
        return this->frame_chain != nullptr && this->frame_chain->ready(name);
    }

    void runtime::fill_compute_skin_requests(std::span<ray_tracing::caster_level const> const casters) {
        // The job's input, built from the map the STRUCTURE SET built: one request per SKINNED caster (a zero
        // destination means "not skinned: its geometry is what the build read"). The list is a member so a frame
        // does not allocate while recording, and the JOB decides what to do with it.
        this->compute_skin_requests.clear();
        this->compute_skin_requests.reserve(casters.size());
        for (auto const& built : casters) {
            if (built.skin_destination_address == 0) {
                continue;
            }
            this->compute_skin_requests.push_back(pass::compute_skin_request{
                .source_vertices = built.skin_source_address,
                .destination = built.skin_destination_address,
                .source_stride = built.skin_source_stride,
                .destination_stride = built.skin_destination_stride,
                .vertex_count = built.skin_vertex_count,
                .skin_base = built.skin_base,
            });
        }
    }

    bool runtime::record_compute_skin_pass(deren::promise::rhi::command_buffer& commands, std::span<ray_tracing::caster_level const> const casters) {
        // The knob and the frame slot are the RENDERER's; the dispatches and the barrier the acceleration
        // structure build needs after them are the JOB's (vulkan.pass.compute_skin_job). "Is there anything
        // skinned at all" is NOT asked here any more: the structure set owns that answer (its map), and it only
        // calls this when it has skinned levels to refit.
        if (!this->rt_skin_bake || !this->compute_skin.ready()) {
            return false;
        }
        this->fill_compute_skin_requests(casters);
        return this->compute_skin.record(commands, this->compute_skin_requests, this, &runtime::push_index_block);
    }

    // THE RAY-TRACED SHADOW PASS'S RESOLVER IS GONE (S3): its one barrier image comes from the frame's resource
    // table (per-frame-slot instance), its two shared sets from the owner, its pipeline from the PASS (which owns
    // it) and its push block is composed by the pass itself out of `resolved_io::constants` - the first pass for
    // which a push block crossed that line. The two halves of its old gate went where the scene pass's did:
    //
    //  * "this slot's visibility image exists" is the RESOURCE TABLE (no entry, no resolution);
    //  * "this slot's top level structure is built, and the pipeline exists" is the pass's FEATURE
    //    (`feature_active("rt_shadow")`). The resource table also publishes the slot's typed AS handle;
    //    feature gating continues to enforce traversal/pipeline readiness.
    //
    // THE ONE THING THAT DID NOT MOVE INTO THE PASS is the pair of `ensure_gbuffer_*_sampled` calls it used to
    // make: they are the frame's *ordering* rule ("whoever samples the G-buffer first publishes the G-buffer
    // instance's attachment writes"), and the flags they read are the renderer's. They now run in the rt_shadow
    // STAGE's preamble in `begin_recording`, immediately before the stage records - which is the same position in
    // the command stream, because the stages carry no marks of their own (`make_pass_host` leaves the mark pair
    // null), so nothing is emitted between them.

    void runtime::record_overlay_if_enabled(rhi::command_buffer& commands) {
        if (this->debug_gui_shown && this->debug_overlay != nullptr && this->debug_overlay->is_active()) {
            this->debug_overlay->record(commands);
        }
    }

    // =============================================================================================
    // THE POST CHAIN (deren.engine.pass.post)
    // =============================================================================================
    //
    // NO RESOLVER IS LEFT FOR IT (S3). The composite's declaration resolves generically except for the one
    // frame-decided choice (which target and pipeline variant, and the push block that follows from it), which
    // its own `resolve` override makes; its four bloom levels and FXAA resolve ENTIRELY from their declarations -
    // the level they write (a `bloom` element), the level they read (the element before it, or nothing at level
    // 0), their extent (the bloom element's size) and
    // the pipeline they record with, which is the COMPOSITE's R16F variant published by name (`post_hdr`) and
    // resolved by asking the chain's passes - see `resolve_pipeline`.
    //
    // WHAT IS NOT RESOLVED, and it is a decision rather than an omission: the HDR target's transition to a sampled
    // layout. The composite reads that image whether or not bloom runs and the bright-pass prefilter reads it too,
    // and on a frame the whole chain is skipped nobody else would move it - so it has exactly one owner and that
    // owner is the frame loop (see render_resource::post_composite_io).

    bool runtime::post_fxaa_active() const noexcept {
        // ONE definition of "the FXAA pass is this frame's last writer": the resolver picks the composite's target
        // and pipeline variant with it, the frame loop decides the overlay's owner with it, the feature registry
        // reports it, and set_fxaa() already folds the pipeline's existence into `fxaa_on` for the same reason. The
        // pipeline is the FXAA PASS's now (deren.engine.pass.fxaa), which is why this asks the pass rather than a member.
        //
        // ... AND IT CANNOT BE TRUE ON A FRAME THE UPSCALE RUNS, which is a CORRECTNESS rule rather than tidiness:
        // the two passes read the SAME LDR image and both want to be the frame's last writer, and FXAA's edge
        // filter is defined on the display-referred image AT THE RESOLUTION IT FILTERS - the render extent. On a
        // scaled frame the upscale resamples that image up to the output, so an FXAA result written at the render
        // extent would be thrown away by the resolve (or, if FXAA won, the resolve would magnify an already
        // filtered image). Both cannot run, and the upscale is the one that must: it is what makes a scaled frame
        // fill the presented image at all.
        return this->fxaa_on && this->pass_ready("fxaa") && !this->post_upscale_active();
    }

    bool runtime::post_upscale_active() const noexcept {
        // ONE definition of "the upscale pass is this frame's last writer", composed from the two facts that
        // decide it: the render chain is running BELOW the output size, and the pass built the pipeline it
        // records with (deren.engine.pass.upscale, created by create_passes()).
        //
        // AT render_scale == 1.0 THE ANSWER IS FALSE BY CONSTRUCTION, and that is the whole reason this
        // predicate is not just `pass_ready`: the render extent returns the swapchain's extent at 1.0, so
        // the resolve would read an image at the output size and write an image at the output size - a
        // same-size resample that costs a pass and changes no pixel. This is also what keeps every scale-1.0
        // capture byte-identical: the record step never resolves or records the stage, the composite never writes
        // the LDR image for it, and the frame is the frame it always was. The scale is the ENGINE's own
        // number now (abi 14, copied from the creation option the context was built with), not a field
        // read out of the context.
        return this->render_scale < 1.0f && this->pass_ready("upscale");
    }

    void runtime::draw_overlay_after(void* const owner, deren::promise::rhi::command_buffer& commands) {
        // The composite's frame carries this as a plain function pointer (a pass records into what it is given and
        // knows nothing about this class), so the member is reached through the context that frame also holds.
        // THE ARCHITECTURE LINE (abi 20): the pass hands the CONTRACT buffer, and the third-party overlay's own
        // recorder - which is not a contract consumer - receives the native handle derived HERE, once, through the
        // same unwrap path every other raw step uses.
        runtime& self = *static_cast<runtime*>(owner);
        self.record_overlay_if_enabled(commands);
    }

    // =============================================================================================
    // THE G-BUFFER DEBUG VIEW (deren.engine.pass.geometry_buffer_debug)
    // =============================================================================================
    //
    // THE G-BUFFER DEBUG VIEW'S RESOLVER AND ITS `ensure_inputs` CALLBACK ARE GONE (S3). Its declaration resolves
    // generically - the HDR display target, the FOUR images it moves to a sampled layout (three stored surface
    // targets and the motion vectors, all per-image families the table publishes),
    // its own pipeline and a full-frame extent - and its push block is the pass's own (its channel, the frame's two
    // projection terms and a motion gain derived from the frame's width). The images themselves are heap slots the
    // shader names. Nothing here is left but the frame's own
    // transitions, which stay in the frame loop as they were: the HDR target's move to a colour attachment has to
    // happen even on a frame the pass cannot draw, because the post chain samples that image.

    void runtime::clear_hdr_for_missing_gbuffer_set() {
        // The debug view did not record - its pipeline is missing (a startup failure), or the frame has no target
        // for it. The pass cannot do this itself, because a pass records nothing when its declaration does not
        // resolve - so the frame's
        // answer lives here: the target is CLEARED, which is what makes such a frame black rather than undefined
        // (the post chain samples that image), and the log line says so once per frame. (The log line's wording is
        // historical: the missing thing used to be a descriptor set, and the frame's answer to a missing one was
        // this same clear.)
        // THE HDR TARGET IS THE ENGINE'S OWN (③-D/E A1.3) now, so the guard is the CONTRACT's bound plus the
        // manager's validity rather than a backend vector's length, and the handle the clear below takes is
        // the CONTRACT image itself.
        uint32_t const index = this->current_image_index;
        if (index >= rhi::max_swapchain_images || !static_cast<bool>(this->hdr_images[index])) {
            return;
        }
        deren::utility::log("runtime: gbuffer debug pass has no descriptor set - showing a cleared frame");
        // undefined -> color_attachment (the recipe the raw transition copied) and the clear itself: both
        // ride the contract, and the clear replaces the hand-built attachment + begin/end pair.
        rhi::command_buffer* const frame_commands = this->frame_command_buffer().get();
        if (frame_commands == nullptr) {
            return; // no frame is in flight: there is no recording buffer to clear through
        }
        rhi::subresource_range const one_layer{.base_mip = 0u, .mip_count = 1u, .base_layer = 0u, .layer_count = 1u};
        if (frame_commands->barrier(rhi::image_barrier{.resource = this->hdr_images[index].get(),
                                                       .from = rhi::image_use::undefined,
                                                       .to = rhi::image_use::color_attachment,
                                                       .range = one_layer}) != rhi::error::ok) {
            deren::utility::log("runtime: the fallback HDR-target transition was refused through the contract");
            return;
        }
        if (frame_commands->clear_color_image(*this->hdr_images[index], deren::utility::vessel::array<float, 4>{}, one_layer) != rhi::error::ok) {
            deren::utility::log("runtime: the fallback HDR-target clear was refused through the contract");
        }
    }

    bool runtime::record_post_process() {
        // NO LEGACY HALF IS LEFT TO FALL BACK TO: eligibility is a READINESS question now (a missing pipeline
        // or overlay pass), and a frame that fails it is reported once and failed rather than recorded by a
        // path that no longer exists.
        if (!this->main_graph_frame) {
            // NOTHING CAN RECORD THIS FRAME (no G-buffer pipeline): DEFINE the frame rather than fail it or present
            // undefined pixels. The swapchain image is transitioned off undefined, cleared to the background colour and
            // left in the colour-attachment state - exactly what the post chain leaves on a normal frame - so
            // end_recording's present transition takes its usual from-state and the frame completes normally. This is
            // the degenerate case a frame loop must survive: a pipeline that failed to build is a log line, not a broken
            // frame (and the independent review of this cleanup measured that the alternative - returning false without
            // a status - presented an UNDEFINED image as a successful frame).
            if (!this->main_graph_not_ready_reported) {
                deren::utility::log("main graph: the frame has no recordable scene (the G-buffer pipeline is missing); presenting the cleared background");
                this->main_graph_not_ready_reported = true;
            }
            auto* const commands = this->frame_command_buffer().get();
            auto* const image = this->rhi_face().frame_image();
            rhi::subresource_range const whole{.base_mip = 0u, .mip_count = 1u, .base_layer = 0u, .layer_count = 1u};
            if (commands == nullptr || image == nullptr)
                return false;
            if (commands->barrier(rhi::image_barrier{.resource = image, .from = rhi::image_use::undefined, .to = rhi::image_use::color_attachment, .range = whole}) != rhi::error::ok)
                return false;
            if (commands->clear_color_image(*image,
                                            deren::utility::vessel::array<float, 4>{this->background_color.r, this->background_color.g, this->background_color.b, 1.0f},
                                            whole) != rhi::error::ok)
                return false;
            // The swapchain image WAS written: end_recording's present transition starts at the attachment state, and
            // post_wrote_swapchain == true is also what keeps the screenshot path's read-back legal on this frame.
            return true;
        }
        if (!this->pass_ready("post_composite")) {
            this->bloom_graph_error = rhi::error::not_ready;
            return false; // no post pipeline (creation failed): the HDR frame cannot be presented correctly
        }
        // THE ONE RECORDING PATH: the composed graph records the scene chain and the post chain
        // (record_post_graph's own comment states the lanes). The legacy scene tail and the legacy post
        // stages this function used to fall back to are gone.
        this->bloom_graph_error = this->record_post_graph();
        if (this->bloom_graph_error != rhi::error::ok)
            deren::utility::log("post graph: recording refused ({})", static_cast<std::uint32_t>(this->bloom_graph_error));
        return this->bloom_graph_error == rhi::error::ok;
    }
    frame_status runtime::end_recording() {
        deren::engine::profiling::cpu_phase_timer const phase_timer{this->cpu_timings, deren::engine::profiling::cpu_phase::post};
        rhi::api_core& vk = this->vulkan_core;
        if (!this->frame_command_buffer()) {
            return frame_status::end_recording_failed; // no frame is open: there is nothing to close
        }

        // close the scene rendering instance and run the post-process pass (exposure/tonemap).
        // The return value says whether a fullscreen pass actually wrote the swapchain image: only
        // then is it in GENERAL and only then does it hold this frame's result.
        bool const post_wrote_swapchain = this->record_post_process();
        if (this->bloom_graph_error != rhi::error::ok) {
            return frame_status::end_recording_failed;
        }
        // Screenshot: while the post pass wrote the swapchain image it is still in GENERAL and still owned
        // by this frame - the only point where a read-back copy is legal. Doing it here (rather than after
        // the present) also means the capture needs no extra submit, no re-acquire and no layout hand-back
        // to the WSI.
        //
        // S2 BATCH 2: THE COPY IS THE FIRST FULLY CONTRACT-RECORDED OPERATION OF THIS FRAME. The list is
        // the frame's own primary command buffer (borrowed from the backend), the image is the frame image,
        // the destination is the backend's read-back slot, and the two use() calls are the layout pair the
        // hand-written version carried - the shadow gate (A5) proves they land on the same barriers. THE
        // SUBMITTER DOES NOT CHANGE: this runtime submits the same command buffer below.
        if (post_wrote_swapchain && this->screenshot_requested) {
            deren::promise::rhi::command_buffer* const commands = vk.begin_commands();
            deren::promise::rhi::image* const frame_image = vk.frame_image();
            deren::promise::rhi::buffer* const slot = vk.frame_readback_buffer();
            if (commands != nullptr && frame_image != nullptr && slot != nullptr) {
                deren::promise::rhi::image_copy_region const region{.extent = frame_image->extent()};
                // BOTH BARRIERS ARE CHECKED (the recording surface answers with an error instead of
                // dropping one silently - a dropped barrier leaves the image in a state nobody declared,
                // which is the failure class task-148 measured). A refused first barrier skips the copy:
                // there is no point recording a copy whose entry barrier never happened.
                deren::promise::rhi::error const to_source =
                    commands->use(*frame_image, deren::promise::rhi::image_use::color_attachment, deren::promise::rhi::image_use::transfer_source);
                deren::promise::rhi::error copied = to_source;
                deren::promise::rhi::error hand_back = deren::promise::rhi::error::ok;
                if (to_source == deren::promise::rhi::error::ok) {
                    copied = commands->copy_image_to_buffer(*slot, *frame_image, region);
                    // The image gets its render-target role back even when the copy was refused: the
                    // present transition below assumes the frame's own state, and the refusal is reported
                    // rather than swallowed.
                    hand_back = commands->use(*frame_image, deren::promise::rhi::image_use::transfer_source, deren::promise::rhi::image_use::color_attachment);
                }

                if (to_source != deren::promise::rhi::error::ok) {
                    // The capture is dropped for this frame; the request stays pending, so the next frame
                    // retries - and the refusal is on the record instead of being silently ignored.
                    deren::utility::log("screenshot: the read-back's first barrier was refused ({}); this frame's capture is dropped",
                                        static_cast<std::uint32_t>(to_source));
                } else if (hand_back != deren::promise::rhi::error::ok) {
                    deren::utility::log("screenshot: the read-back's hand-back barrier was refused ({}); this frame's capture is dropped",
                                        static_cast<std::uint32_t>(hand_back));
                } else if (copied == deren::promise::rhi::error::ok) {
                    this->screenshot_pending = true;
                    this->screenshot_requested = false; // served
                } else if (copied == deren::promise::rhi::error::unsupported) {
                    // This surface cannot be a copy source at all (the swapchain images lack TRANSFER_SRC),
                    // so the copy would violate VUID-vkCmdCopyImageToBuffer-srcImage-00186. The JUDGEMENT
                    // is the backend's - the swapchain is its object - and the one-time log and the F12
                    // shutdown that go with it stay here, where they always were. A permanent condition:
                    // retrying every frame would only spam.
                    if (!this->screenshot_unsupported_logged) {
                        this->screenshot_unsupported_logged = true;
                        deren::utility::log("screenshot: unsupported (swapchain has no TRANSFER_SRC usage) - F12 disabled");
                    }
                    this->screenshot_requested = false;
                }
                // not_ready / invalid_argument: the request stays pending and the next frame retries, which
                // is the posture the hand-written version had ("a failed copy stays pending").
            }
        }
        // Dynamic rendering has no render pass finalLayout to hand the image back to the
        // presentation engine: transition the swapchain image to PRESENT_SRC_KHR explicitly. The
        // post pass leaves the image in GENERAL, so the barrier is needed whenever
        // it ran. When it was skipped the image never entered GENERAL, and claiming
        // that old layout would be a lie (validation: "oldLayout is not matching with the current
        // layout"): transition from UNDEFINED instead - the frame has no content to preserve anyway.
        // THE PER-IMAGE INDEX IS CHECKED BEFORE IT IS USED (③-D/E item A1: the contract's
        // `rhi::max_swapchain_images` promise). Every per-image array in this renderer is sized by that
        // constant, and the engine only ever indexes with the index the ACQUIRE answered - so an index at or
        // past the bound is a broken invariant, and it is a PANIC carrying both numbers rather than a silent
        // out-of-range read, a clamp or a wrap.
        if (this->current_image_index >= rhi::max_swapchain_images) {
            deren::utility::panic(std::source_location::current(),
                                  "runtime: the acquired image index {} is at or past rhi::max_swapchain_images ({}) - "
                                  "the per-image arrays are sized by the contract's bound, so this is a broken invariant "
                                  "rather than something to clamp",
                                  this->current_image_index,
                                  rhi::max_swapchain_images);
        }
        // color_attachment -> present when the post chain wrote the swapchain, undefined -> present when it
        // was skipped (the two recipes the raw presentation barrier chose between). The image is the frame's
        // OWN contract handle (`frame_image()`), which the backend's registry accepts by identity.
        if (this->post_graph_drew_frame) {
            this->bloom_graph_error = this->finish_post_graph();
            if (this->bloom_graph_error != rhi::error::ok) {
                return frame_status::end_recording_failed;
            }
        } else
            (void)this->frame_command_buffer()->barrier(rhi::image_barrier{.resource = this->rhi_face().frame_image(),
                                                                           .from = post_wrote_swapchain ? rhi::image_use::color_attachment : rhi::image_use::undefined,
                                                                           .to = (this->create_options.native_window == nullptr) ? rhi::image_use::shader_read : rhi::image_use::present,
                                                                           .range = rhi::subresource_range{.base_mip = 0u, .mip_count = 1u, .base_layer = 0u, .layer_count = 1u}});
        // GPU timing: last mark of the frame. The interval it closes is everything after the FXAA
        // (or composite) pass - the screenshot read-back copy and the present barrier.
        this->gpu_mark(gpu_mark_id::frame_end);
        // THE OTHER END OF THE LIFECYCLE (abi 26): `end_recording()` closes the buffer the BEGIN opened above,
        // and the backend performs `vkEndCommandBuffer` - so no `VK_SUCCESS` stands between the frame's last
        // mark and the present transition any more.
        if (auto const frame_commands = this->frame_command_buffer(); !frame_commands || frame_commands->end_recording() != rhi::error::ok) {
            return frame_status::end_recording_failed;
        }
        return frame_status::proceed;
    }

    frame_status runtime::submit_and_present() {
        this->cluster_retention_open = false;
        if (this->bloom_graph_error != rhi::error::ok) {
            return frame_status::submit_failed;
        }
        // The resource table's differential check reports itself HERE, once, at the end of the frame that closes
        // its window - the last point of a frame, so the counters cover EVERY stage (the post and lighting stages record
        // inside `end_recording`, which is why this cannot sit there). THREE frames rather than one, because a
        // pass may legitimately resolve only from the second frame on (TAA needs a history), and a check that
        // watched one frame would report those as uncovered. See verify_resource_table: a mismatch is a table
        // entry to fix before that pass's resolver can be deleted.
        ++this->resource_check_frames;
        if (!this->resource_check_reported && this->resource_check_frames >= 3) {
            this->resource_check_reported = true;
            deren::utility::log("resource table: {} entries published, {} declaration handle checks against the resolvers, {} mismatch(es) over {} frame(s)",
                                this->frame_resources.size(),
                                this->resource_check_checked,
                                this->resource_check_mismatched,
                                this->resource_check_frames);
        }
        deren::engine::profiling::cpu_phase_timer const phase_timer{this->cpu_timings, deren::engine::profiling::cpu_phase::submit};

        // SUBMIT + PRESENT GO THROUGH THE CONTRACT (abi 14): the frame verb the contract was missing
        // is here, so the engine no longer names the backend's submission primitive. The list is the
        // backend's own borrowed recording view (`begin_commands()` - the same list this frame's
        // recording verbs were handed), and the IMAGE plus the present-ready semaphore are the
        // backend's own acquire state: this object never touches either. The command buffer the frame
        // RECORDED into is the one `begin_commands()` names (its raw handle came from that same buffer
        // through `native_frame_commands()` - see `frame_command_buffer`), so the verb submits exactly what
        // was recorded.
        // Acquire, CPU preparation and recording overlap the preceding host presentation call.
        // Collect its result before another queue submission; only the caller rebuilds resources.
        rhi::error preceding_present = rhi::error::ok;
        if (this->pending_present.valid()) {
            auto const waiting = std::chrono::steady_clock::now();
            preceding_present = this->pending_present.get();
            this->pending_present = {};
            this->cpu_timings.add(deren::engine::profiling::cpu_phase::present, std::chrono::steady_clock::now() - waiting);
            if (preceding_present != rhi::error::ok && preceding_present != rhi::error::out_of_date) {
                return frame_status::present_failed;
            }
        }
        auto const submit_started = std::chrono::steady_clock::now(); // sub-phase marks: what of submit is the
        // queue submission (with its timeline signal) and what is the present call below
        rhi::command_buffer* const commands = this->rhi_face().begin_commands();
        if (commands == nullptr) {
            // A frame that reached the submit point is the frame `wait_and_acquire()` opened, so this
            // is an invariant, not a state a caller can produce: refusing by not submitting is the
            // same answer `submit()` itself would give (`not_ready`), and it never appears as a
            // successful frame.
            deren::utility::log("runtime: submit called with no frame in flight");
            return frame_status::submit_failed;
        }
        rhi::error const submitted = this->rhi_face().submit(*commands);
        if (submitted != rhi::error::ok) {
            // The contract's answer names WHAT failed (device_lost, out_of_*_memory, ...) - the
            // information the old `!= VK_SUCCESS` test threw away.
            deren::utility::log("runtime: frame submit was refused ({})", static_cast<std::uint32_t>(submitted));
            return frame_status::submit_failed;
        }
        if (this->frame_chain != nullptr)
            this->submitted_resource_views[this->frame_ring().position()] = this->frame_chain->resources().snapshot();
        if (this->bloom_graph_pending)
            for (auto const& generation : this->bloom_graph_pending_generations)
                if (generation)
                    this->bloom_graph_states[weak_bloom_generation{generation}][this->current_image_index] = true;
        this->submitted_fallback_views[this->frame_ring().position()] = this->shared_resource_fallback.resources().snapshot();
        this->cpu_timings.add(deren::engine::profiling::cpu_phase::submit_queue, std::chrono::steady_clock::now() - submit_started);
        auto const present_started = std::chrono::steady_clock::now();
        // THE PRESENT LADDER GOES THROUGH THE ERROR MECHANISM: `present()` answers the contract's
        // vocabulary, and the translation is the backend's at the present call site - SUBOPTIMAL is
        // out_of_date HERE (the frame showed, but the surface is one resize from gone), the reading
        // the acquire site must NOT inherit. The engine reads the code; it never touches a VkResult
        // and never names the image: the backend presents the image its own acquire took.
        rhi::error presented = preceding_present;
        auto* const overlap = this->async_present_enabled
                                  ? static_cast<rhi::async_presentation*>(this->rhi_face().query_extension(rhi::extension_kind::async_presentation))
                                  : nullptr;
        if (presented == rhi::error::ok && overlap != nullptr) {
            if (!this->async_present_reported) {
                deren::utility::log("async present: enabled; snapshot on caller, collect before next submit");
                this->async_present_reported = true;
            }
            this->pending_present = overlap->begin();
            if (!this->pending_present.valid())
                return frame_status::present_failed;
        } else if (presented == rhi::error::ok) {
            presented = this->create_options.native_window == nullptr ? rhi::error::ok : this->rhi_face().present();
        }
        this->cpu_timings.add(deren::engine::profiling::cpu_phase::present, std::chrono::steady_clock::now() - present_started);
        if (presented == rhi::error::out_of_date) {
            deren::utility::log("present out of date, recreating swapchain");
            // THE REBUILD IS THE SWAPCHAIN'S OWN VERB (abi 14), and only a generation that was
            // actually rebuilt (`ok`; `not_ready` is the deferred state) invalidates the per-image
            // state - exactly the condition the raw bool used to spell.
            // THE ENGINE'S PRESENTATION-IMAGE VIEWS GO FIRST (see release_frame_swapchain_views): the
            // rebuild destroys the images they were made over.
            this->release_frame_swapchain_views();
            rhi::swapchain* const surface = this->rhi_face().frame_swapchain();
            if (surface != nullptr && surface->recreate() == rhi::error::ok) {
                this->on_swapchain_recreated();
            }
        } else if (presented != rhi::error::ok) {
            return frame_status::present_failed;
        }
        // THE RING ADVANCES THROUGH THE FACE (today's to_next_frame, walked, not poked).
        auto const now = std::chrono::steady_clock::now();
        if (this->gaussian_last_present != std::chrono::steady_clock::time_point{})
            this->gaussian_frame_wall_ms[static_cast<std::size_t>(this->frame_ring().position())] = std::chrono::duration<double, std::milli>(now - this->gaussian_last_present).count();
        this->gaussian_last_present = now;
        if (std::getenv("DEREN_GAUSSIAN_RESIZE_TRACE"))
            deren::utility::log("gaussian completed frame: {}", ++this->gaussian_trace_frames);
        this->frame_ring().walk_to_next();
        return frame_status::proceed;
    }

    frame_status runtime::render_frame() {
        // Whole frame in one call: run the frame phases directly, in order, with no caller
        // writes interleaved (callers that need per-frame host updates run the phases at fine
        // granularity themselves, writing between pace_and_acquire() and begin_recording()).
        frame_status const skip = this->poll_events();
        if (skip != frame_status::proceed) {
            return skip;
        }
        this->recreate_if_minimized();
        frame_status const paced = this->pace_and_acquire();
        if (paced != frame_status::proceed) {
            return paced;
        }
        frame_status const begin = this->begin_recording();
        if (begin != frame_status::proceed) {
            return begin;
        }
        this->record_main_drawcalls();
        frame_status const end = this->end_recording();
        if (end != frame_status::proceed) {
            return end;
        }
        return this->submit_and_present();
    }
} // namespace deren::engine
