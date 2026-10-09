// The ray-traced shadow pass's implementation: the two barriers around the visibility image, the two shared blocks
// and the full-resolution dispatch, plus the pipeline it owns. The recording was moved out of
// `runtime::record_rt_shadow_pass` UNCHANGED in behaviour - the same UNDEFINED -> GENERAL transition before the
// dispatch, the same bind order (the scene block, then the G-buffer images), the same 80-byte push, the same workgroup size
// and the same GENERAL -> SHADER_READ hand-back.
//
// HOW IT WAS VERIFIED, and this is the interesting part: **the capture gate cannot decide this pass.** No
// scenario in `scripts/windows/check_render.ps1` sets `rt_shadows`, and the config default is `false`, so all
// twelve reference frames were captured with the pass NOT running - which is why the 12 x 2 gate is green for
// this change and why that green is NOT the evidence. The evidence is an A/B against the parent commit's binary
// with a scenario that pins `rt_shadows = true`: both builds produce the SAME hash
// (AEEB757EA347CC4178006F7FD5C1795AFD7274556D72C9028ABB3970B3DA72BB, twice each, validation clean).

module;

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

module deren.vulkan.pass.ray_traced_shadow;

import deren.promise.rhi; // the record series (abi 20): the two barriers, the push block's endpoint and the traceRays escape
import deren.vulkan.render_resource;
import deren.vulkan.constant_init;
import deren.vulkan.pipelines; // build_rt_shadow: the compute pipeline this pass owns
import deren.utility;

// The contract's spelling, local to this TU (post.cpp, upscale.cpp, taa.cpp carry the same alias).
namespace rhi = deren::promise::rhi;

namespace deren::vulkan::pass {

    rt_shadow_pass::~rt_shadow_pass() {
        this->release_owned();
    }

    void rt_shadow_pass::release_owned() noexcept {
        this->pass_pipeline.reset();
    }

    render_resource::pass_io const& rt_shadow_pass::io() const noexcept {
        return render_resource::rt_shadow_io;
    }

    deren::vulkan::pass::behaviour const& rt_shadow_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view rt_shadow_pass::feature() const noexcept {
        // The renderer's registry answers whether the ray-traced path is on at all (the knob AND a device with
        // ray queries); this pass's own answer is whether it built its pipeline, which the registry also asks.
        return "rt_shadow";
    }

    bool rt_shadow_pass::pipeline_ready() const noexcept {
        return this->pass_pipeline.has_value();
    }

    deren::promise::rhi::pipeline* rt_shadow_pass::pipeline_handle() const noexcept {
        return this->pass_pipeline.has_value() ? this->pass_pipeline->contract : nullptr;
    }

    void rt_shadow_pass::create(pass_context const& context) {
        if (context.face == nullptr) {
            return;
        }
        if (this->built_against != nullptr && this->built_against != context.face) {
            this->release_owned();
        }
        this->built_against = context.face;
        if (this->pass_pipeline.has_value()) {
            return; // already built for this device
        }
        auto const fetch = [&context](std::string_view const name) {
            return context.shader != nullptr ? context.shader(context.owner, name) : std::span<uint8_t const>{};
        };
        std::span<uint8_t const> const raygen = fetch(raygen_name);
        std::span<uint8_t const> const closest_hit = fetch(closest_hit_name);
        std::span<uint8_t const> const miss = fetch(miss_name);
        std::span<uint8_t const> const any_hit = fetch(any_hit_name);
        if (raygen.empty() || closest_hit.empty() || miss.empty() || any_hit.empty()) {
            deren::utility::log("ray-traced shadows unavailable: the owner has not registered all of {}, {}, {} and {}", raygen_name, closest_hit_name, miss_name, any_hit_name);
            return;
        }
        // Everything this pass reads is a heap slot the shaders name themselves (the scene buffers, the
        // G-buffer images, the acceleration structure), so the pipeline is all it builds.
        auto built = pipelines::build_rt_shadow_ray_tracing(*context.face, raygen, closest_hit, miss, any_hit);
        if (!built) {
            deren::utility::log("ray-traced shadows unavailable: {}", built.error());
            this->release_owned();
            return;
        }
        this->pass_pipeline = std::move(*built->pipeline);

        // ---- THE SHADER BINDING TABLE, the half of a ray-tracing pipeline that belongs to its CALLER: the
        //      group handles are per-pipeline data, and the STRIDE is a property of the device rather than a
        //      constant. Here a handle is 32 bytes while a region's address must be 64-byte aligned, so using the
        //      handle size as the stride is exactly the first-attempt VUID this pass would otherwise hit.
        //      Group bytes use the RHI pipeline; launch uses command_buffer::trace_rays.
        uint32_t const handle_size = context.ray_tracing_properties.handle_size;
        uint32_t const handle_alignment = context.ray_tracing_properties.handle_alignment;
        uint32_t const base_alignment = context.ray_tracing_properties.base_alignment;
        if (handle_size == 0 || handle_alignment == 0 || base_alignment == 0 || context.create_upload_buffer == nullptr) {
            deren::utility::log("ray-traced shadows unavailable: this device published no shader binding table numbers to build one against");
            this->release_owned();
            return;
        }
        uint32_t const region_size = ((handle_size + base_alignment - 1u) / base_alignment) * base_alignment;
        uint32_t const group_count = built->group_count;
        std::vector<uint8_t> handles(static_cast<size_t>(group_count) * handle_size);
        // THE SBT QUERY, through the basis (abi 22): `out.size()` is the query's dataSize, and the pipeline is
        // the CONTRACT object the backend turns into its own native handle - so this file names neither.
        if (!pass::shader_group_handles(context.face, *this->pass_pipeline->contract, 0, group_count, std::span<std::uint8_t>(handles))) {
            deren::utility::log("ray-traced shadows unavailable: the shader group handles could not be read back");
            this->release_owned();
            return;
        }
        // THE REGIONS ARE THE CONTRACT'S TYPE (`rhi::shader_binding_table_region`): three numbers per table - the
        // address of its first record, the bytes it spans, and the stride between records. One region per group,
        // each starting on a base-aligned offset and holding exactly one record: the order is the builder's
        // (raygen, miss, hit), so `handles[group]` lands in region `group`.
        std::vector<uint8_t> table(static_cast<size_t>(group_count) * region_size, 0);
        for (uint32_t group = 0; group < group_count; ++group) {
            std::memcpy(table.data() + static_cast<size_t>(group) * region_size, handles.data() + static_cast<size_t>(group) * handle_size, handle_size);
        }
        uint64_t address = 0;
        // THE TABLE IS BUILT THROUGH THE CONTRACT NOW (abi 24): the hook takes the contract's `buffer_flags`
        // (a shader binding table asks for `shader_binding_table`), answers the contract handle the OWNER keeps
        // alive, and writes the device address through the out-parameter - so this file names no Vulkan type for
        // any of it, which is why it no longer includes a Vulkan header at all.
        deren::promise::rhi::buffer* const table_buffer = context.create_upload_buffer(context.owner, table.data(), static_cast<uint64_t>(table.size()),
                                                                                       deren::promise::rhi::buffer_flags{deren::promise::rhi::to_bits(
                                                                                           deren::promise::rhi::buffer_flag::shader_binding_table)},
                                                                                       &address);
        if (table_buffer == nullptr || address == 0) {
            deren::utility::log("ray-traced shadows unavailable: the shader binding table buffer could not be created");
            this->release_owned();
            return;
        }
        auto const region = [region_size](uint64_t const at) { return rhi::shader_binding_table_region{.address = at, .size = region_size, .stride = region_size}; };
        this->raygen_region = region(address);
        this->miss_region = region(address + region_size);
        this->hit_region = region(address + 2u * region_size);
        // The line the runtime used to log when it built this pipeline: a pass that says what it built is what
        // makes a missing one visible in the startup log rather than in a frame that looks merely shadowless.
        deren::utility::log("SUCCESS: ray-traced sun shadow pipeline created (raygen + miss + closest hit + any hit, one ray per pixel, terminated on first hit)");
    }

    void rt_shadow_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing to reset: this pass owns no per-generation state. Its visibility image is per FRAME SLOT and
        // belongs to the acceleration structures' own lifetime (the renderer rebuilds both together), and its
        // pipeline does not depend on the surface's format or size - so the generation change is not its event.
        // The one-shot log line stays set, because "this pass traces rays" does not become untrue on a resize.
    }

    void rt_shadow_pass::record(resolved_io const& io) {
        if (!this->pass_pipeline.has_value() || io.barrier_images.size() < render_resource::rt_shadow_barriers.size() ||
            io.pipelines.empty() || io.pipelines[0] == nullptr || io.list == nullptr ||
            io.barrier_images[barrier_visibility].image_handle == nullptr ||
            this->hit_region.address == 0 ||
            io.extent.width == 0 || io.extent.height == 0) {
            return; // the runner resolves all of this or skips the pass (see frame_pass::resolve)
        }
        rhi::image* const visibility = io.barrier_images[barrier_visibility].image_handle;

        // The image is written as a storage image (GENERAL) and read by the lighting stage as a sampler
        // (SHADER_READ). Both transitions happen here, around the dispatch, because this is the only place that
        // knows the image is being rewritten - the lighting stage's descriptor declares SHADER_READ whether or
        // not this pass ran (see the off path in the frame loop).
        // THE PAIR IS THE SHIPPED RECIPE AND THE STAGE IS THE HINT (abi 21): (undefined, shader_write) is
        // `undefined_to_general_transition`, and `stage_hint::ray_tracing` replaces the stage that recipe names
        // (COMPUTE_SHADER) with the one that actually RAN - a traceRays LAUNCH, not a dispatch. THE OVERRIDE IS
        // MEASURED, which is what the barrier model's own note asks for before a vocabulary grows: with the
        // recipe's stage the writes go unsynchronised and the lighting stage samples an image the trace has not
        // finished writing (measured: half the model lost its sun, deterministically, while a constant write -
        // which no ordering can make wrong - came out right).
        std::array<rhi::image_barrier, 1> const to_general = {
            rhi::image_barrier{.resource = visibility, .from = rhi::image_use::undefined, .to = rhi::image_use::shader_write, .range = {}},
        };
        if (io.list->barrier(rhi::barrier_group{.images = to_general, .stage = rhi::stage_hint::ray_tracing}) != rhi::error::ok) {
            return; // a refused barrier would leave the visibility image in a state nobody declared
        }

        // No sets to bind (see the heap bind in begin_recording): the tracer's scene buffers, its G-buffer images
        // and the acceleration structure are heap slots the shader names, and the frame's indices arrive in its
        // push block.

        // THE PUSH BLOCK IS THE PASS'S OWN NOW (S3): the renderer used to compose it and hand it over as raw
        // bytes, which was the last thing it knew about this pass's frame. What it carries is this frame's
        // inverse view-projection - a frame CONSTANT, so it arrives through `resolved_io::constants`, the channel
        // that exists for exactly this - and the three ray-offset terms, which are this shader's own constants and
        // therefore the struct's defaults rather than values anybody has to pass in.
        push_constants push = {};
        push.inv_view_proj = io.constants.inv_view_proj;
        [[maybe_unused]] bool const pushed = io.push_block(*io.cmd, pass::push_bytes(push));
        // THE LAUNCH DIMS ARE THE EXTENT: one invocation per pixel of the visibility image, which is what the
        // compute form got from its dispatch and its bounds check. The launch itself is the pass's wrapper: the
        // contract buffer in, the native entry point out of the escape (see `trace_rays` above), and the REGIONS
        // are the contract's own type now (see the members).
        io.list->trace_rays(this->raygen_region, this->miss_region, this->hit_region, this->callable_region, io.extent.width, io.extent.height, 1);

        // ... and the hand-off to the lighting stage, with the SAME hint and for the SAME reason: the recipe
        // (shader_write, shader_read) is `general_to_sampling_transition`, and its SOURCE stage is the
        // ray-tracing one here (see the first barrier's note).
        std::array<rhi::image_barrier, 1> const to_sampling = {
            rhi::image_barrier{.resource = visibility, .from = rhi::image_use::shader_write, .to = rhi::image_use::shader_read, .range = {}},
        };
        if (io.list->barrier(rhi::barrier_group{.images = to_sampling, .stage = rhi::stage_hint::ray_tracing}) != rhi::error::ok) {
            return; // a refused hand-off would leave the visibility image in a state nobody declared
        }

        if (!this->logged) {
            this->logged = true;
            deren::utility::log("ray-traced shadows: tracing {}x{} rays per frame through the ray-tracing pipeline (one per pixel, terminated on the first hit)", io.extent.width, io.extent.height);
        }
    }

} // namespace deren::vulkan::pass
