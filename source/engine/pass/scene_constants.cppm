/** @file
 * @brief Shared per-slot camera, lighting and character constants, owned by the passchain.
 * @defgroup engine_pass_scene_constants Shared Scene Constants
 * This resource producer records no GPU commands. The host derives values, and the chain uploads
 * them only after slot completion. Every shading pass reads the existing shared heap descriptors.
 */
module;
#include <string_view>
#include <vector>
export module deren.engine.pass.scene_constants;
export import deren.utility.shared;
import deren.engine.pass;
import deren.engine.render_resource;
import deren.engine.primitive;
import deren.promise.rhi;

export namespace deren::engine::pass {
    class scene_constants_pass final : public frame_pass {
        bool light_required = true;
        bool head_required = true;
        bool camera_required = true;
        render_resource::instances::resource_snapshot resource_view;

    public:
        void select_resources(bool light, bool head, bool camera = true) noexcept {
            light_required = light;
            head_required = head;
            camera_required = camera;
        }
        void describe_resources(render_resource::instances::resource_context const&,
                                deren::utility::vessel::vector<render_resource::instances::request>& requests) const override {
            namespace managed = render_resource::instances;
            namespace rhi = deren::promise::rhi;
            managed::request value;
            value.id = render_resource::resource_id::light_ubo;
            value.type = managed::kind::buffer;
            value.scope = render_resource::resource_scope::per_frame_slot;
            value.life = managed::lifetime::frame_slot;
            value.buffer.size = sizeof(deren::engine::light_ubo);
            value.buffer.usage = rhi::buffer_usage::uniform_coherent;
            value.buffer.flags = rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::storage);
            value.zero_on_create = true;
            if (light_required)
                requests.push_back(value);
            value.id = render_resource::resource_id::head_ubo;
            value.buffer.size = sizeof(deren::engine::head_ubo);
            if (head_required)
                requests.push_back(value);
            value.id = render_resource::resource_id::camera_ubo;
            value.buffer.size = sizeof(deren::engine::camera_ubo);
            if (camera_required)
                requests.push_back(value);
        }
        void bind_resources(render_resource::instances::resource_snapshot const& snapshot) override {
            resource_view = snapshot;
        }
        render_resource::instances::resource_snapshot const& resources() const noexcept {
            return resource_view;
        }
        render_resource::pass_io const& io() const noexcept override {
            static render_resource::pass_io const value{.name = "scene_constants"};
            return value;
        }
        deren::engine::pass::behaviour const& behaviour() const noexcept override {
            static deren::engine::pass::behaviour const value{.extent = extent_rule::none, .resync_viewport = false};
            return value;
        }
        std::string_view feature() const noexcept override {
            return {};
        }
        void create(pass_context const&) override {
        }
        void on_swapchain_recreated(pass_host const&) override {
        }
        void record(resolved_io const&) override {
        }
    };
} // namespace deren::engine::pass
