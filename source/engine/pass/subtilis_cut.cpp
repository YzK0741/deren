// The GPU cut job's implementation: its pipeline and the one dispatch per frame plus the two resource
// barriers that make the compute writes visible to the mesh-task draw.

module;

#include <array>
#include <cstdint>
#include <glm/glm.hpp>

module deren.engine.pass.subtilis_cut;
import deren.vstd;
import deren.utility.shared;
import deren.engine.pipelines;
import deren.utility;

namespace deren::engine::pass {

    subtilis_cut_job::~subtilis_cut_job() {
        this->release_contract();
    }

    void subtilis_cut_job::release_contract() noexcept {
        this->pass_pipeline.reset();
    }

    bool subtilis_cut_job::ready() const noexcept {
        return this->pass_pipeline.has_value();
    }

    std::expected<void, deren::utility::vessel::string> subtilis_cut_job::create(pass_context const& context) {
        if (context.face == nullptr) {
            return std::unexpected(deren::utility::vessel::string("subtilis cut: no device"));
        }
        if (this->built_against != nullptr && this->built_against != context.face) {
            this->release_contract();
        }
        this->built_against = context.face;
        std::span<uint8_t const> const spirv = context.shader != nullptr ? context.shader(context.parent, shader_name) : std::span<uint8_t const>{};
        if (spirv.empty()) {
            return std::unexpected(deren::utility::vessel::string("subtilis cut: the parent has no ") + deren::utility::vessel::string(shader_name));
        }
        auto built = pipelines::build_subtilis_cut(*context.face, spirv);
        if (!built) {
            return std::unexpected(std::move(built.error()));
        }
        this->pass_pipeline = std::move(built->trace);
        deren::utility::log("SUCCESS: GPU subtilis cut pipeline created");
        return {};
    }

    bool subtilis_cut_job::record(deren::promise::rhi::command_buffer& commands, std::uint64_t const items, std::uint32_t const item_count,
                                  deren::promise::rhi::buffer& command_buffer, deren::promise::rhi::buffer& meshlet_table) const noexcept {
        if (!this->ready() || item_count == 0u || this->pass_pipeline->contract == nullptr) {
            return false;
        }
        if (commands.bind_pipeline(*this->pass_pipeline->contract) != deren::promise::rhi::error::ok) {
            return false;
        }
        subtilis_cut_push_constants const push{
            .items = glm::uvec2(static_cast<std::uint32_t>(items & 0xFFFFFFFFu), static_cast<std::uint32_t>(items >> 32u)),
            .item_count = item_count,
            .reserved = 0u,
        };
        if (commands.push_constants(0u, std::as_bytes(std::span(&push, 1))) != deren::promise::rhi::error::ok) {
            return false;
        }
        commands.dispatch(item_count, 1u, 1u);
        // The shader wrote BOTH the selected records and the command. The command is the one the raster's
        // indirect read consumes, so it must be visible before the record step's draw calls; the records are
        // consumed by the mesh stage of the same draw.
        std::array<deren::promise::rhi::buffer_barrier, 2> const barriers{{
            {.resource = &command_buffer, .from = deren::promise::rhi::buffer_use::shader_write, .to = deren::promise::rhi::buffer_use::indirect_read},
            {.resource = &meshlet_table, .from = deren::promise::rhi::buffer_use::shader_write, .to = deren::promise::rhi::buffer_use::shader_read},
        }};
        if (commands.barrier(deren::promise::rhi::barrier_group{.buffers = barriers}) != deren::promise::rhi::error::ok) {
            deren::utility::log("subtilis cut: the write-to-raster barrier was refused - the GPU cut would draw with stale records");
            return false;
        }
        return true;
    }
} // namespace deren::engine::pass
