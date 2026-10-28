/**
 * @file source/engine/pass/visibility_raster.cppm
 * @defgroup engine_pass_visibility_raster Visibility Raster Pass
 * @brief the VISIBILITY BUFFER's producer: mesh-task dispatches that write the main view's depth and ONE
 *        packed id per pixel, and nothing else.
 *
 * WHY THIS PASS EXISTS AS A PASS. The visibility buffer's first half is one rendering instance over two
 * attachments - the shared depth image and the R32_UINT id target - in which every admitted draw is a
 * mesh-task dispatch. That is the whole scope, so the pass owns it: it declares the two targets, opens the
 * instance, binds its own raster pipeline (the mesh stage plus the id-only fragment) and closes it. The
 * PER-ITEM work - which leaves, which geometry lanes, which per-frame draw-item rows, which indirect
 * commands - is the RENDERER's knowledge (it owns the scene, the buffer addresses and the tables), so the
 * pass asks for it through `visibility_frame::record_items` and knows nothing about leaves.
 *
 * WHAT IT DOES NOT DO, deliberately: it does not shade, does not draw the shadow pass and does not allocate
 * anything. The resolve pass that turns the ids back into surface attributes is a separate step (and a separate
 * pass); this one's contract is the depth test, the depth it writes and the id. THE DEPTH IS THE FRAME'S OWN
 * G-BUFFER DEPTH (v0.5), not a private copy: this lane replaces the geometry pass on an admitted frame, so what it
 * writes here is what the lighting and toon stages sample afterwards.
 *
 * LOAD OPS: BOTH ATTACHMENTS ARE CLEARED HERE. Since v0.5 this pass replaces the geometry lane on the frames the
 * visibility path is admitted to (the runtime's frame gate: `deren::engine::visibility::choose_opaque_lane`), so its
 * clears are the FRAME's clears - the id to zero ("no geometry won this pixel") and the shared G-buffer depth to the
 * far plane, exactly the pair the geometry pass used to write. On a frame the path is not admitted to, this pass does
 * not run at all and the geometry lane clears both.
 */
module;

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

export module deren.engine.pass.visibility_raster;

export import deren.utility.shared;

import deren.promise.rhi;
import deren.engine.pass;
import deren.engine.pipelines;
import deren.engine.render_layout;
import deren.engine.render_resource;

export namespace deren::engine::pass {
    namespace rhi = deren::promise::rhi;

    /**
     * @brief what the renderer records for the visibility raster pass, in one value
     *
     * A CALLBACK RATHER THAN A DRAW LIST, for the same reason `scene_frame` has one: the leaves, their
     * device buffer addresses and the per-frame item/indirect tables are the renderer's facts, and a pass
     * that reached into them would be a second owner of the frame loop's recording. The callback receives
     * the frame's command buffer and returns how many items it recorded.
     */
    struct visibility_frame {
        using segment_ref = deren::promise::rhi::mergeable_command_buffer_ref;
        using bind_segment_fn = bool (*)(void* owner, segment_ref const& segment);
        using record_items_fn = bool (*)(void* owner, deren::promise::rhi::command_buffer& command_buffer,
                                         std::span<segment_ref const> segments, bind_segment_fn bind_segment,
                                         void* segment_owner, std::uint32_t& recorded);

        /// how many items the renderer will record this frame (the pass's log line, and the frame's driver)
        std::uint32_t item_count = 0;
        /**
         * @brief record this frame's admitted draws: push one item block and issue its mesh-task dispatch each
         * @param owner the renderer
         * @param command_buffer the frame's primary, already inside this pass's rendering instance
         * @param segments optional per-worker mergeable segments; empty records straight into the primary
         * @param bind_segment opens one segment and rebinds this pass's pipeline/dynamic state into it
         * @param segment_owner the pass handed to @p bind_segment
         * @param recorded how many items were actually recorded (an item the renderer skips is not counted)
         * @return false when the renderer could not record the frame's items at all (the pass then fails)
         */
        record_items_fn record_items = nullptr;
        std::span<segment_ref const> segments = {};
        bind_segment_fn bind_segment = nullptr;
        void* segment_owner = nullptr;
        void* owner = nullptr;
        deren::promise::rhi::image_extent extent = {};
    };

    /// the declaration's two attachments: the id target (colour) and the FRAME'S SHARED G-BUFFER DEPTH - from
    /// v0.5 this lane IS the opaque producer when it runs, so the depth it tests against is the one the lighting,
    /// transparent and toon stages read afterwards (the dedicated capture depth of the capture-only step is
    /// retired from the frame: writing it too would be a second depth no reader names).
    inline constexpr std::array<render_resource::render_target, 2> visibility_raster_targets = {{
        {.resource = render_resource::resource_id::visibility_buffer, .kind = render_resource::target_kind::color},
        {.resource = render_resource::resource_id::gbuffer_depth, .kind = render_resource::target_kind::depth},
    }};

    inline constinit render_resource::pass_io const visibility_raster_io = {
        .name = "visibility raster",
        .bindings = {},
        .targets = visibility_raster_targets,
        .push = std::nullopt,
    };

    class visibility_raster_pass final : public frame_pass {
    public:
        ~visibility_raster_pass() override = default;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override {
            return visibility_raster_io;
        }

        [[nodiscard]] deren::engine::pass::behaviour const& behaviour() const noexcept override {
            // a graphics pass over its declared targets; the viewport comes from the resolved extent because
            // this pipeline is NOT in the runtime's registry (it is the pass's own), so `update_pass_geometry`'s
            // hand-maintained resync does not see it - the declaration carries the rule instead.
            static deren::engine::pass::behaviour const value{.kind = behaviour_kind::graphics, .extent = extent_rule::full, .resync_viewport = true};
            return value;
        }

        std::string_view feature() const noexcept override {
            return {}; // no feature knob: whether the lane exists is the graph's decision (and the frame's fallback)
        }

        /// @brief whether the pass has the pipeline it needs (not `ready()`: the framework owns that name)
        [[nodiscard]] bool has_pipeline() const noexcept {
            return this->raster_pipeline.has_value();
        }

        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const&) override {
            // nothing is per-generation: the pipeline depends on the device alone, and the targets arrive per frame
        }

        void set_load_existing(bool value) noexcept {
            this->load_existing = value;
        }
        [[nodiscard]] bool loads_existing() const noexcept {
            return this->load_existing;
        }
        void set_frame(visibility_frame const& frame) noexcept {
            this->pass_frame = frame;
        }

        [[nodiscard]] rhi::error record_content(resolved_io const& io) noexcept override;

        /// Open one per-worker segment inside this pass's rendering instance and rebind its pipeline state.
        static bool bind_segment(void* owner, rhi::mergeable_command_buffer_ref const& segment) noexcept;

    private:
        bool bind_segment_impl(rhi::mergeable_command_buffer_ref const& segment) noexcept;
        bool load_existing = false;
        visibility_frame pass_frame{};
        std::optional<pipelines::pipeline_handle> raster_pipeline = std::nullopt;
        void* built_against = nullptr;
    };
} // namespace deren::engine::pass
