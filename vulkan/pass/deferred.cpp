// The deferred lighting stage's implementation: the scene-colour dependency barrier, the frame's two per-image
// input transitions, the LOAD instance over the frame's scene target and the fullscreen draw. Moved out of
// `runtime::record_lighting_pass` UNCHANGED in behaviour - the same barrier, the same two ensures, the same LOAD
// attachment, the same bind order (the scene block, then the G-buffer images), the same 88-byte push and the same 3-vertex draw -
// so the capture gate decides the move on all twelve scenarios, every one of which runs this stage.

module;

#include <algorithm> // std::clamp / std::max in set_ssao's clamps
#include <array>
#include <cstdint>
#include <cstring>
#include <glm/glm.hpp> // the push block's ssao lane and the frame's inverse view-projection
#include <span>
#include <string>

module deren.vulkan.pass.deferred;

import deren.promise.rhi;
import deren.vulkan.pipelines; // build_deferred: the pipeline this pass owns
import deren.utility;

namespace rhi = deren::promise::rhi;

namespace deren::vulkan::pass {

    deferred_pass::~deferred_pass() {
        this->release_owned();
    }

    void deferred_pass::release_owned() noexcept {
        this->pass_pipeline.reset();
    }

    render_resource::pass_io const& deferred_pass::io() const noexcept {
        return render_resource::deferred_io;
    }

    deren::vulkan::pass::behaviour const& deferred_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view deferred_pass::feature() const noexcept {
        return "deferred";
    }

    bool deferred_pass::pipeline_ready() const noexcept {
        return this->pass_pipeline.has_value();
    }

    deren::promise::rhi::pipeline* deferred_pass::pipeline_handle() const noexcept {
        return this->pass_pipeline.has_value() ? this->pass_pipeline->contract : nullptr;
    }

    void deferred_pass::set_frame(deferred_frame const& frame) noexcept {
        this->pass_frame = frame;
    }

    void deferred_pass::prepare_frame(frame_facts const& facts) noexcept {
        // ONE flag, and it is the composed predicate (the knob AND ray queries AND this frame's structures), not
        // the similarly named feature fact - see frame_facts' own note. The punctual lane is the OTHER kind of fact:
        // whether the stochastic pass recorded this frame, which is what makes the two paths exclusive.
        this->set_frame(deferred_frame{.punctual_replaced = facts.megalights_resolved});
    }

    void deferred_pass::create(pass_context const& context) {
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
            deren::utility::log("deferred lighting disabled: the owner has no {} or {}", vertex_shader_name, fragment_shader_name);
            return;
        }
        // The blend state is the stage's own: the attachment is LOADed and the lighting ADDS to the emissive the
        // G-buffer pass wrote.
        std::array<rhi::blend_mode, 1> const blend = {rhi::blend_mode::additive};
        auto built = pipelines::build_deferred(*context.face,
                                               std::span<rhi::blend_mode const>(blend), vertex_spirv, fragment_spirv);
        if (!built) {
            deren::utility::log("deferred lighting disabled: {}", built.error());
            this->release_owned();
            return;
        }
        this->pass_pipeline = std::move(built->lighting);
        deren::utility::log("SUCCESS: deferred lighting pipeline created (shades the stored surface, additive over the emissive)");
    }

    void deferred_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing to reset: no per-generation handle here. The pipeline does not depend on the surface's size and
        // the target arrives per frame (the host picks it - see the header).
    }

    void deferred_pass::set_ssao(bool const enabled, float const radius, float const intensity, uint32_t const samples) noexcept {
        // The clamps came with the parameters, because they are the same fact: an intensity above 1 darkens past
        // black, a negative radius is meaningless, and the sample count is bounded by the SHADER's own array (16).
        this->ssao_active = enabled;
        this->ssao_radius = std::max(radius, 0.0f);
        this->ssao_intensity = std::clamp(intensity, 0.0f, 1.0f);
        this->ssao_samples = std::clamp(samples, 0u, 16u);
    }

    bool deferred_pass::ssao_enabled() const noexcept {
        return this->ssao_active;
    }

    void deferred_pass::set_unlit(bool const unlit) noexcept {
        this->unlit_shading = unlit;
    }

    bool deferred_pass::unlit() const noexcept {
        return this->unlit_shading;
    }

    void deferred_pass::record(resolved_io const& io) {
        if (!this->pass_pipeline.has_value() || io.targets.empty() || io.pipelines.empty() || io.pipelines[0] == nullptr ||
            io.extent.width == 0 || io.extent.height == 0) {
            return; // the runner resolves all of this or skips the pass (see frame_pass::resolve)
        }
        // THE CONTRACT HANDLES ARE THE GUARD (plan X5 B2): the raw lane is gone, so this reads the lane the pass
        // records through - a null one is exactly "this frame has no target".
        deren::promise::rhi::image* const target = io.targets[0].image_handle;
        deren::promise::rhi::image_view* const target_view = io.targets[0].view_handle;
        if (target == nullptr || target_view == nullptr) {
            return;
        }
        // The scene-colour dependency, which cannot be folded into the sampling transitions below: it does not
        // change layout, and its consumer is this instance's LOAD - a color-attachment access, not the
        // FRAGMENT_SHADER read those transitions publish. Dynamic rendering inserts no dependency of its own between
        // two instances, so without it the load is not ordered after the G-buffer pass's store.
        // THE PAIR RIDES THE CONTRACT NOW (abi 20): the backend derives the masks and the layouts from the
        // (color_attachment, color_attachment) recipe this site has always used - a dependency that does not
        // change layout, and the one table pair that spells exactly that.
        if (io.list->barrier(rhi::image_barrier{.resource = io.targets[0].image_handle,
                                                .from = rhi::image_use::color_attachment,
                                                .to = rhi::image_use::color_attachment,
                                                .range = {}}) != rhi::error::ok) {
            return; // a refused barrier would leave the target in a state nobody declared
        }
        // The three stored targets and the G-buffer depth became samples in this STAGE's preamble, in the
        // renderer - the frame's ordering rule about images the G-buffer pass wrote (see deferred_frame's note).
        // THE RENDERING SCOPE RIDES THE CONTRACT NOW (abi 20): one colour attachment, LOAD + STORE (what the raw
        // helper spelled), no depth - the whole scope this pass opens.
        std::array<rhi::color_attachment, 1> const colors = {
            rhi::color_attachment{.view = io.targets[0].view_handle, .load = rhi::load_op::load, .store = rhi::store_op::store, .clear = {}},
        };
        rhi::rendering_info const rendering_info{
            .struct_size = sizeof(rhi::rendering_info),
            .area = {.offset_x = 0, .offset_y = 0, .width = io.extent.width, .height = io.extent.height},
            .layer_count = 1,
            .colors = colors,
            .depth = {},
            .has_depth = false,
            .secondary_contents = false, // no secondary command buffers: the lighting draw records straight into the primary
        };
        if (io.list->begin_rendering(rendering_info) != rhi::error::ok) {
            return;
        }
        // No set to bind: the stored targets and the G-buffer depth are heap slots, and the frame bound the heaps
        // on this command buffer (see begin_recording); the pass's push block carries the two indices.
        io.list->set_cull_mode(rhi::cull_mode::none); // the synthetic triangle has no facing to cull
        // THE PUSH BLOCK IS THE PASS'S OWN (S3): its shape was always the pass's (`push_constants`, `static_assert`ed
        // against the declaration), and its VALUES are now the pass's too - the SSAO parameters and the flat-render
        // flag it owns, the frame's inverse view-projection from `resolved_io::constants`, and the frame's answer to
        // what the lighting chain is doing. The renderer used to compose all of it and hand it over as raw bytes.
        push_constants push = {};
        push.inv_view_proj = io.constants.inv_view_proj;
        push.ssao = glm::vec4(this->ssao_radius, this->ssao_active ? this->ssao_intensity : 0.0f, static_cast<float>(this->ssao_samples), this->ssao_bias);
        push.unlit = this->unlit_shading ? 1.0f : 0.0f;
        push.punctual_replaced = this->pass_frame.punctual_replaced ? 1.0f : 0.0f;
        [[maybe_unused]] bool const pushed = io.push_block(*io.cmd, pass::push_bytes(push));
        io.list->draw(3, 1, 0, 0);
        io.list->end_rendering();
    }

} // namespace deren::vulkan::pass