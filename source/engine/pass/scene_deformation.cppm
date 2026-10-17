/** @file
 * @brief Shared frame-slot deformation storage, owned and retained by the passchain.
 * Setup data and partial updates survive reuse; runtime preserves contents on provider handover.
 */
module;
#include <array>
#include <glm/glm.hpp>
#include <string_view>
#include <vector>
export module deren.engine.pass.scene_deformation;
export import deren.utility.shared;
import deren.engine.pass;
import deren.engine.render_resource;
import deren.engine.primitive;
import deren.engine.render_layout;
import deren.promise.rhi;

export namespace deren::engine::pass {
    class scene_deformation_pass final : public frame_pass {
        deren::utility::vessel::array<bool, 4> required{true, true, true, true};
        render_resource::instances::resource_snapshot resource_view;

    public:
        static constexpr deren::utility::vessel::array ids{render_resource::resource_id::motion_vectors,
                                                           render_resource::resource_id::skin_matrices,
                                                           render_resource::resource_id::skin_matrices_previous,
                                                           render_resource::resource_id::morph_targets};
        static constexpr deren::utility::vessel::array<uint64_t, 4> sizes{uint64_t(scene_motion_capacity) * sizeof(glm::mat4),
                                                                          uint64_t(scene_skin_capacity) * sizeof(glm::mat4),
                                                                          uint64_t(scene_skin_capacity) * sizeof(glm::mat4),
                                                                          uint64_t(scene_morph_capacity) * sizeof(float)};
        static constexpr deren::utility::vessel::array heap_slots{render_layout::heap_slots::previous_transforms,
                                                                  render_layout::heap_slots::skin_matrices,
                                                                  render_layout::heap_slots::skin_matrices_previous,
                                                                  render_layout::heap_slots::morph_data};
        void select_resources(deren::utility::vessel::array<bool, 4> const& selected) noexcept {
            required = selected;
        }
        void describe_resources(render_resource::instances::resource_context const&,
                                deren::utility::vessel::vector<render_resource::instances::request>& requests) const override {
            namespace managed = render_resource::instances;
            namespace rhi = deren::promise::rhi;
            for (size_t i = 0; i < ids.size(); ++i) {
                if (!required[i])
                    continue;
                managed::request value;
                value.id = ids[i];
                value.type = managed::kind::buffer;
                value.scope = render_resource::resource_scope::per_frame_slot;
                value.life = managed::lifetime::frame_slot;
                value.buffer.size = sizes[i];
                value.buffer.usage = rhi::buffer_usage::storage_coherent;
                value.buffer.flags = rhi::to_bits(rhi::buffer_flag::device_address);
                value.zero_on_create = true;
                requests.push_back(value);
            }
        }
        void bind_resources(render_resource::instances::resource_snapshot const& snapshot) override {
            resource_view = snapshot;
        }
        render_resource::instances::resource_snapshot const& resources() const noexcept {
            return resource_view;
        }
        render_resource::pass_io const& io() const noexcept override {
            static render_resource::pass_io const value{.name = "scene_deformation"};
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
} // namespace deren::engine::pass
