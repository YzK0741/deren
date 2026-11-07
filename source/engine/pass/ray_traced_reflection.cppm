// module version: 1.0.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file source/engine/pass/ray_traced_reflection.cppm
 * @brief The ray-traced reflection pass: one reflection ray per pixel into a per-frame-slot target.
 *
 * The pass follows the ray-traced shadow's shape: it owns its ray-tracing pipeline (raygen, miss and one
 * triangles hit group), its three shader binding table regions, the push block and the full-resolution
 * `traceRays` launch. It owns no descriptor set: the camera, light UBO, TLAS, G-buffer images and the
 * reflection target are heap slots its shaders name themselves.
 *
 * The target is declared by `output_targets_pass` (index 17) and sampled by the deferred lighting stage.
 */

module;

#include <cstdint>
#include <cstring>
#include <glm/glm.hpp>

export module deren.engine.pass.ray_traced_reflection;
export import deren.vstd;
export import deren.utility.shared;
import deren.promise.rhi;

import deren.engine.pass;
import deren.engine.render_resource;
import deren.engine.pipelines;

export namespace deren::engine::pass {

    class rt_reflection_pass final : public frame_pass {
    public:
        /// @brief the push block shared with `source/shaders/lighting/rt_reflection.slang`
        struct push_constants {
            glm::mat4 inv_view_proj = glm::mat4(1.0f);
            // x = ray tmin, y = absolute normal-offset floor, z = relative offset scale, w = unused
            glm::vec4 params = glm::vec4(0.01f, 0.002f, 0.0015f, 0.0f);
        };

        rt_reflection_pass() = default;
        ~rt_reflection_pass() override;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override;
        [[nodiscard]] deren::engine::pass::behaviour const& behaviour() const noexcept override;
        [[nodiscard]] std::string_view feature() const noexcept override;
        void create(pass_context const& context) override;
        void describe_resources(render_resource::instances::resource_context const&, deren::utility::vessel::vector<render_resource::instances::request>&) const override;
        void bind_resources(render_resource::instances::resource_snapshot const&) override;
        void on_swapchain_recreated(pass_host const& host) override;

        deren::promise::rhi::error record_content(resolved_io const& io) noexcept override;

        [[nodiscard]] bool pipeline_ready() const noexcept;
        [[nodiscard]] bool ready() const noexcept override {
            return this->pipeline_ready();
        }
        [[nodiscard]] deren::promise::rhi::pipeline* pipeline_handle() const noexcept override;

    private:
        [[nodiscard]] bool record_ready(resolved_io const& io) const noexcept;
        render_resource::instances::resource_snapshot resource_view;
        static constexpr std::string_view raygen_name = "rt_reflection.rgen.spv";
        static constexpr std::string_view closest_hit_name = "rt_reflection.rchit.spv";
        static constexpr std::string_view miss_name = "rt_reflection.rmiss.spv";
        static constexpr uint32_t group_size = 8;
        static constexpr uint32_t barrier_reflection = 0;
        static_assert(barrier_reflection + 1 == render_resource::rt_reflection_barriers.size(),
                      "the reflection pass's barrier slots must match the declaration it indexes");

        static constexpr deren::utility::vessel::array<std::string_view, 1> pipeline_names = {"rt_reflection"};
        inline static constexpr deren::engine::pass::behaviour pass_behaviour = {
            .kind = behaviour_kind::ray_tracing,
            .group_size_x = group_size,
            .group_size_y = group_size,
            .group_size_z = 1,
            .extent = extent_rule::full,
            .extent_of = resource_id::none,
            .pipelines = pipeline_names,
            .resync_viewport = false,
        };
        void release_owned() noexcept;
        deren::promise::rhi::api_core* built_against = nullptr;
        std::optional<pipelines::pipeline_handle> pass_pipeline = std::nullopt;
        deren::promise::rhi::shader_binding_table_region raygen_region = {};
        deren::promise::rhi::shader_binding_table_region miss_region = {};
        deren::promise::rhi::shader_binding_table_region hit_region = {};
        deren::promise::rhi::shader_binding_table_region callable_region = {};
        bool logged = false;
    };

    static_assert(sizeof(rt_reflection_pass::push_constants) == render_resource::rt_reflection_io.push->size,
                  "the reflection pass's declared push block must be the size of the struct the renderer composes");

} // namespace deren::engine::pass
