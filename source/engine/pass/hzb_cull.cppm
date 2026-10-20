/**
 * @file source/engine/pass/hzb_cull.cppm
 * @defgroup engine_pass_hzb_cull HZB Cull Pass
 * @brief the HZB's consumer on the GPU: one workgroup per item, walking that item's host-selected candidates and
 *        testing each conservatively against the history pyramid (M3 phase B2).
 *
 * The cull publishes stable survivors, counts and per-item mesh-task commands. Runtime declares compute writes
 * and mesh/resolve/indirect reads in the render graph; its scoped barriers own all cross-pass synchronization.
 * Rejected runs publish UINT32_MAX and preserve the host command: raster and resolve both fetch the original
 * host-culled run. Unsupported items stay on their original path. The runtime's byte-exact static gate controls
 * admission (design sections 8/9); no valid history means the original visibility lane.
 */
module;

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

export module deren.engine.pass.hzb_cull;

export import deren.utility.shared;

import deren.promise.rhi;
import deren.engine.pass;
import deren.engine.pipelines;
import deren.engine.render_resource;
import deren.engine.visibility;
import deren.engine.visibility.hzb;

export namespace deren::engine::pass {
    namespace rhi = deren::promise::rhi;

    /// the SPIR-V module this pass builds its pipeline from (the app loads and registers it by this name, and the
    /// runtime asks for it before creating the pass - see runtime::ensure_hzb_resources)
    inline constexpr std::string_view hzb_cull_shader_name = "hzb_cull.comp.spv";

    // ---- THE HISTORY HEADER'S WORD OFFSETS THE CULL READS (spec §4.1) ----
    // `common/hzb.slang` owns the header's word layout and the probe/verify read the SAME lanes back; the lane's byte
    // capacity and magic already live in pass/hzb_build.cppm (the writer's constants), so only the three offsets the
    // reader names are mirrored here. A drift is not a validation error - it is the cull testing against another
    // lane's matrices - which is why the probe's metadata check pins the two spellings.
    inline constexpr std::uint32_t hzb_header_word_view = 4u;        ///< the view matrix's 16 float words
    inline constexpr std::uint32_t hzb_header_word_proj = 20u;       ///< the projection's 16 float words
    inline constexpr std::uint32_t hzb_header_word_camera_pos = 36u; ///< the eye's 3 words (word 39 is the vec4 pad)

    // ================================================================================================
    // THE THREE PHASE-B TABLE LAYOUTS (spec §5.1), mirrored by `common/hzb.slang` and pinned here:
    //   heap_slots_hzb_items      - HOST-written per-item candidate descriptions (this file's `hzb_item`)
    //   heap_slots_hzb_results    - GPU-written flags/counts/stats (the word offsets below)
    //   heap_slots_hzb_survivors  - GPU-written compacted records (the meshlet table's own shape)
    // The counts are the `test_render_resources`-style contract: the shader spells the same numbers, and a drift is a
    // pass writing a table the probe/verify cannot explain rather than a validation error.
    // ================================================================================================

    /// one frame lane of `heap_slots_hzb_items` holds one description per possible item (the packed id's item field)
    inline constexpr std::uint32_t hzb_cull_item_capacity = deren::engine::visibility::id_item_capacity; // 4096

    /// the candidate records one frame lane of the flags/survivors tables can address (the meshlet table's budget)
    inline constexpr std::uint32_t hzb_cull_candidate_capacity = deren::engine::visibility::hzb::hzb_candidate_capacity; // 65536

    /// `HzbItem.source`: the candidate records are NOT in the host-culled lane, so the cull walks nothing for the
    /// item this phase (shared source is not tested in phase B; acceptance uses host-cut subtilis runs)
    inline constexpr std::uint32_t hzb_item_source_unculled = 0u;
    /// `HzbItem.source`: the candidate records are `heap_slots_hzb_culled` at `meshlet_base + i`
    inline constexpr std::uint32_t hzb_item_source_culled = 1u;

    /**
     * @brief one item's candidate description, written by the host while the frame records (spec §4.3)
     *
     * WHY THE HOST ENUMERATES ITEMS AND NOT CANDIDATES: the item's candidate run is already compacted into the
     * host-culled table at the primitive's own `meshlet_base` (the cut the visibility lane records through), so the
     * GPU only needs where the run starts and how long it is. Writing one 16-byte entry per item replaces a
     * per-candidate upload and keeps the host's per-frame cost at the table write.
     */
    struct hzb_item {
        std::uint32_t meshlet_base = 0;    ///< the run's first candidate index in its source table
        std::uint32_t candidate_count = 0; ///< candidates the run holds; 0 = this item offers the cull nothing
        std::uint32_t source = 0;          ///< `hzb_item_source_*`
        std::uint32_t pad = 0;             ///< original host dispatch count; probe verifies commands (16 B entry)
    };
    static_assert(sizeof(hzb_item) == 16u, "the shader's HzbItem is four u32 words - the entry must be exactly 16 B");

    // ---- heap_slots_hzb_results: the word offsets of ONE frame lane (bytes = words * 4) ----
    /// one u32 per candidate: 0 = survivor, 1 = culled (the CPU reference's hzb_decision values, §6.7)
    inline constexpr std::uint32_t hzb_results_flags_offset = 0u;
    /// one u32 per item: the number of survivors this frame's cull kept
    inline constexpr std::uint32_t hzb_results_pass1_count_offset = hzb_cull_candidate_capacity;
    /// one u32 per item: phase C's recovery survivors (the layout is reserved now so its offsets never move)
    inline constexpr std::uint32_t hzb_results_pass2_count_offset = hzb_results_pass1_count_offset + hzb_cull_item_capacity;
    /// the stats block: four rows this phase accumulates atomically, the rest reserved
    inline constexpr std::uint32_t hzb_results_stats_offset = hzb_results_pass2_count_offset + hzb_cull_item_capacity;
    inline constexpr std::uint32_t hzb_results_stats_capacity = 16u;
    /// one frame lane's size in words/bytes (the buffer is `slot_count` lanes inside ONE descriptor)
    inline constexpr std::uint32_t hzb_results_words_per_lane = hzb_results_stats_offset + hzb_results_stats_capacity;
    inline constexpr std::uint64_t hzb_results_bytes_per_lane = static_cast<std::uint64_t>(hzb_results_words_per_lane) * sizeof(std::uint32_t);

    /// the stats block's rows (§10.3): the four numbers the shutdown line and the probe's checks read
    inline constexpr std::uint32_t hzb_stat_candidates = 0u;     ///< candidates the cull walked
    inline constexpr std::uint32_t hzb_stat_culled = 1u;         ///< ... conservatively dismissed
    inline constexpr std::uint32_t hzb_stat_survivors = 2u;      ///< ... kept
    inline constexpr std::uint32_t hzb_stat_overflow_items = 3u; ///< items whose run could not be described (§9.4)

    // THE SHADER'S MIRRORS OF THE DRAW-ITEM RECORD, asserted here so the GPU's model fetch cannot drift from the
    // host's record silently: hzb_cull.slang reads the model's 16 floats at dword 12 of a 28-dword row.
    static_assert(deren::engine::visibility::draw_item_bytes == 112u);
    static_assert(deren::engine::visibility::draw_item_model_offset == 48u);
    static_assert(deren::engine::visibility::draw_item_model_offset == 12u * sizeof(std::uint32_t));
    static_assert(deren::engine::visibility::draw_item_bytes == 28u * sizeof(std::uint32_t));

    class hzb_cull_pass final : public frame_pass {
    public:
        ~hzb_cull_pass() override = default;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override {
            // NO BINDINGS, NO TARGETS, NO DECLARED PUSH BLOCK: every resource travels through the frame's heap and
            // the graph's declaration owns the ordering (the same empty-declaration shape the build pass records
            // with). The push block is composed per frame from `set_frame`'s values plus the framework's index lanes.
            static constinit render_resource::pass_io const value{.name = "hzb cull"};
            return value;
        }

        [[nodiscard]] deren::engine::pass::behaviour const& behaviour() const noexcept override {
            // ONE WORKGROUP PER ITEM, 64 THREADS (the shader's [numthreads(64, 1, 1)]): the dispatch is
            // `item_count` groups, which the host knows exactly (§2.1 - no indirect dispatch is needed for the cull
            // itself), and the extent rule is the frame's so a resize changes nothing about the pass.
            static deren::engine::pass::behaviour const value{.kind = behaviour_kind::compute,
                                                              .group_size_x = 64,
                                                              .group_size_y = 1,
                                                              .group_size_z = 1,
                                                              .extent = extent_rule::full,
                                                              .resync_viewport = false};
            return value;
        }

        std::string_view feature() const noexcept override {
            return {}; // no feature knob: the DEREN_HZB_CULL gate (default off) is the runtime's, and whether the lane is appended at all is the graph's decision
        }

        /// @brief whether the pass can record: its pipeline exists (the runtime asks this before appending the lane)
        [[nodiscard]] bool has_pipeline() const noexcept {
            return this->cull_pipeline.has_value();
        }
        /// @brief the framework's generic form of the same question
        [[nodiscard]] bool ready() const noexcept override {
            return this->has_pipeline();
        }
        [[nodiscard]] rhi::pipeline* pipeline_handle() const noexcept override {
            return this->cull_pipeline.has_value() ? this->cull_pipeline->contract : nullptr;
        }

        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const&) override {
            // nothing is per-generation: the pipeline depends on the device alone and the tables arrive per frame
        }

        /**
         * @brief hand the pass this frame's dispatch shape: how many items, and the extent the history header must
         *        describe before its pyramid may be indexed at all (§7.2's header guard)
         * @note called by the runtime once per frame it appends the lane, before the graph records
         */
        void set_frame(std::uint32_t const item_count, rhi::image_extent const extent) noexcept {
            this->frame_item_count = item_count;
            this->frame_extent = extent;
        }

        [[nodiscard]] rhi::error record_content(resolved_io const& io) noexcept override;

    private:
        std::optional<pipelines::pipeline_handle> cull_pipeline = std::nullopt;
        void* built_against = nullptr;
        std::uint32_t frame_item_count = 0;
        rhi::image_extent frame_extent = {};
        // Cross-pass visibility of results/survivors/commands belongs to scoped graph barriers.
    };
} // namespace deren::engine::pass
