/**
 * @file source/engine/pass/visibility_resolve.cppm
 * @defgroup engine_pass_visibility_resolve Visibility Resolve Pass
 * @brief the VISIBILITY BUFFER's consumer: a compute stage that turns one packed id per pixel back into the five
 *        G-buffer targets (visibility-buffer v0.4c).
 *
 * WHY THIS PASS EXISTS AS A PASS. The visibility buffer splits the main view's opaque geometry into two stages: the
 * raster pass (`visibility_raster`) writes depth + one id per pixel, and this pass reconstructs everything the old
 * G-buffer fragment stage used to write - albedo/metallic, world normal/roughness, material id/AO/flags, velocity and
 * the emissive term - by reading the id back, fetching the triangle it names out of the per-frame draw-item and
 * meshlet tables, and running the SAME material decode and five-target composition the fragment stage runs
 * (`scene/gbuffer_body.slang`). It owns the pipeline, the dispatch and the two heap indices its push block carries;
 * the per-pixel arithmetic is the shader's.
 *
 * WHY ITS DECLARATION IS EMPTY. Every input and output travels through the FRAME'S HEAP (the id/depth sampled
 * descriptors, the five storage twins, the camera/material/item/meshlet/previous/statistics descriptors), so the
 * pass binds no descriptor set and renders no attachment - exactly like the clustered-light sort. What the graph
 * needs from it is the ORDERING (which images it reads and writes, at which stages), and that is declared by the
 * resolve lane in `render_graph/visibility.cppm`, whose parameters the runtime binds to this pass. This object is
 * therefore the RECORDING half only: bind the pipeline the declaration resolved, push the two heap indices, dispatch
 * one 8x8 group per tile of the render extent.
 *
 * THE EXTENT IS THE FRAME'S (`extent_rule::full`): the dispatch covers the render extent, one thread per pixel, which
 * is exactly the shape the images are sized for - the shader derives the extent from the storage image itself, so a
 * resize cannot leave a stale dimension in a push block.
 *
 * NOT THE G-BUFFER'S ONLY WRITER, YET. v0.4c records this pass BESIDE the old geometry lane, ordered BEFORE it, so
 * the geometry lane's clear-and-draw overwrites the resolved pixels and the frame stays byte-identical - that is what
 * lets the pass land (and be validated, and be proven by its failure counters) without moving a picture. v0.5 is the
 * step that REPLACES the geometry lane on an eligible frame, behind the `[render] visibility_buffer` switch.
 */
module;

#include <cstdint>
#include <optional>
#include <string_view>

export module deren.engine.pass.visibility_resolve;
export import deren.utility.shared;

import deren.promise.rhi;
import deren.engine.pass;
import deren.engine.pipelines;
import deren.engine.render_resource;

export namespace deren::engine::pass {
    namespace rhi = deren::promise::rhi;

    class visibility_resolve_pass final : public frame_pass {
    public:
        ~visibility_resolve_pass() override = default;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override {
            // NO BINDINGS, NO TARGETS, NO PUSH BLOCK DECLARED: the resources arrive through the frame's heap and the
            // graph's resolve lane owns the ordering; the push block is composed at record time (see
            // record_content). An empty declaration is the accurate one for a heap-native compute pass - the
            // clustered-light sort is the precedent.
            static constinit render_resource::pass_io const value{.name = "visibility resolve"};
            return value;
        }

        [[nodiscard]] deren::engine::pass::behaviour const& behaviour() const noexcept override {
            // ONE 8x8 GROUP PER PIXEL TILE over the whole render extent: the shader's [numthreads(8, 8, 1)] is the
            // same number, and the record step sizes the grid from the extent - so a resize needs no state.
            static deren::engine::pass::behaviour const value{.kind = behaviour_kind::compute,
                                                              .group_size_x = 8,
                                                              .group_size_y = 8,
                                                              .group_size_z = 1,
                                                              .extent = extent_rule::full,
                                                              .resync_viewport = false};
            return value;
        }

        std::string_view feature() const noexcept override {
            return {}; // no feature knob: whether the lane exists is the graph's decision (and the frame's fallback)
        }

        /// @brief whether the pass has the pipeline it needs (not `ready()`: the framework owns that name)
        [[nodiscard]] bool has_pipeline() const noexcept {
            return this->resolve_pipeline.has_value();
        }
        /// @brief the framework's generic form of the same question (the declaration-driven resolver asks it)
        [[nodiscard]] bool ready() const noexcept override {
            return this->has_pipeline();
        }
        [[nodiscard]] rhi::pipeline* pipeline_handle() const noexcept override {
            return this->resolve_pipeline.has_value() ? this->resolve_pipeline->contract : nullptr;
        }

        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const&) override {
            // nothing is per-generation: the pipeline depends on the device alone, and the images arrive per frame
        }

        [[nodiscard]] rhi::error record_content(resolved_io const& io) noexcept override;

    private:
        static constexpr std::string_view shader_name = "visibility_resolve.comp.spv";
        std::optional<pipelines::pipeline_handle> resolve_pipeline = std::nullopt;
        void* built_against = nullptr;
    };
} // namespace deren::engine::pass
