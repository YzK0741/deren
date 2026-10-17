/** @file @brief Passchain-owned shared scene tables and packed mesh command/culling storage. */
module;
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <string_view>
#include <vector>
export module deren.engine.pass.scene_tables;
export import deren.utility.shared;
import deren.engine.pass;
import deren.engine.render_resource;
import deren.engine.primitive;
import deren.engine.meshlet;
import deren.engine.render_layout;
import deren.promise.rhi;
export namespace deren::engine::pass {
    class scene_tables_pass final : public frame_pass {
        deren::utility::vessel::array<bool, 9> required{true, true, true, true, true, true, true, true, true};
        render_resource::instances::resource_snapshot resource_view;

    public:
        static constexpr size_t resource_count = 9;
        static constexpr deren::utility::vessel::array ids{render_resource::resource_id::material_table, render_resource::resource_id::instance_table,
                                                           render_resource::resource_id::toon_colour_table, render_resource::resource_id::toon_lane_table, render_resource::resource_id::toon_rig_table,
                                                           render_resource::resource_id::meshlet_table, render_resource::resource_id::mesh_indirect_commands,
                                                           render_resource::resource_id::meshlet_statistics, render_resource::resource_id::meshlet_culled_table};
        static constexpr deren::utility::vessel::array heap_slots{render_layout::heap_slots::materials, render_layout::heap_slots::instance_transforms,
                                                                  render_layout::heap_slots::toon_colours, render_layout::heap_slots::toon_lanes, render_layout::heap_slots::toon_rig,
                                                                  render_layout::heap_slots::meshlets, 0u, render_layout::heap_slots::meshlet_stats, render_layout::heap_slots::meshlet_culled};
        static deren::utility::vessel::array<render_resource::instances::request, resource_count> descriptions(render_resource::instances::resource_context const& context) {
            namespace managed = render_resource::instances;
            namespace rhi = deren::promise::rhi;
            deren::utility::vessel::array<uint64_t, resource_count> const sizes{
                uint64_t(material_capacity) * sizeof(material_record), uint64_t(instance_capacity) * sizeof(glm::mat4),
                uint64_t(material_capacity) * uint64_t(toon_colour_lane::count) * sizeof(glm::vec4),
                uint64_t(material_capacity) * toon_lane_blocks * sizeof(glm::uvec4), sizeof(toon_rig), uint64_t(meshlet_capacity) * sizeof(meshlet),
                uint64_t(context.frame_slots) * 2u * meshlet_capacity * rhi::mesh_task_command_size,
                uint64_t(context.frame_slots) * 8u * sizeof(uint32_t), uint64_t(context.frame_slots) * meshlet_capacity * sizeof(meshlet)};
            deren::utility::vessel::array<managed::request, resource_count> values;
            for (size_t i = 0; i < values.size(); ++i) {
                auto& value = values[i];
                value.id = ids[i];
                value.type = managed::kind::buffer;
                value.scope = render_resource::resource_scope::device_wide;
                value.life = i < 6 ? managed::lifetime::scene : managed::lifetime::frame_slot;
                value.buffer.size = sizes[i];
                value.buffer.usage = rhi::buffer_usage::storage_coherent;
                value.buffer.flags = i == 6 ? rhi::to_bits(rhi::buffer_flag::indirect) : rhi::to_bits(rhi::buffer_flag::device_address);
                value.zero_on_create = true;
            }
            return values;
        }
        void select_resources(deren::utility::vessel::array<bool, resource_count> const& selected) noexcept {
            required = selected;
        }
        void describe_resources(render_resource::instances::resource_context const& context, deren::utility::vessel::vector<render_resource::instances::request>& requests) const override {
            auto const values = descriptions(context);
            for (size_t i = 0; i < values.size(); ++i)
                if (required[i])
                    requests.push_back(values[i]);
        }
        void bind_resources(render_resource::instances::resource_snapshot const& snapshot) override {
            resource_view = snapshot;
        }
        render_resource::instances::resource_snapshot const& resources() const noexcept {
            return resource_view;
        }
        render_resource::pass_io const& io() const noexcept override {
            static render_resource::pass_io const value{.name = "scene_tables"};
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
