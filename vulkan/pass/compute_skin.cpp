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
        auto built = pipelines::build_compute_skin(*context.face, context.device, spirv);
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
        // THE BIND IS RAW ON PURPOSE (see the header): this job's compute pipeline is not a contract pipeline
        // yet, so the one bind goes through the contract's own escape. A face that answers no native command
        // buffer records nothing rather than mis-casting a foreign pointer.
        VkCommandBuffer const native = pass::native_commands(this->built_against, commands);
        if (native == VK_NULL_HANDLE) {
            return false;
        }
        vkCmdBindPipeline(native, VK_PIPELINE_BIND_POINT_COMPUTE, this->pipeline());
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

        // What follows reads what these dispatches wrote: the BUILD on the frame the structures are created, and
        // the REFIT on every frame after. A compute write is not visible to the acceleration structure build
        // stage without this barrier, and the symptom would be a structure built or refitted against the
        // previous frame's vertices - a shadow one frame behind, which reads as animation lag.
        // THE BUILD-ORDERING BARRIER STAYS RAW, and it is the record series' one measured gap here: this is a
        // GLOBAL memory barrier (COMPUTE_SHADER/SHADER_WRITE -> ACCELERATION_STRUCTURE_BUILD/SHADER_READ) and
        // `barrier_group` carries IMAGES and BUFFERS, not a memory barrier with no operand. The escape is the
        // contract's own answer for a step it has no concept for; a face without one records nothing (the
        // dispatches above are already recorded, which is the same ordering the old code left).
        VkMemoryBarrier2 skin_order = {};
        skin_order.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        skin_order.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        skin_order.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        skin_order.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        skin_order.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        VkDependencyInfo const skin_dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                  .pNext = nullptr,
                                                  .dependencyFlags = 0,
                                                  .memoryBarrierCount = 1,
                                                  .pMemoryBarriers = &skin_order,
                                                  .bufferMemoryBarrierCount = 0,
                                                  .pBufferMemoryBarriers = nullptr,
                                                  .imageMemoryBarrierCount = 0,
                                                  .pImageMemoryBarriers = nullptr};
        vkCmdPipelineBarrier2(native, &skin_dependency);
        return true;
    }

} // namespace deren::vulkan::pass
