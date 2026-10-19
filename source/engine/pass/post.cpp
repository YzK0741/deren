// The post chain's implementation: the composite (HDR + the weighted bloom levels -> exposure -> ACES ->
// display) and the bloom chain's four levels (the bright-pass prefilter into level 0, then one downsample per
// level). Moved out of `runtime::record_composite` and `runtime::record_bloom_chain` UNCHANGED in behaviour - the
// same barriers in the same order, the same clear instances over the same views, the same one-set binds, the same
// 52-byte push block with the same lanes and the same fullscreen draw - so the capture gate decides the move on
// all fourteen scenarios, every one of which runs the composite and (measured) the
// bloom chain as well.

module;

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

module deren.engine.pass.post;
import deren.utility.shared;

import deren.promise.rhi;      // the record series (abi 20): the barriers, the rendering scope, the draw
import deren.engine.pipelines; // build_post: the chain's two pipelines, one per colour format the chain renders into
import deren.utility;

namespace deren::engine::pass {

    namespace rhi = deren::promise::rhi;

    // =============================================================================================
    // THE COMPOSITE
    // =============================================================================================

    post_composite_pass::~post_composite_pass() {
        this->release_owned();
    }

    void post_composite_pass::release_owned() noexcept {
        this->composite.reset();
        this->hdr.reset();
    }

    render_resource::pass_io const& post_composite_pass::io() const noexcept {
        return render_resource::post_composite_io;
    }

    deren::engine::pass::behaviour const& post_composite_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view post_composite_pass::feature() const noexcept {
        // NO FEATURE NAME, and that is deliberate rather than an oversight: the record step only asks a pass's feature
        // when it has one, and this pass's gate is the HOST's - `record_post_process` returns before the chain
        // when the pipelines or the frame's composite set are missing. A name here would be a second copy of that
        // question, and the deferred step measured what a pass feature name with no branch in the runtime does
        // (the record step skips the pass on every frame and the frame goes unlit).
        return {};
    }

    void post_composite_pass::create(pass_context const& context) {
        if (context.face == nullptr) {
            return;
        }
        if (this->built_against != nullptr && this->built_against != context.face) {
            this->release_owned();
        }
        this->built_against = context.face;
        if (this->composite.has_value()) {
            return; // already built for this device
        }
        std::span<uint8_t const> const vertex_spirv = context.shader != nullptr ? context.shader(context.owner, vertex_shader_name) : std::span<uint8_t const>{};
        std::span<uint8_t const> const fragment_spirv = context.shader != nullptr ? context.shader(context.owner, fragment_shader_name) : std::span<uint8_t const>{};
        if (vertex_spirv.empty() || fragment_spirv.empty()) {
            deren::utility::log("post chain disabled: the owner has no {} or {}", vertex_shader_name, fragment_shader_name);
            return;
        }
        // The SURFACE's format is one of the two pipelines' (the other renders into R16F bloom levels and into
        // the LDR image FXAA reads), and it is a session-stable device fact the context carries for exactly this
        // kind of reason (see pass_context::swap_chain_image_format).
        auto built = pipelines::build_post(*context.face, context.swap_chain_format, vertex_spirv, fragment_spirv);
        if (!built) {
            deren::utility::log("post chain disabled: {}", built.error());
            this->release_owned();
            return;
        }
        this->composite = std::move(built->composite);
        this->hdr = std::move(built->hdr);
        // The surface's format is cached here because the `encode_gamma` lane is a consequence of it (see
        // resolve): a session-stable device fact, which is exactly what a create step may keep.
        this->swap_chain_format = context.swap_chain_image_format;
        deren::utility::log("SUCCESS: post chain pipelines created (the composite for the swapchain and the R16F variant)");
    }

    void post_composite_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing to reset: the two pipelines depend on the surface's FORMAT (a session-stable device fact) and
        // on nothing whose size changes, and the sets this pass binds belong to the post family, whose owner
        // retires them (the record step calls this so a pass that owned a family could not forget to - see
        // recreate_stage).
    }

    bool post_composite_pass::pipeline_ready() const noexcept {
        return this->composite.has_value() && this->hdr.has_value();
    }

    deren::promise::rhi::pipeline* post_composite_pass::pipeline_handle() const noexcept {
        // THE DEFAULT VARIANT, the `composite` one (the swapchain/LDR target's R16F-format pipeline); the HDR
        // variant is published by NAME (see named_pipeline below), never by this accessor.
        return this->composite.has_value() ? this->composite->contract : nullptr;
    }

    owned_pipeline post_composite_pass::named_pipeline(std::string_view const name) const noexcept {
        // THE ONE NAME THIS PASS PUBLISHES TO ITS SIBLINGS: the bloom levels' `post_hdr` (see the class note and
        // frame_pass::named_pipeline). Its own name is answered by `pipeline_handle()` above.
        return name == bloom_pipeline_name ? owned_pipeline{.contract = this->hdr.has_value() ? this->hdr->contract : nullptr} : owned_pipeline{};
    }

    deren::promise::rhi::pipeline* post_composite_pass::composite_pipeline() const noexcept {
        return this->composite.has_value() ? this->composite->contract : nullptr;
    }

    deren::promise::rhi::pipeline* post_composite_pass::hdr_pipeline() const noexcept {
        return this->hdr.has_value() ? this->hdr->contract : nullptr;
    }

    void post_composite_pass::set_frame(composite_frame const& frame) noexcept {
        this->pass_frame = frame;
    }

    void post_composite_pass::set_overlay(draw_callback const overlay) noexcept {
        // The host's hook, installed once by whoever owns the passes. It is STORED, not put in the frame: what the
        // frame decides per frame is whether THIS pass is the frame's last writer (see prepare_frame).
        this->overlay_callback = overlay;
    }

    void post_composite_pass::prepare_frame(frame_facts const& facts) noexcept {
        // WHO WRITES THE LDR IMAGE is the fact that decides both lanes at once: when EITHER resolve runs (FXAA at
        // the frame's resolution, the upscale below render_scale 1.0 - the two are mutually exclusive, see
        // runtime::post_fxaa_active), this pass is not the frame's last writer, so it writes the intermediate and
        // that resolve carries the overlay; when neither runs, this pass is the last writer and draws the
        // overlay itself.
        bool const resolved = facts.fxaa_resolves || facts.upscale_resolves;
        composite_frame frame = {};
        frame.after_draw = resolved ? draw_callback{} : this->overlay_callback;
        frame.write_ldr = resolved;
        frame.suppress_bloom = facts.debug_view;
        this->set_frame(frame);
    }

    bool post_composite_pass::resolve(resolve_context const& context, resolved_io& out) const {
        if (!resolve_declaration(*this, context, out)) {
            return false;
        }
        // THE FRAME DECIDES THE TARGET AND THE PIPELINE TOGETHER (see this override's declaration note): with a
        // resolve on (FXAA at the frame's extent, the upscale below render_scale 1.0), the composite writes the
        // R16F LDR image with the R16F variant; otherwise it writes the declared swapchain image and neither
        // resolve pass runs. The LDR image is not in the declaration - one `render_target` names one resource -
        // so it comes from the frame's table, in the declaration's own vocabulary, exactly as the declared one did.
        if (!this->pass_frame.write_ldr) {
            return this->fill_push(out, false);
        }
        render_resource::resource_info const* const ldr = render_resource::find(resource_id::ldr);
        if (ldr == nullptr) {
            return false;
        }
        resolved_binding const target = context.resources->find(resource_id::ldr, 0, instance_for(ldr->scope, out.frame));
        // THE CONTRACT LANE IS THE GUARD (plan X5 B2): the raw lane is gone, and "no LDR image this generation"
        // is exactly "no contract handle was published for it".
        if (target.view_handle == nullptr || target.image_handle == nullptr) {
            return false; // no LDR image this generation: do not record a composite that cannot write anywhere
        }
        out.target_storage[0] = target;
        // THE VARIANT THIS FRAME NEEDS, in the ONE lane there is now (abi 21): the LDR target is the R16F
        // pipeline's, and `resolve` is where the frame's decision between the two variants lives.
        out.pipeline_storage[0] = this->hdr_pipeline();
        out.pipelines = std::span<deren::promise::rhi::pipeline* const>(out.pipeline_storage.data(), 1);
        return this->fill_push(out, true);
    }

    bool post_composite_pass::fill_push(resolved_io& out, bool const writing_ldr) const {
        // The lanes are the frame's settings (more than one pass reads them - see
        // vulkan.frame_constants::render_settings), this pass's own GI upsample switch, and the TWO that follow
        // from the target choice: `encode_gamma` (an R16F target needs the shader to encode, an sRGB swapchain
        // attachment does the transfer in hardware) and the GI weight, which is the frame's answer to whether the
        // chain resolved.
        render_settings const& settings = out.constants.settings;
        post_push_constants const push = {
            .exposure = settings.exposure,
            // Zero while the G-buffer debug view is up (the frame says so - see composite_frame::suppress_bloom),
            // for the reason that field records.
            .bloom_intensity = this->pass_frame.suppress_bloom ? 0.0f : settings.bloom_intensity,
            .bloom_threshold = settings.bloom_threshold,
            .encode_gamma = writing_ldr ? 1.0f : (is_srgb_swapchain_format(this->swap_chain_format) ? 0.0f : 1.0f),
            .fxaa_subpixel = settings.fxaa_subpixel,
            .fxaa_edge_threshold = settings.fxaa_edge_threshold,
        };
        static_assert(sizeof(push) == render_resource::post_push_bytes, "the composed push block must be the size the declaration promises");
        std::memcpy(out.push_storage.data(), &push, sizeof(push));
        out.push = std::span<std::byte const>(out.push_storage.data(), sizeof(push));
        return true;
    }

    rhi::error post_composite_pass::record_content(resolved_io const& io) noexcept {
        if (!this->pipeline_ready() || io.targets.empty() || io.pipelines.empty() || !io.pipelines[0] ||
            !io.cmd || !io.list || io.cmd.get() != io.list || !io.targets[0].image_handle || !io.targets[0].view_handle ||
            io.extent.width == 0 || io.extent.height == 0 || io.push.size() < sizeof(post_push_constants)) {
            return rhi::error::invalid_argument;
        }
        // The push block the host filled, with the pass's own stage lane written last: `mode = 2` is what makes
        // post.frag composite instead of prefiltering or downsampling.
        post_push_constants push = {};
        std::memcpy(&push, io.push.data(), sizeof(push));
        push.mode = 2.0f;
        // THE RENDERING SCOPE RIDES THE CONTRACT NOW (abi 20): one colour attachment, CLEAR + STORE (the
        // helper the raw spelling used), zero clear colour, no depth - the composite's whole scope.
        deren::utility::vessel::array<rhi::color_attachment, 1> const colors = {
            rhi::color_attachment{.view = io.targets[0].view_handle, .load = rhi::load_op::clear, .store = rhi::store_op::store, .clear = {}},
        };
        rhi::rendering_info const rendering_info{
            .struct_size = sizeof(rhi::rendering_info),
            .area = {.offset_x = 0, .offset_y = 0, .width = io.extent.width, .height = io.extent.height},
            .layer_count = 1,
            .colors = colors,
            .depth = {},
            .has_depth = false,
            .secondary_contents = false, // this instance is inline; the overlay segment runs in its own after it ends
        };
        auto const begun = io.list->begin_rendering(rendering_info);
        if (begun != rhi::error::ok) {
            return begun;
        }
        io.list->set_cull_mode(rhi::cull_mode::none); // the synthetic triangle has no facing to cull
        if (!io.push_block(*io.cmd, pass::push_bytes(push))) {
            io.list->end_rendering();
            return rhi::error::operation_failed;
        }
        io.list->draw(3, 1, 0, 0);
        // THE OVERLAY RECORDS ITS OWN SEGMENT HERE (gui abi 5): the third-party recorder is not a contract
        // consumer, so the plugin starts one of its mergeable targets under this instance's inheritance and
        // returns it; the segment is EXECUTED by the host in its own secondary-contents instance after THIS
        // instance ends (see draw_callback::finish - the two cannot share one instance because a
        // CONTENTS_SECONDARY_COMMAND_BUFFERS instance may contain no primary draw).
        if (this->pass_frame.after_draw.valid()) {
            auto const overlay_begin = pass::make_overlay_begin(
                rendering_info, io.targets[0].image_handle != nullptr ? io.targets[0].image_handle->format()
                                                                      : deren::promise::rhi::image_format::unknown);
            this->pass_frame.after_draw.record(this->pass_frame.after_draw.owner, *io.cmd, overlay_begin.begin);
        }
        io.list->end_rendering();
        if (this->pass_frame.after_draw.valid() && this->pass_frame.after_draw.finish != nullptr) {
            return this->pass_frame.after_draw.finish_with_dependency(*io.cmd, *io.targets[0].image_handle, rendering_info);
        }
        return rhi::error::ok;
    }

    // =============================================================================================
    // THE BLOOM CHAIN's FOUR LEVELS
    // =============================================================================================

    post_bloom_pass::post_bloom_pass(uint32_t const level) noexcept
        : bloom_level(level < render_resource::post_bloom_io.size() ? level : 0u)
        , declared_io(&render_resource::post_bloom_io[this->bloom_level]) {
        // The behaviour is a MEMBER because the ELEMENT is part of it: four instances of this class cannot share
        // one declaration of "which resource my extent comes from". Everything else is every other fullscreen
        // pass's: the record step sets the viewport and scissor from the extent the rule produced.
        this->pass_behaviour = deren::engine::pass::behaviour{
            .kind = behaviour_kind::fullscreen,
            .extent = extent_rule::resource,
            .extent_of = resource_id::bloom,
            .extent_of_element = static_cast<uint16_t>(this->bloom_level),
            .pipelines = pipeline_names,
            .resync_viewport = true,
        };
    }

    post_bloom_pass::~post_bloom_pass() = default;

    render_resource::pass_io const& post_bloom_pass::io() const noexcept {
        return *this->declared_io;
    }

    deren::engine::pass::behaviour const& post_bloom_pass::behaviour() const noexcept {
        return this->pass_behaviour;
    }

    std::string_view post_bloom_pass::feature() const noexcept {
        // THE BLOOM CHAIN'S OWN GATE, and the renderer already answers this name: `bloom_intensity > 0` and the
        // chain's pipeline being there. It has to be a feature rather than a resolver that fails, because the
        // OFF path is not "nothing happens": the host moves the four levels to a sampled layout so the
        // composite's descriptor (which declares all four as inputs) is valid on a frame no level was written -
        // and that off path is the host's, triggered by this stage recording none of its passes.
        return "bloom";
    }

    void post_bloom_pass::create(pass_context const&) {
        // NOTHING TO BUILD, and that is this pass's whole shape: it records with the chain's R16F pipeline, which
        // the COMPOSITE owns - one push block serves all five stages (one shader, one `mode` lane), so a copy per
        // level would be five identical objects and five chances to disagree about the push block. Its target
        // is an element of its declaration and its extent comes from the declaration's rule, which is why it has
        // no error path here either.
    }

    void post_bloom_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing to reset: this pass owns no handle at all (its pipeline arrives through resolved_io::pipelines
        // and its target through its own declaration).
    }

    uint32_t post_bloom_pass::level() const noexcept {
        return this->bloom_level;
    }

    rhi::error post_bloom_pass::record_content(resolved_io const& io) noexcept {
        if (io.targets.empty() || io.pipelines.empty() || io.pipelines[0] == nullptr ||
            io.extent.width == 0 || io.extent.height == 0 || io.list == nullptr || !io.cmd ||
            io.cmd.get() != io.list || io.targets[0].image_handle == nullptr || io.targets[0].view_handle == nullptr) {
            return rhi::error::invalid_argument;
        }
        // The push block is composed HERE (S3): the frame's three settings (exposure, the bloom weight and the
        // bright-pass threshold - all of them lanes more than one pass pushes, which is why they are frame
        // settings), the struct's defaults for every lane this stage does not read - which is what the renderer
        // pushed before the move - and the stage's own lane: 0 for the bright-pass prefilter, 1 for every
        // downsample, because the bright-pass test runs on the first level only.
        render_settings const& settings = io.constants.settings;
        post_push_constants push = {
            .exposure = settings.exposure,
            .bloom_intensity = settings.bloom_intensity,
            .bloom_threshold = settings.bloom_threshold,
            .mode = this->bloom_level == 0u ? 0.0f : 1.0f,
        };
        deren::utility::vessel::array<rhi::color_attachment, 1> const colors = {
            rhi::color_attachment{.view = io.targets[0].view_handle, .load = rhi::load_op::clear, .store = rhi::store_op::store, .clear = {}},
        };
        rhi::rendering_info const rendering_info{
            .struct_size = sizeof(rhi::rendering_info),
            .area = {.offset_x = 0, .offset_y = 0, .width = io.extent.width, .height = io.extent.height},
            .layer_count = 1,
            .colors = colors,
            .depth = {},
            .has_depth = false,
            .secondary_contents = false,
        };
        auto const begun = io.list->begin_rendering(rendering_info);
        if (begun != rhi::error::ok) {
            return begun;
        }
        io.list->set_cull_mode(rhi::cull_mode::none);
        // THIS LEVEL'S SOURCE: the third lane says which one, and the HOST turns it into a heap slot (see
        // runtime::push_stage_block). Level 0 (the prefilter) and the composite read the HDR target, which is the
        // lane's 0; a downsample at level N reads the level above it, which is N.
        if (!io.push_block(*io.cmd, pass::push_bytes(push), this->bloom_level)) {
            io.list->end_rendering();
            return rhi::error::operation_failed;
        }
        io.list->draw(3, 1, 0, 0);
        io.list->end_rendering();
        return rhi::error::ok;
    }

} // namespace deren::engine::pass
