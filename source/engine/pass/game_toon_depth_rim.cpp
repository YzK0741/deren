// The game chain's depth rim's implementation: the pipeline (one ADDITIVE target, no depth attachment), the
// fullscreen draw, and nothing else.
//
// MODELLED ON toon_screen_rim.cpp, and what is MISSING from it is the point: no barriers, and no clear. No
// barriers because the two images this pass samples are already in a sampled layout - the G-buffer targets and
// the depth are published by the stage preamble (the renderer's per-image bookkeeping), and this pass is never
// the one that took them out of it. No clear because the target is LOADed: the whole point is to ADD to the
// frame the character-forward stage produced, not to replace it.

module;

#include <cstdint>
#include <new> // ::operator new's aligned overloads must be VISIBLE here: the deferred
               // __libcpp_allocate codegen crashes without them (clang 22, measured this session)
#include <span>

module deren.engine.pass.game_toon_depth_rim;
import deren.utility.shared;

import deren.promise.rhi;
import deren.engine.render_resource;
import deren.engine.pipelines; // make_graphics_pipeline: the contract factory this pass builds through
import deren.utility;

namespace rhi = deren::promise::rhi;

namespace deren::engine::pass {

    render_resource::pass_io const& game_toon_depth_rim_pass::io() const noexcept {
        return pass_declaration;
    }

    deren::engine::pass::behaviour const& game_toon_depth_rim_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view game_toon_depth_rim_pass::feature() const noexcept {
        // ITS OWN NAME, and the post-graph lane is gated on the COMPOSED runtime predicate rather than on this
        // string (see runtime.post_graph.cpp): `game_toon_active()` is the same answer `make_character_forward_frame`
        // uses to select the pipeline, so the contour and the game-shaded surface cannot disagree about which chain
        // is drawing. The name stays here because a pass is recordable-or-not in the DECLARATION's vocabulary, and
        // because a future owner that wants a feature-table branch should not have to invent the name.
        return "game_toon_depth_rim";
    }

    void game_toon_depth_rim_pass::create(pass_context const& context) {
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
        std::span<uint8_t const> const vertex_spirv = context.shader != nullptr ? context.shader(context.owner, vertex_shader_name) : std::span<uint8_t const>{};
        std::span<uint8_t const> const fragment_spirv = context.shader != nullptr ? context.shader(context.owner, fragment_shader_name) : std::span<uint8_t const>{};
        if (vertex_spirv.empty() || fragment_spirv.empty()) {
            deren::utility::log("game toon depth rim disabled: the owner has no {} or {}", vertex_shader_name, fragment_shader_name);
            return;
        }
        // ONE colour target and it is the HDR one, because this pass runs INSIDE the HDR chain (after the
        // character-forward stage, before the resolve) - a rim written to the swapchain would be tonemapped twice.
        deren::utility::vessel::array<rhi::image_format, 1> const formats = {rhi::image_format::r16g16b16a16_sfloat};
        // ADDITIVE: the rim is a contribution to the frame, not a replacement for it.
        deren::utility::vessel::array<rhi::blend_mode, 1> const blends = {rhi::blend_mode::additive};
        auto built = pipelines::make_graphics_pipeline(*context.face,
                                                       std::span<rhi::image_format const>(formats),
                                                       rhi::image_format::unknown, // NO depth attachment: the depth is sampled, not tested
                                                       vertex_spirv,
                                                       fragment_spirv,
                                                       1u,
                                                       /*depth_test=*/false,
                                                       0.0f,
                                                       0.0f,
                                                       0.0f,
                                                       std::span<rhi::blend_mode const>(blends));
        if (!built) {
            deren::utility::log("game toon depth rim disabled: {}", built.error());
            this->release_owned();
            return;
        }
        built->viewport = rhi::viewport{.x = 0.0f, .y = 0.0f, .width = 1.0f, .height = 1.0f, .min_depth = 0.0f, .max_depth = 1.0f}; // the record step resyncs it from io.extent
        built->scissor = rhi::rect{.offset_x = 0, .offset_y = 0, .width = 1u, .height = 1u};
        this->pass_pipeline = std::move(*built);
        deren::utility::log("SUCCESS: game toon depth rim pipeline created (a fullscreen additive contour from the depth)");
    }

    void game_toon_depth_rim_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing to reset: the pipeline depends on the HDR target's format and on nothing whose size changes,
        // and the viewport/scissor are resynced by the record step (see pass_behaviour::resync_viewport).
    }

    void game_toon_depth_rim_pass::release_owned() noexcept {
        this->pass_pipeline.reset();
    }

    game_toon_depth_rim_pass::~game_toon_depth_rim_pass() {
        this->release_owned();
    }

    bool game_toon_depth_rim_pass::pipeline_ready() const noexcept {
        return this->pass_pipeline.has_value();
    }

    deren::promise::rhi::pipeline* game_toon_depth_rim_pass::pipeline_handle() const noexcept {
        return this->pass_pipeline.has_value() ? this->pass_pipeline->contract : nullptr;
    }

    void game_toon_depth_rim_pass::set_shape(float const width, float const scale, float const strength) noexcept {
        this->rim_width = width;
        this->rim_scale = scale;
        this->rim_strength = strength;
    }

    void game_toon_depth_rim_pass::set_colour(float const r, float const g, float const b) noexcept {
        this->rim_colour[0] = r;
        this->rim_colour[1] = g;
        this->rim_colour[2] = b;
    }

    void game_toon_depth_rim_pass::record_impl(resolved_io const& io) {
        if (!this->pipeline_ready() || io.targets.empty() || io.extent.width == 0 || io.extent.height == 0) {
            return; // the record step resolves all of this or skips the pass (see frame_pass::resolve)
        }
        deren::promise::rhi::image_view* const target_view = io.targets[0].view_handle;
        if (target_view == nullptr) {
            return;
        }
        // LOAD, not clear: this instance ADDS to the frame the character-forward stage wrote - one colour
        // attachment, LOAD + STORE, no depth; the whole scope this pass opens.
        deren::utility::vessel::array<rhi::color_attachment, 1> const colors = {
            rhi::color_attachment{.view = io.targets[0].view_handle, .load = rhi::load_op::load, .store = rhi::store_op::store, .clear = {}},
        };
        rhi::rendering_info const rendering_info{
            .struct_size = sizeof(rhi::rendering_info),
            .area = {.offset_x = 0, .offset_y = 0, .width = io.extent.width, .height = io.extent.height},
            .layer_count = 1,
            .colors = colors,
            .depth = {},
            .has_depth = false,
            .secondary_contents = false, // nothing here executes a secondary command buffer
        };
        if (io.list->begin_rendering(rendering_info) != rhi::error::ok) {
            return;
        }
        io.list->set_cull_mode(rhi::cull_mode::none); // the synthetic triangle has no facing to cull
        // NO SET TO BIND: the depth and the albedo are per-swapchain-image heap slots the shader indexes with
        // the image index its push block carries. The two projection terms are the frame's, and the rest is this
        // pass's own parameter.
        push_constants const push = {
            .proj_22 = io.constants.proj[2][2],
            .proj_32 = io.constants.proj[3][2],
            .rim_width = this->rim_width,
            .rim_scale = this->rim_scale,
            .rim_strength = this->rim_strength,
            .pad0 = 0.0f,
            .pad1 = 0.0f,
            .pad2 = 0.0f,
            .rim_colour = {this->rim_colour[0], this->rim_colour[1], this->rim_colour[2], 1.0f},
        };
        [[maybe_unused]] bool const pushed = io.push_block(*io.cmd, pass::push_bytes(push));
        io.list->draw(3, 1, 0, 0);
        io.list->end_rendering();
    }

    rhi::error game_toon_depth_rim_pass::record_content(resolved_io const& io) noexcept {
        // THIS PASS OWNS NO BARRIERS: the graph declares the sampled inputs and the target write on its behalf,
        // so the graph entry only refuses what the record step would have refused.
        if (!this->pipeline_ready() || io.targets.empty() || io.targets[0].view_handle == nullptr || io.extent.width == 0 ||
            io.extent.height == 0 || io.list == nullptr) {
            return rhi::error::invalid_argument;
        }
        this->record_impl(io);
        return rhi::error::ok;
    }

} // namespace deren::engine::pass
