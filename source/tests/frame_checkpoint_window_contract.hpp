#pragma once

inline void check_window_checkpoints() {
    CHECK(glfwInit() == GLFW_TRUE);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    auto* window = glfwCreateWindow(32, 24, "checkpoint probe", nullptr, nullptr);
    CHECK(window);
    if (!window) { glfwTerminate(); return; }
    {
        rhi::create_info options{};
        options.native_window = window;
        options.validation_layers = true;
        auto core = deren::engine::load_api_core(options);
        CHECK(core);
        if (core) {
            auto* checkpoint = rhi::query_extension<rhi::frame_checkpoint>(*core);
            auto* walker = core->frames->walk_frames();
            CHECK(checkpoint && walker);
            for (unsigned frame = 0; checkpoint && walker && frame < 6; ++frame) {
                if (frame == 3) {
                    glfwSetWindowSize(window, 48, 40);
                    glfwPollEvents();
                    CHECK(core->frames->frame_swapchain()->recreate() == rhi::error::ok);
                }
                auto opened = walker->wait_and_acquire();
                CHECK(opened.result.code == rhi::error::ok);
                if (opened.result.code != rhi::error::ok) break;
                auto* commands = core->frames->begin_commands();
                auto image = rhi::query_extension<rhi::frame_resources>(*core)->retain_image();
                auto view = image->make_view({.role = rhi::view_role::color_attachment});
                CHECK(view);
                CHECK(commands->begin_recording({}) == rhi::error::ok);
                CHECK(commands->barrier(rhi::image_barrier{.resource = image.get(), .from = rhi::image_use::undefined, .to = rhi::image_use::color_attachment}) == rhi::error::ok);
                rhi::color_attachment target{.view = view.get(), .load = rhi::load_op::clear};
                auto extent = core->frames->frame_swapchain()->extent();
                rhi::rendering_info render{};
                render.area = {0, 0, extent.width, extent.height};
                render.colors = std::span(&target, 1);
                CHECK(commands->begin_rendering(render) == rhi::error::ok);
                commands->end_rendering();
                CHECK(checkpoint->checkpoint_and_wait() == rhi::error::ok);
                CHECK(checkpoint->checkpoint_and_wait() == rhi::error::ok);
                CHECK(walker->position() == opened.frame.frame_index);
                CHECK(commands->barrier(rhi::image_barrier{.resource = image.get(), .from = rhi::image_use::color_attachment, .to = rhi::image_use::present}) == rhi::error::ok);
                CHECK(commands->end_recording() == rhi::error::ok);
                CHECK(core->frames->submit() == rhi::error::ok);
                CHECK(core->frames->present() == rhi::error::ok);
                walker->walk_to_next();
            }
            core->wait_idle(); // test teardown only
        }
    }
    glfwDestroyWindow(window);
    glfwTerminate();
}
