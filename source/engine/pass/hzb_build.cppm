/**
 * @file source/engine/pass/hzb_build.cppm
 * @defgroup engine_pass_hzb_build HZB Build Pass
 * @brief the HZB's producer: a heap-native compute stage that builds the max pyramid (one dispatch per level)
 *        and its header from this frame's G-buffer depth (M3 phase A2).
 *
 * SPEC: docs/superpowers/specs/2026-10-20-hzb-occlusion-design.md §4.1, §4.2, §8.1, §11 phase A. Phase A is
 * BUILD + VALIDATE ONLY: the pass writes only `heap_slots_hzb_levels` and `heap_slots_hzb_header`, no other pass
 * reads them this frame, and nothing culls against them yet - so enabling it cannot change a pixel. That is the
 * acceptance gate (the capture must be byte-identical to the OFF run), and it is why the pass lands before the
 * cull pass that will consume its output (phase B).
 *
 * WHY THE PASS RECORDS ITS OWN BARRIERS BETWEEN LEVELS. Level L's 2x2 max reads level L-1 WHOLE, so adjacent
 * dispatches inside this one pass have a real write -> read dependency. The render graph sees a pass as one
 * node and cannot order commands inside it, so this pass records exactly one buffer barrier
 * `{shader_write -> shader_read}` between adjacent dispatches (§8.1: on the same buffer, COMPUTE -> COMPUTE, the
 * RHI role pair is sufficient - spec C12's mesh-stage gap does not apply here). Everything OUTSIDE the pass is
 * declared by the graph adapter (render_graph pass "hzb build", appended in runtime.post_graph.cpp): the depth
 * sampled read after the raster lane wrote it, and the two HZB buffer writes.
 *
 * WHY THE BUFFERS ARE TARGETS RATHER THAN OWN BINDINGS. The pass records nothing with the buffers except the
 * inter-level barrier above - the shader reaches all three tables through the frame's heap (the per-image depth
 * slot, `heap_slots_hzb_levels`, `heap_slots_hzb_header`), exactly like the visibility resolve and the cluster
 * sort. So there is no descriptor set to resolve; what the recording half needs is the LEVELS buffer's handle
 * for the barrier, which the runtime hands over (`set_hzb_levels`) together with the buffers themselves. The
 * graph's declaration carries the ordering and the transitions, which is the only cross-pass sync shape this
 * renderer allows (spec §8.1).
 */
module;

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

export module deren.engine.pass.hzb_build;

export import deren.utility.shared;

import deren.promise.rhi;
import deren.engine.pass;
import deren.engine.pipelines;
import deren.engine.render_resource;
import deren.engine.visibility.hzb;

export namespace deren::engine::pass {
    namespace rhi = deren::promise::rhi;

    /**
     * THE HEADER'S HOST-SIDE SHAPE (spec §4.1): one 256-byte lane per frame slot, whose word layout lives in
     * source/shaders/common/hzb.slang (`hzb_header_words` / `hzb_header_word_*`) and whose magic is mirrored
     * there as `hzb_magic`.
     *
     * @note THIS PAIR BELONGS IN `deren.engine.visibility.hzb` ONCE PHASE B GIVES THE HEADER A READER: the cull
     *       pass will need the same constants, and `hzb.cppm` is the module that owns "the layout the host and
     *       the shaders share". It is here for phase A because that module is the CPU test's reference and this
     *       slice must not edit it (task-4 scope); the readback probe's metadata check is what pins the two
     *       spellings until then (scripts/measure/hzb_verify.py refuses a header whose magic/extent disagree).
     */
    inline constexpr std::uint32_t hzb_header_byte_capacity = 256u;
    /// the header's first word: "HZB" + version 1; mirrored as `hzb_magic` in common/hzb.slang
    inline constexpr std::uint32_t hzb_header_magic = 0x485A4201u;

    /// the SPIR-V module this pass builds its pipeline from (the app loads and registers it by this name, and the
    /// runtime asks for it before creating the pass - see runtime::ensure_hzb_resources)
    inline constexpr std::string_view hzb_build_shader_name = "hzb_build.comp.spv";

    class hzb_build_pass final : public frame_pass {
    public:
        ~hzb_build_pass() override = default;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override {
            // NO BINDINGS, NO TARGETS, NO DECLARED PUSH BLOCK: every resource travels through the frame's heap (the
            // per-image depth, the two HZB tables, the camera) and the graph's declaration owns the ordering. The
            // push block is composed per dispatch at record time (one `level` lane plus the framework's two heap
            // index lanes) - the same empty-declaration shape the visibility resolve records with.
            static constinit render_resource::pass_io const value{.name = "hzb build"};
            return value;
        }

        [[nodiscard]] deren::engine::pass::behaviour const& behaviour() const noexcept override {
            // ONE 8x8 GROUP PER TILE OF EACH PYRAMID LEVEL: the shader's [numthreads(8, 8, 1)] is the same number,
            // and the `full` extent rule is the frame's render extent - the extent of the G-buffer depth the
            // pyramid is built from, which is also the extent the host sizes the buffer with.
            static deren::engine::pass::behaviour const value{.kind = behaviour_kind::compute,
                                                              .group_size_x = 8,
                                                              .group_size_y = 8,
                                                              .group_size_z = 1,
                                                              .extent = extent_rule::full,
                                                              .resync_viewport = false};
            return value;
        }

        std::string_view feature() const noexcept override {
            return {}; // no feature knob: the DEREN_HZB_BUILD gate (default off) is the runtime's, and whether the pass is appended at all is the graph's decision
        }

        /// @brief whether the pass can record: its pipeline exists (the runtime asks this before appending the lane)
        [[nodiscard]] bool has_pipeline() const noexcept {
            return this->build_pipeline.has_value();
        }
        /// @brief the framework's generic form of the same question
        [[nodiscard]] bool ready() const noexcept override {
            return this->has_pipeline();
        }
        [[nodiscard]] rhi::pipeline* pipeline_handle() const noexcept override {
            return this->build_pipeline.has_value() ? this->build_pipeline->contract : nullptr;
        }

        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const&) override {
            // nothing is per-generation: the pipeline depends on the device alone, and the buffers arrive per frame
            // from the runtime (which rebuilds them on a resize and re-hands them through set_hzb_levels)
        }

        /**
         * @brief hand the pass the LEVELS buffer the inter-level barriers are recorded on
         * @param levels the runtime-owned HZB pyramid buffer, or null when this frame has none (the pass then
         *        refuses to record rather than recording an unordered sequence of dispatches)
         * @note the buffer itself lives in the RUNTIME (spec §5.2's runtime-owned fallback), because its lane
         *       stride depends on the render extent and the resource manager's scene tables do not fit a
         *       GPU-only, extent-dependent table (see runtime::ensure_hzb_resources's note). The pass borrows the
         *       handle for exactly one pass's recording.
         */
        void set_hzb_levels(rhi::buffer* levels) noexcept {
            this->levels_buffer = levels;
        }

        [[nodiscard]] rhi::error record_content(resolved_io const& io) noexcept override;

    private:
        std::optional<pipelines::pipeline_handle> build_pipeline = std::nullopt;
        void* built_against = nullptr;
        rhi::buffer* levels_buffer = nullptr;
    };
} // namespace deren::engine::pass
