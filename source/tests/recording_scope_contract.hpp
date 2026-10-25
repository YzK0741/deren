#pragma once

// Include after importing deren.promise.rhi and the CHECK harness.
namespace deren::vk_test {
    inline void check_recording_scope_contract(deren::promise::rhi::api_core& core) {
        namespace rhi = deren::promise::rhi;
        auto commands = core.commands->create_command_buffer({});
        CHECK(commands);
        if (!commands)
            return;
        CHECK(commands->begin_recording({}) == rhi::error::ok);
        rhi::rendering_info scope{};
        scope.area.width = scope.area.height = 4;
        CHECK(commands->begin_rendering(scope) == rhi::error::ok);
        CHECK(commands->begin_rendering(scope) == rhi::error::not_ready);
        commands->end_rendering(); // rejected nested begin did not remove the original scope.
        CHECK(commands->end_recording() == rhi::error::ok);
        CHECK(commands->begin_recording({}) == rhi::error::ok);
        commands->end_rendering();
        CHECK(commands->apply_operations() == rhi::error::invalid_argument);
        CHECK(commands->end_recording() == rhi::error::invalid_argument);
        CHECK(commands->begin_recording({}) == rhi::error::ok);
        CHECK(commands->end_recording() == rhi::error::ok);

        auto inherited = core.commands->make_mergeable_command_buffer({});
        CHECK(inherited);
        if (!inherited)
            return;
        rhi::command_buffer_inheritance_info inheritance{};
        CHECK(inherited->begin_recording({.usage = rhi::to_bits(rhi::command_buffer_usage::render_pass_continue), .next = &inheritance.header}) == rhi::error::ok);
        inherited->end_rendering(); // the inherited scope belongs to the primary.
        CHECK(inherited->apply_operations() == rhi::error::invalid_argument);
        CHECK(inherited->end_recording() == rhi::error::invalid_argument);
        CHECK(inherited->begin_recording({}) == rhi::error::ok);
        CHECK(inherited->end_recording() == rhi::error::ok);
    }
} // namespace deren::vk_test
