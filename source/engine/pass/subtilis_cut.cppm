// The GPU cut job: one workgroup per planned item, the immutable DAG payload already resident, and the
// selected run written straight into the meshlet table and the mesh-task command.

module;

#include <cstdint>
#include <glm/glm.hpp>

export module deren.engine.pass.subtilis_cut;

export import deren.vstd;
export import deren.utility.shared;
import deren.promise.rhi;

import deren.engine.pass;
import deren.engine.pipelines;

export namespace deren::engine::pass {

    /// The compute shader's push block: the per-frame cut payload's address plus the dispatch's item count.
    struct subtilis_cut_push_constants {
        glm::uvec2 items = glm::uvec2(0u);
        std::uint32_t item_count = 0;
        std::uint32_t reserved = 0;
    };
    static_assert(sizeof(subtilis_cut_push_constants) == 16u);

    /**
     * @brief Holds the cut pipeline and records the single dispatch plus the ordering barriers the raster needs.
     *
     * The shader writes the meshlet records and the mesh-task command. A later draw of the same frame reads
     * both, so the command needs SHADER_WRITE -> INDIRECT_READ and the table needs SHADER_WRITE ->
     * SHADER_READ before the first main-view dispatch.
     */
    class subtilis_cut_job {
    public:
        subtilis_cut_job() = default;
        ~subtilis_cut_job();

        subtilis_cut_job(subtilis_cut_job const&) = delete ("subtilis_cut_job is non-copyable");
        subtilis_cut_job& operator=(subtilis_cut_job const&) = delete ("subtilis_cut_job is non-copyable");
        subtilis_cut_job(subtilis_cut_job&&) = default;
        subtilis_cut_job& operator=(subtilis_cut_job&&) = delete ("subtilis_cut_job cannot be move-assigned");

        [[nodiscard]] std::expected<void, deren::utility::vessel::string> create(pass_context const& context);
        [[nodiscard]] bool record(deren::promise::rhi::command_buffer& commands, std::uint64_t items, std::uint32_t item_count,
                                  deren::promise::rhi::buffer& command_buffer, deren::promise::rhi::buffer& meshlet_table) const noexcept;
        [[nodiscard]] bool ready() const noexcept;

    private:
        static constexpr std::string_view shader_name = "subtilis_cut.comp.spv";
        void release_contract() noexcept;
        deren::promise::rhi::api_core* built_against = nullptr;
        std::optional<pipelines::pipeline_handle> pass_pipeline = std::nullopt;
    };
} // namespace deren::engine::pass
