#pragma once

// Internal RHI recording helpers. Include after importing deren.promise.rhi.
namespace deren::engine::feedback_readback {
    inline promise::rhi::error clear(promise::rhi::command_recorder& commands, promise::rhi::buffer& gpu,
                                     promise::rhi::buffer const& zero, std::uint64_t bytes) {
        namespace rhi = promise::rhi;
        // The slot's prior GPU work completed before reuse. Its last use was the readback copy.
        rhi::buffer_barrier ready{.resource = &gpu, .from = rhi::buffer_use::transfer_source, .to = rhi::buffer_use::transfer_destination};
        auto status = commands.barrier({.buffers = std::span(&ready, 1)});
        if (status != rhi::error::ok)
            return status;
        status = commands.copy_buffer(gpu, zero, bytes, 0, 0);
        if (status != rhi::error::ok)
            return status;
        rhi::buffer_barrier shader{.resource = &gpu, .from = rhi::buffer_use::transfer_destination, .to = rhi::buffer_use::shader_read_write};
        return commands.barrier({.buffers = std::span(&shader, 1)});
    }
    inline promise::rhi::error publish(promise::rhi::command_recorder& commands, promise::rhi::buffer& host,
                                       promise::rhi::buffer& gpu, std::uint64_t bytes) {
        namespace rhi = promise::rhi;
        std::array<rhi::buffer_barrier, 2> copy{{{.resource = &gpu, .from = rhi::buffer_use::shader_write, .to = rhi::buffer_use::transfer_source},
                                                 {.resource = &host, .from = rhi::buffer_use::host_read, .to = rhi::buffer_use::transfer_destination}}};
        auto status = commands.barrier({.buffers = copy});
        if (status != rhi::error::ok)
            return status;
        status = commands.copy_buffer(host, gpu, bytes, 0, 0);
        if (status != rhi::error::ok)
            return status;
        rhi::buffer_barrier readable{.resource = &host, .from = rhi::buffer_use::transfer_destination, .to = rhi::buffer_use::host_read};
        return commands.barrier({.buffers = std::span(&readable, 1)});
    }
} // namespace deren::engine::feedback_readback
