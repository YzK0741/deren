#pragma once
#include <array>
#include <cstddef>
#include <span>

// Include after importing deren.promise.rhi and the CHECK harness.
namespace deren::vk_test {
    inline void check_buffer_creation_contract(deren::promise::rhi::api_core& core) {
        namespace rhi = deren::promise::rhi;
        CHECK(!core.resources->create_buffer({}));
        std::array<std::byte, 20> bytes{};
        bytes.fill(std::byte{0x53});
        for (auto usage : {rhi::buffer_usage::vertex, rhi::buffer_usage::index, rhi::buffer_usage::uniform_gpu_only}) {
            for (std::size_t length : {0u, 8u, 16u, 20u}) {
                auto buffer = core.resources->create_buffer({.size = 16, .usage = usage, .initial_bytes = std::span(bytes).first(length)});
                CHECK(static_cast<bool>(buffer) == (length == 0 || length == 16));
                if (buffer)
                    CHECK(buffer->size() == 16);
            }
        }
        auto mapped = core.resources->create_buffer({.size = 16, .usage = rhi::buffer_usage::storage_coherent, .initial_bytes = std::span(bytes).first(16)});
        CHECK(mapped);
        if (mapped) {
            auto content = mapped->mapped();
            CHECK(content.size() == 16);
            if (content.size() == 16)
                for (auto byte : content)
                    CHECK(byte == std::byte{0x53});
        }
    }
} // namespace deren::vk_test
