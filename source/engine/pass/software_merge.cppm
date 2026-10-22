/**
 * @file source/engine/pass/software_merge.cppm
 * @defgroup engine_pass_software_merge Software Merge Pass
 * @brief the SOFTWARE RASTER's depth-only merge (M4 Task 2 scaffolding; docs/superpowers/specs/2026-10-22-software-
 *        raster-design.md §3.2/§5.2): one fullscreen depth-only graphics pass that folds the software image's depth
 *        into the frame's own G-buffer depth with LESS_OR_EQUAL + depth write, i.e. the write IS `min(hw, sw)`.
 *
 * WHY THIS IS A GRAPHICS PASS AND NOT A COMPUTE STAGE. The merge must change the SAME depth image the lighting and
 * the HZB read, and a depth write with the test enabled is what gives `min(hardware, software)` for free - including
 * the design's tie rule (an equal value writes the same value, so a tie changes nothing). A compute image would need a
 * storage write of the G-buffer depth AND a separate depth-usage descriptor, a second route to one image.
 *
 * ITS DECLARATION IS ONE DEPTH TARGET and nothing else: the frame's own `gbuffer_depth`, kind `depth` (the schema's
 * `target_kind::depth`). No colour attachment, no own bindings - the software images arrive through the frame's heap as
 * sampled descriptors, and the fragment stage samples them to decide its SV_Depth value. The load op is LOAD (the
 * hardware depth is the comparison's other half) and the store is STORE (the merge's result is what later stages see);
 * both are the pass's knowledge at record time, which is why they are not declared.
 *
 * ORDER IS THE CORRECTNESS ARGUMENT (spec §3.2): the resolve reads the UNMERGED hardware depth and the software
 * candidates, so this pass must record AFTER the resolve. Its graph lane
 * (`render_graph/visibility.cppm`'s `append_software_merge`) declares exactly that edge.
 *
 * SCAFFOLDING, DELIBERATELY: M4 Task 2 lands this object, its depth-only pipeline and the tests that pin the
 * declaration; Task 3 fills the fragment stage's sampling and depth computation.
 */
module;

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

export module deren.engine.pass.software_merge;
export import deren.utility.shared;

import deren.promise.rhi;
import deren.engine.pass;
import deren.engine.pipelines;
import deren.engine.render_resource;

export namespace deren::engine::pass {
    namespace rhi = deren::promise::rhi;

    class software_merge_pass final : public frame_pass {
    public:
        software_merge_pass() = default;
        ~software_merge_pass() override = default;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override {
            // ONE DEPTH TARGET, THE FRAME'S OWN G-BUFFER DEPTH. The software images are read through the frame's heap
            // (sampled descriptors), so they are not bindings; the only thing a declaration has to say is the
            // attachment this pass renders into, and that it fills the DEPTH slot rather than a colour one.
            static constexpr std::array<render_resource::render_target, 1> merge_targets{{{
                .resource = render_resource::resource_id::gbuffer_depth,
                .element = 0,
                .kind = render_resource::target_kind::depth,
            }}};
            static constinit render_resource::pass_io const value{.name = "software merge", .targets = merge_targets};
            return value;
        }

        [[nodiscard]] deren::engine::pass::behaviour const& behaviour() const noexcept override {
            // A GRAPHICS pass over the FULL extent (one fullscreen triangle at the render resolution), and the
            // viewport must be re-synced from the resolved extent because this pass's pipeline is its OWN object
            // rather than a name in the renderer's registry.
            static deren::engine::pass::behaviour const value{.kind = behaviour_kind::graphics,
                                                              .group_size_x = 8u,
                                                              .group_size_y = 8u,
                                                              .group_size_z = 1u,
                                                              .extent = extent_rule::full,
                                                              .resync_viewport = true};
            return value;
        }

        std::string_view feature() const noexcept override {
            return {}; // no feature knob: whether the lane exists is the graph's decision (Task 4 wires the selection)
        }

        /// @brief whether the pass has the depth-only pipeline it needs (not `ready()`: the framework owns that name)
        [[nodiscard]] bool has_pipeline() const noexcept {
            return this->merge_pipeline.has_value();
        }
        /// @brief the framework's generic form of the same question (the declaration-driven resolver asks it)
        [[nodiscard]] bool ready() const noexcept override {
            return this->has_pipeline();
        }
        [[nodiscard]] rhi::pipeline* pipeline_handle() const noexcept override {
            return this->merge_pipeline.has_value() ? this->merge_pipeline->contract : nullptr;
        }

        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const&) override {
            // nothing is per-generation: the pipeline depends on the depth format (a device fact) alone
        }

        [[nodiscard]] rhi::error record_content(resolved_io const& io) noexcept override;

    private:
        static constexpr std::string_view vertex_shader_name = "software_merge.vert.spv";
        static constexpr std::string_view fragment_shader_name = "software_merge.frag.spv";
        std::optional<pipelines::pipeline_handle> merge_pipeline = std::nullopt;
        void* built_against = nullptr;
    };
} // namespace deren::engine::pass
