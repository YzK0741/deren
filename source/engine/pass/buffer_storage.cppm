/** @file @brief Resource-only pass for dynamic object requirements prepared by a completed job slot. */
module;
#include <span>
#include <string_view>
#include <vector>
export module deren.engine.pass.buffer_storage;
export import deren.utility.shared;
import deren.engine.pass;
import deren.engine.render_resource;
export namespace deren::engine::pass {
    class buffer_storage_pass final : public frame_pass {
        deren::utility::vessel::vector<render_resource::instances::request> needs;
        render_resource::instances::resource_snapshot resource_view;

    public:
        void configure(std::span<render_resource::instances::request const> requests) {
            needs.assign(requests.begin(), requests.end());
        }
        void describe_resources(render_resource::instances::resource_context const&, deren::utility::vessel::vector<render_resource::instances::request>& requests) const override {
            requests.insert(requests.end(), needs.begin(), needs.end());
        }
        void bind_resources(render_resource::instances::resource_snapshot const& snapshot) override {
            resource_view = snapshot;
        }
        render_resource::pass_io const& io() const noexcept override {
            static render_resource::pass_io const value{.name = "buffer_storage"};
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
    };
    using resource_storage_pass = buffer_storage_pass;
} // namespace deren::engine::pass
