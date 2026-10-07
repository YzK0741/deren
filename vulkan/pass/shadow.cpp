// The shadow pass's implementation: the depth-only pipeline (built against the SCENE pipeline layout and the
// context's depth format) and the per-cascade recording - the layer's transition to a depth attachment, the
// depth-only instance at the map's edge, the pre-recorded secondary's execution and the instance's end. Moved out of
// `runtime::make_shadow_pipeline` + the shadow block of `begin_recording` UNCHANGED in behaviour, except for the
// things the pass does not own (the hand-back barrier over every allocated layer, and the per-cascade content, which
// the renderer records through the frame's callback) - so the capture gate, which runs with shadows on in all twelve
// scenarios, decides the move.

module;

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

module deren.vulkan.pass.shadow;

import deren.promise.rhi;
import deren.vulkan.constant_init;
import deren.vulkan.pipelines; // build_shadow: the depth-only pipeline this pass owns
import deren.utility;

namespace rhi = deren::promise::rhi;

namespace deren::vulkan::pass {

    shadow_pass::~shadow_pass() {
        this->release_owned();
    }

    void shadow_pass::release_owned() noexcept {
        this->mesh_pipeline.reset();
        this->meshlet_pipeline.reset();
    }

    render_resource::pass_io const& shadow_pass::io() const noexcept {
        return render_resource::shadow_io;
    }

    deren::vulkan::pass::behaviour const& shadow_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view shadow_pass::feature() const noexcept {
        // THE NAME THE RENDERER ALREADY ANSWERS: `f.shadow` is the two knobs, this pipeline's existence and "the flat
        // render mode reads no shadow map" - one answer shared by the frame loop's stage gate, the overlay and the
        // startup log, so the three cannot disagree about whether this pass runs.
        return "shadow";
    }

    void shadow_pass::create(pass_context const& context) {
        // THE DEPTH FORMAT IS THE CONTRACT'S `depth` ROLE now (abi 8): the backend picks the device's
        // concrete depth format, so there is no "the context must name one" precondition any more - and there is
        // no device in the context to check either (abi 21): the builders take the contract face alone.
        if (context.face == nullptr) {
            return;
        }
        if (this->built_against != nullptr && this->built_against != context.face) {
            this->release_owned();
        }
        this->built_against = context.face;
        if (this->mesh_pipeline.has_value()) {
            return; // already built for this device
        }
        std::span<uint8_t const> const fragment_spirv = context.shader != nullptr ? context.shader(context.owner, fragment_shader_name) : std::span<uint8_t const>{};
        // ---- THE VERTEX FORM IS GONE (docs/mesh_shaders.md step 4): the pass's geometry arrives through a MESH
        // stage, so the vertex module is neither loaded nor registered anywhere, and the mesh one is REQUIRED. A
        // device without VK_EXT_mesh_shader does not reach here at all (the runtime refuses to start), which is why
        // the two checks below are defensive: they keep a mis-registered shader a log line rather than a crash.
        if (context.shader == nullptr || fragment_spirv.empty()) {
            deren::utility::log("shadow disabled: the owner has no {}", fragment_shader_name);
            return;
        }
        if (!context.mesh_shaders) {
            deren::utility::log("shadow disabled: the device has no mesh shaders, and the vertex form is gone (docs/mesh_shaders.md step 4)");
            return;
        }
        std::span<uint8_t const> const mesh_spirv = context.shader != nullptr ? context.shader(context.owner, mesh_shader_name) : std::span<uint8_t const>{};
        if (mesh_spirv.empty()) {
            deren::utility::log("shadow disabled: the owner has no {}", mesh_shader_name);
            return;
        }
        // The pipeline is heap-native: nothing about the draw's descriptors or push travels through a layout.
        // IT IS THE MESH FORM THAT IS BUILT FIRST NOW, and it is required: with the vertex form gone this pass cannot
        // draw its casters any other way, so a refusal is a DISABLED PASS rather than a fallback - visible as a
        // missing shadow rather than as a wrong picture, and named in the log.
        auto mesh_built = pipelines::build_shadow(*context.face, rhi::image_format::depth, create_bias_constant, create_bias_slope, create_bias_clamp, mesh_spirv, fragment_spirv, rhi::shader_stage::mesh);
        if (!mesh_built) {
            deren::utility::log("shadow disabled: the mesh pipeline was refused ({})", mesh_built.error());
            this->release_owned();
            return;
        }
        this->mesh_pipeline = std::move(*mesh_built);
        deren::utility::log("SUCCESS: shadow MESH pipeline created (the depth-only pass, fed by mesh dispatches)");
        // ---- ... and the MESHLET form (docs/mesh_shaders.md step 3): only the MESH module differs (same fragment
        // stage), and a missing shader or a refusal is a log line - the mesh form above is a complete answer.
        std::span<uint8_t const> const meshlet_spirv = context.shader != nullptr ? context.shader(context.owner, meshlet_shader_name) : std::span<uint8_t const>{};
        if (!meshlet_spirv.empty()) {
            auto meshlet_built = pipelines::build_shadow(*context.face, rhi::image_format::depth, create_bias_constant, create_bias_slope, create_bias_clamp, meshlet_spirv, fragment_spirv, rhi::shader_stage::mesh);
            if (meshlet_built) {
                this->meshlet_pipeline = std::move(*meshlet_built);
                deren::utility::log("SUCCESS: shadow MESHLET pipeline created (one workgroup per meshlet, window read from the table)");
            } else {
                deren::utility::log("shadow: the meshlet pipeline was refused ({}), so the pass keeps the mesh form", meshlet_built.error());
            }
        }
    }

    void shadow_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing to reset: the pipeline depends on the DEPTH format and on nothing whose size changes, and the map
        // it renders into is sized by the frame's `map_size` rather than by the surface.
    }

    bool shadow_pass::pipeline_ready() const noexcept {
        // ONE FORM IS ENOUGH, and neither is the vertex one: the mesh form is required at create and the meshlet form
        // is preferred over it, so "ready" is "either was built" (docs/mesh_shaders.md step 4)
        return this->meshlet_pipeline.has_value() || this->mesh_pipeline.has_value();
    }

    deren::promise::rhi::pipeline* shadow_pass::pipeline_handle() const noexcept {
        // THE MESHLET FORM WHEN THERE IS ONE, and the MESH form otherwise: they are the same pass (same targets,
        // same fragment stage, same casters), so which one draws is not the frame's business. There is NO vertex
        // form since step 4 (docs/mesh_shaders.md): a null here means no shadow map rather than a different
        // rasterizer.
        if (this->meshlet_pipeline.has_value()) {
            return this->meshlet_pipeline->contract;
        }
        if (this->mesh_pipeline.has_value()) {
            return this->mesh_pipeline->contract;
        }
        return nullptr; // no mesh form, no shadow map: the vertex form is gone (see create)
    }

    void shadow_pass::set_frame(shadow_frame const& frame) noexcept {
        this->pass_frame = frame;
    }

    void shadow_pass::record(resolved_io const& io) {
        if (!this->pipeline_ready() || io.targets.empty() || io.list == nullptr) {
            return;
        }
        if (this->pass_frame.record_cascade == nullptr || this->pass_frame.run_tasks == nullptr || this->pass_frame.map_size == 0u) {
            return;
        }
        // ONE LAYER PER CASCADE, and never more than the map has: the DECLARATION claims a RUN of cascade layers
        // (render_resource::shadow_targets) and the FRAME caps it at the layers the image actually has - so the
        // two counts meet here. The secondaries are the frame's own count, and they are what decides how many
        // layers this frame renders: a cascade with no secondary to record into is not rendered at all.
        uint32_t const layers = static_cast<uint32_t>(std::min<std::size_t>(this->pass_frame.cascades.size(), io.targets.size()));
        if (layers == 0u) {
            return;
        }
        // THE PASS'S OWN PIPELINE, AS THE CONTRACT HANDLE (abi 21): it travels to the frame's cascade callback,
        // which binds it through `command_buffer::bind_pipeline` inside the secondary's own session. There is no
        // raw `VkPipeline` in this file any more.
        deren::promise::rhi::pipeline* const pipeline = this->pipeline_handle();
        // ... and HOW it must be fed travels with it: a mesh pipeline has no input assembler, so its casters are
        // dispatched rather than drawn (see the frame's record_cascade).
        bool const meshlets = this->meshlet_pipeline.has_value();
        bool const mesh_stage = meshlets || this->mesh_pipeline.has_value();
        // ---- THE CONTENT: one task per cascade, each into its OWN secondary ----
        // A VkCommandPool is not thread safe, which is why every cascade has its own {pool, buffer} pair (the same
        // rule the main pass's workers follow). Only the CONTENT moves off the primary thread: the barriers, the
        // instances and the executions below stay here, in the layer order the attachments require, so a parallel
        // frame's recorded commands are identical to a sequential one's.
        std::vector<std::function<void()>> tasks;
        tasks.reserve(layers);
        std::vector<bool> recorded(layers, false);
        for (uint32_t cascade = 0; cascade < layers; ++cascade) {
            tasks.emplace_back([this, cascade, pipeline, mesh_stage, meshlets, &recorded] {
                // THE CASCADE'S SECONDARY IS A CONTRACT HANDLE NOW (abi 20), BORROWED from the frame: the
                // callback begins and ends it through the contract, so no raw handle is named here at all.
                rhi::command_buffer* const secondary = this->pass_frame.cascades[cascade];
                if (secondary == nullptr) {
                    return;
                }
                recorded[cascade] = this->pass_frame.record_cascade(this->pass_frame.owner, *secondary, cascade, pipeline, mesh_stage, meshlets);
            });
        }
        this->pass_frame.run_tasks(this->pass_frame.owner, tasks);

        // ---- ONE INSTANCE PER CASCADE, in the primary ----
        VkExtent2D const map_extent = {this->pass_frame.map_size, this->pass_frame.map_size};
        for (uint32_t cascade = 0; cascade < layers; ++cascade) {
            rhi::image_view* const layer_view = io.targets[cascade].view_handle;
            rhi::image* const layer_image = io.targets[cascade].image_handle;
            if (layer_view == nullptr || layer_image == nullptr) {
                continue;
            }
            // THIS layer to a renderable depth attachment: one barrier per layer, because the transition's
            // subresource range is single-layer and each layer is its own attachment here. Its loadOp CLEAR discards
            // the previous frame's contents, so UNDEFINED as the old layout is valid.
            // THE PAIR RIDES THE CONTRACT (abi 20): undefined -> depth_attachment is the shipped
            // `depth_attachment_transition`, and the layer it covers is the contract range's base_layer - the raw
            // constant's single-mip, single-layer range retargeted at `cascade`, field for field.
            if (io.list->barrier(rhi::image_barrier{.resource = layer_image,
                                                    .from = rhi::image_use::undefined,
                                                    .to = rhi::image_use::depth_attachment,
                                                    .range = {.base_mip = 0, .mip_count = 1, .base_layer = cascade, .layer_count = 1}}) != rhi::error::ok) {
                return; // a refused barrier would leave the layer in a state nobody declared
            }
            // Depth-only rendering into this cascade (no colour attachment), with loadOp CLEAR (the far plane) and
            // storeOp STORE - the map has to survive for the shading stages that sample it.
            // THE SCOPE RIDES THE CONTRACT TOO (abi 20): one depth attachment, no colour one, and the raw call's
            // flags word is the `secondary_contents` bit.
            rhi::depth_attachment const depth_attachment = {.view = layer_view,
                                                            .load = rhi::load_op::clear, // make_depth_attachment_info's loadOp
                                                            .store = rhi::store_op::store,
                                                            .read_only = false,
                                                            .has_stencil = false,
                                                            .clear_depth = 1.0f,
                                                            .clear_stencil = 0};
            rhi::rendering_info const rendering_info{
                .struct_size = sizeof(rhi::rendering_info),
                .area = {.offset_x = 0, .offset_y = 0, .width = map_extent.width, .height = map_extent.height},
                .layer_count = 1, // the raw make_rendering_info's layerCount, which is 1 at every site in this engine
                .colors = {},
                .depth = depth_attachment,
                .has_depth = true,
                .secondary_contents = true, // the raw call passed VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT
            };
            if (io.list->begin_rendering(rendering_info) != rhi::error::ok) {
                return; // a refused scope would leave the cascade in a state nobody declared
            }
            // Never execute a secondary whose begin failed - executing an unrecorded command buffer is a VUID and can
            // wedge the frame slot, which is what the recorded flags are for (a null buffer or a log line from the
            // callback leaves its flag false).
            if (recorded[cascade]) {
                rhi::command_buffer* const secondary = this->pass_frame.cascades[cascade];
                if (io.list->execute(*secondary) != rhi::error::ok) {
                    deren::utility::log("shadow pass: executing cascade {}'s secondary was refused - the cascade records nothing this frame", cascade);
                }
            }
            io.list->end_rendering();
        }
    }

} // namespace deren::vulkan::pass
