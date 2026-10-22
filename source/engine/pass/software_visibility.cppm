/**
 * @file source/engine/pass/software_visibility.cppm
 * @defgroup engine_pass_software_visibility Software Visibility Pass
 * @brief the SOFTWARE RASTER's recording half (M4 Task 2 scaffolding; docs/superpowers/specs/2026-10-22-software-raster-
 *        design.md §3.2/§4.2): a heap-native compute pass that rasterizes the frame's subpixel records into two private
 *        R32_UINT images, tile by tile, one workgroup per 16x16 tile.
 *
 * WHY THIS PASS EXISTS AS A PASS. The hybrid frame splits its opaque visibility into two producers: the hardware
 * raster (`visibility::append`, which owns the frame's real G-buffer depth and id) and this pass, which re-rasterizes
 * ONLY the items the CPU classified as subpixel (spec §3.1) into its own depth/id pair. The resolve then picks between
 * the two candidates per pixel, and the merge writes min(hardware, software) back into the frame's depth. Keeping the
 * software result in PRIVATE images is what makes the lane additive: nothing consumes it until the resolve's optional
 * reads are declared, and a frame with the lane OFF never creates these images at all.
 *
 * WHY ITS DECLARATION IS EMPTY. Every input and output travels through the FRAME'S HEAP (the two software storage
 * images, the camera block, the item/meshlet/tile/statistics tables), so the pass binds no descriptor set and renders
 * no attachment - the same shape as the visibility resolve and the cluster sort. The graph lane
 * (`render_graph/visibility.cppm`'s `append_software_raster`) owns what it reads and writes and the order it sits in.
 *
 * THE EXTENT IS THE FRAME'S (`extent_rule::full`), because the dispatch covers the render extent: `tile_extent` is the
 * CSR list's tile size and the group is 8x8 threads with four pixels each, so the two numbers are one contract
 * (16 = 8 x 2, and four pixels per thread covers the 16x16 tile).
 *
 * SCAFFOLDING, DELIBERATELY: M4 Task 2 lands this object, its pipeline and the tests that pin the declaration; Task 3
 * fills the tiled walk inside `software_visibility.slang`. The recording half is already the final shape - one
 * dispatch over the tile grid - so Task 3 only changes the shader.
 */
module;

#include <cstdint>
#include <optional>
#include <string_view>

export module deren.engine.pass.software_visibility;
export import deren.utility.shared;

import deren.promise.rhi;
import deren.engine.pass;
import deren.engine.pipelines;
import deren.engine.render_resource;
import deren.engine.visibility.software;

export namespace deren::engine::pass {
    namespace rhi = deren::promise::rhi;

    class software_visibility_pass final : public frame_pass {
    public:
        /// THE TILE EDGE, in pixels: the CSR tile list's tile is the same number, so the pass and the CPU binning
        /// cannot disagree about what "one workgroup per tile" means.
        static constexpr std::uint32_t tile_extent = visibility::software::tile_size;
        /// the compute stage's [numthreads] edge (8x8 threads, four pixels each over a 16x16 tile)
        static constexpr std::uint32_t group_size = 8u;

        software_visibility_pass() = default;
        ~software_visibility_pass() override = default;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override {
            // NO BINDINGS, NO TARGETS, NO PUSH BLOCK DECLARED: the resources arrive through the frame's heap and the
            // graph's software lane owns the ordering. An empty declaration is the accurate one for a heap-native
            // compute pass - the clustered-light sort and the visibility resolve are the precedents.
            static constinit render_resource::pass_io const value{.name = "software visibility"};
            return value;
        }

        [[nodiscard]] deren::engine::pass::behaviour const& behaviour() const noexcept override {
            // ONE 8x8 GROUP PER 16x16 TILE over the whole render extent: the shader's [numthreads(8, 8, 1)] is the same
            // number, and the record step dispatches one group per tile - so a resize needs no state here.
            static deren::engine::pass::behaviour const value{.kind = behaviour_kind::compute,
                                                              .group_size_x = group_size,
                                                              .group_size_y = group_size,
                                                              .group_size_z = 1u,
                                                              .extent = extent_rule::full,
                                                              .resync_viewport = false};
            return value;
        }

        std::string_view feature() const noexcept override {
            return {}; // no feature knob: whether the lane exists is the graph's decision (Task 4 wires the selection)
        }

        /// @brief whether the pass has the pipeline it needs (not `ready()`: the framework owns that name)
        [[nodiscard]] bool has_pipeline() const noexcept {
            return this->visibility_pipeline.has_value();
        }
        /// @brief the framework's generic form of the same question (the declaration-driven resolver asks it)
        [[nodiscard]] bool ready() const noexcept override {
            return this->has_pipeline();
        }
        [[nodiscard]] rhi::pipeline* pipeline_handle() const noexcept override {
            return this->visibility_pipeline.has_value() ? this->visibility_pipeline->contract : nullptr;
        }

        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const&) override {
            // nothing is per-generation: the pipeline depends on the device alone, and the images arrive per frame
        }

        [[nodiscard]] rhi::error record_content(resolved_io const& io) noexcept override;

    private:
        static constexpr std::string_view shader_name = "software_visibility.comp.spv";
        std::optional<pipelines::pipeline_handle> visibility_pipeline = std::nullopt;
        void* built_against = nullptr;
    };
} // namespace deren::engine::pass
