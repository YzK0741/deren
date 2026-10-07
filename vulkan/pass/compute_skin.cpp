// The compute-skinning job's implementation: the pipeline it owns and the per-caster dispatch plus the
// build-ordering barrier. Moved out of `runtime::make_compute_skin_pipeline` and
// `runtime::record_compute_skin_pass` UNCHANGED in behaviour - the same layout-free heap path, the same
// 32-byte push block, the same 64-wide workgroup, the same one dispatch per skinned caster
// and the same single memory barrier at the end - so the A/B against the parent commit decides it.

module;

#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

module deren.vulkan.pass.compute_skin;

import deren.vulkan.pipelines; // build_compute_skin: the compute pipeline this job owns
import deren.utility;

namespace deren::vulkan::pass {

    compute_skin_job::~compute_skin_job() {
        this->release_owned();
    }

    void compute_skin_job::release_owned() noexcept {
        this->pass_pipeline.reset();
    }

    bool compute_skin_job::ready() const noexcept {
        return this->pass_pipeline.has_value();
    }

    VkPipeline compute_skin_job::pipeline() const noexcept {
        return this->pass_pipeline.has_value() ? this->pass_pipeline->get_pipeline() : VK_NULL_HANDLE;
    }

    std::expected<void, std::string> compute_skin_job::create(pass_context const& context) {
        if (context.face == nullptr) {
            return std::unexpected(std::string("compute skin: no device"));
        }
        if (this->built_against != nullptr && this->built_against != context.face) {
            this->release_owned();
        }
        this->built_against = context.face;
        std::span<uint8_t const> const spirv = context.shader != nullptr ? context.shader(context.owner, shader_name) : std::span<uint8_t const>{};
        if (spirv.empty()) {
            return std::unexpected(std::string("compute skin: the owner has no ") + std::string(shader_name));
        }
        // The per-joint matrices the dispatch reads are a heap slot the shader names itself (see the header), so
        // nothing about the renderer's buffers is handed in and the pipeline is all this job builds.
        auto built = pipelines::build_compute_skin(*context.face, spirv);
        if (!built) {
            return std::unexpected(std::move(built.error()));
        }
        this->pass_pipeline = std::move(built->trace);
        deren::utility::log("SUCCESS: compute skinning pipeline created (skinned casters can be refitted per frame)");
        return {};
    }

    bool compute_skin_job::record(deren::promise::rhi::command_buffer& commands, std::span<compute_skin_request const> const requests, void* const push_owner,
                                  bool (*push_indices)(void* owner, deren::promise::rhi::command_buffer& commands, std::span<std::byte const> bytes, uint32_t extra_lane)) const noexcept {
        if (!this->ready() || requests.empty() || push_indices == nullptr) {
            return false;
        }
        // THE BIND RIDES THE CONTRACT (abi 21): this job's pipeline IS a contract pipeline (the builders create
        // through `api_core::create_pipeline`), so `bind_pipeline` carries the compute bind point the backend
        // decided, and there is no raw fallback left to keep - a job whose pipeline has no contract handle is a
        // wiring bug, and it is REPORTED rather than recorded into the void. Nothing else binds for this job: it
        // is not a frame pass, the acceleration-structure set drives it.
        if (!this->pass_pipeline.has_value() || this->pass_pipeline->contract == nullptr) {
            deren::utility::log("compute skin: the job's pipeline has no contract handle - the skin dispatches are skipped");
            return false;
        }
        if (commands.bind_pipeline(*this->pass_pipeline->contract) != deren::promise::rhi::error::ok) {
            return false; // a refused bind would record the dispatches with no pipeline bound
        }
        // No descriptor set is bound: the per-joint matrices are a heap slot the shader names itself, and a set
        // bound to a layout-less pipeline is invalid. The block travels as data (see the header) with the two heap
        // indices appended, which is how the shader finds the frame's matrices at all.

        bool recorded = false;
        for (compute_skin_request const& request : requests) {
            if (request.destination == 0 || request.vertex_count == 0) {
                continue; // not a skinned caster: its geometry is what the build read, unchanged
            }
            compute_skin_push_constants const push = {
                .source_vertices = glm::uvec2(static_cast<uint32_t>(request.source_vertices & 0xFFFFFFFFu), static_cast<uint32_t>(request.source_vertices >> 32u)),
                .destination = glm::uvec2(static_cast<uint32_t>(request.destination & 0xFFFFFFFFu), static_cast<uint32_t>(request.destination >> 32u)),
                .source_stride = request.source_stride,
                .destination_stride = request.destination_stride,
                .vertex_count = request.vertex_count,
                .skin_base = request.skin_base,
            };
            [[maybe_unused]] bool const pushed = push_indices(push_owner, commands, std::as_bytes(std::span(&push, 1)), 0u);
            commands.dispatch((push.vertex_count + group_size - 1u) / group_size, 1, 1);
            recorded = true;
        }
        if (!recorded) {
            return false;
        }

        // THE BUILD-ORDERING BARRIER RIDES THE CONTRACT NOW (abi 21), and it is the site the GLOBAL memory
        // barrier was added for: a memory barrier has NO operand (it orders every write of a stage pair against
        // every read of the next), so it could not be a `buffer_barrier` or an `image_barrier` - it is
        // `barrier_group::has_memory` + a role pair, recorded in the same call as the resource barriers.
        // THE PAIR IS THE MEASUREMENT: (shader_write, acceleration_structure_read) is exactly the raw masks this
        // replaced (COMPUTE_SHADER/SHADER_WRITE -> ACCELERATION_STRUCTURE_BUILD/SHADER_READ), and no earlier
        // role named the acceleration-structure build as a READER (see `buffer_use`).
        deren::promise::rhi::barrier_group const order{
            .struct_size = sizeof(deren::promise::rhi::barrier_group),
            .images = {},
            .buffers = {},
            .stage = deren::promise::rhi::stage_hint::none,
            .has_memory = true,
            .memory = deren::promise::rhi::memory_barrier{.from = deren::promise::rhi::buffer_use::shader_write,
                                                          .to = deren::promise::rhi::buffer_use::acceleration_structure_read},
        };
        if (commands.barrier(order) != deren::promise::rhi::error::ok) {
            // A REFUSED ORDERING BARRIER IS NOT DROPPED SILENTLY: the dispatches are already recorded and the
            // structure build would read the previous frame's vertices (a shadow one frame behind, which reads
            // as animation lag). The failure is named, and the job reports it as a failure.
            deren::utility::log("compute skin: the build-ordering memory barrier was refused - the structure build would read the previous frame's vertices");
            return false;
        }
        return true;
    }

} // namespace deren::vulkan::pass
