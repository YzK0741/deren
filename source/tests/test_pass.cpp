// Headless unit tests: deren.engine.pass (the framework, no device) =================
// The framework's whole value is a CONTRACT: create once per generation, resolve per frame, skip an inactive
// pass without resolving it, apply the behaviour before recording, mark once per stage, and tell every pass
// when the swapchain was rebuilt. Each of those is something this renderer does by hand today, and does
// inconsistently - the manual reset list missed per-image descriptor families once, and the viewport resync is a
// hand-kept pipeline list. So this test asserts the contract with a FAKE host: no device, no Vulkan call, and
// every assertion is about what the runner DID, in what order, and what it deliberately did not do.
#include "vk_test.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp> // the frame constants carry glm types (see deren.engine.frame_constants)
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <vulkan/vulkan.h>

import deren.utility.shared;
import deren.promise.rhi;
import deren.engine.pass;
import deren.engine.pass.chain;
import deren.engine.pass.scene;
import deren.engine.primitive;
import deren.engine.render_resource;
import deren.engine.render_resource.shared;

namespace {
    namespace vp = deren::engine::pass;
    namespace rr = deren::engine::render_resource;

    /// The own binding every fake pass carries: one sampled image of its OWN at binding 0. What a pass reaches
    /// through the frame's heap is not a declaration the fake host has to spell out - the framework has no set to
    /// resolve it into (every stage reads the descriptor heap).
    deren::utility::vessel::array<rr::pass_binding, 1> const fake_own_bindings = {{{.binding = 0,
                                                                                    .owner = rr::binding_owner::own,
                                                                                    .kind = rr::binding_kind::sampled_image,
                                                                                    .resource = rr::resource_id::hdr,
                                                                                    .access = rr::binding_access::read,
                                                                                    .sampler = rr::sampler_hint::gbuffer}}};

    /// the declaration every fake pass carries, renamed to the pass it belongs to
    rr::pass_io named_io(std::string_view const name, std::span<rr::render_target const> const targets = {}) {
        return rr::pass_io{
            .name = name,
            .bindings = fake_own_bindings,
            .targets = targets,
            .push = std::nullopt,
        };
    }

    /// THE COMMAND BUFFER'S STAND-IN (the merged recording face): `resolved_io::cmd` is a
    /// `rhi::shared_object<rhi::command_buffer>` now, and the one thing this test asserts about it is
    /// IDENTITY - the host hands the frame's buffer down and the pass reads back what it was given -
    /// so the stand-in exists to BE that object and records nothing. It is not a device, and every
    /// verb that would need one is a no-op; the two timing verbs answer `unsupported`, the honest
    /// answer for an object that cannot timestamp (the probe backend's own rule).
    struct contract_command_buffer final : deren::promise::rhi::command_buffer {
        std::uint32_t rendering_begins = 0;
        std::uint32_t rendering_ends = 0;
        std::uint32_t secondary_executions = 0;
        deren::promise::rhi::error rendering_result = deren::promise::rhi::error::ok;
        deren::promise::rhi::error begin_result = deren::promise::rhi::error::ok;
        deren::promise::rhi::error end_result = deren::promise::rhi::error::ok;
        deren::promise::rhi::error execute_result = deren::promise::rhi::error::ok;
        deren::promise::rhi::rect rendering_area = {};
        deren::utility::vessel::vector<deren::promise::rhi::color_attachment> rendering_colors;
        deren::promise::rhi::depth_attachment rendering_depth = {};
        bool rendering_has_depth = false;
        bool rendering_secondary_contents = true;

        contract_command_buffer() noexcept
            : deren::promise::rhi::command_buffer("contract_command_buffer") {
        }
        void release() noexcept override {
        }

        [[nodiscard]] deren::promise::rhi::error begin_recording(deren::promise::rhi::command_buffer_begin_info const&) override {
            return begin_result;
        }

        [[nodiscard]] deren::promise::rhi::error end_recording() noexcept override {
            return end_result;
        }

        [[nodiscard]] deren::promise::rhi::error execute(std::span<deren::promise::rhi::mergeable_command_buffer_ref const>) override {
            ++this->secondary_executions;
            return execute_result;
        }

        [[nodiscard]] deren::promise::rhi::error use(deren::promise::rhi::image const&, deren::promise::rhi::image_use, deren::promise::rhi::image_use) noexcept override {
            return deren::promise::rhi::error::ok;
        }

        [[nodiscard]] deren::promise::rhi::error copy_image_to_buffer(deren::promise::rhi::buffer&, deren::promise::rhi::image const&,
                                                                      deren::promise::rhi::image_copy_region const&) noexcept override {
            return deren::promise::rhi::error::unsupported;
        }

        /// THE TIER-1 ACCELERATION-STRUCTURE VERBS (abi 26): this fake records nothing, so it refuses them by
        /// name like the copy above - and no handle can reach them while its `create_acceleration_structure`
        /// answers null.
        [[nodiscard]] deren::promise::rhi::error build_acceleration_structure(deren::promise::rhi::acceleration_structure&) override {
            return deren::promise::rhi::error::unsupported;
        }
        [[nodiscard]] deren::promise::rhi::error refit_acceleration_structure(deren::promise::rhi::acceleration_structure&) override {
            return deren::promise::rhi::error::unsupported;
        }
        [[nodiscard]] deren::promise::rhi::error build_micromap(deren::promise::rhi::micromap&) override {
            return deren::promise::rhi::error::unsupported;
        }

        [[nodiscard]] deren::promise::rhi::error begin_gpu_timing() noexcept override {
            return deren::promise::rhi::error::unsupported;
        }

        [[nodiscard]] deren::promise::rhi::error mark_gpu_timing(std::uint32_t, std::string_view) noexcept override {
            return deren::promise::rhi::error::unsupported;
        }

        [[nodiscard]] deren::promise::rhi::error begin_rendering(deren::promise::rhi::rendering_info const& info) override {
            ++this->rendering_begins;
            this->rendering_area = info.area;
            this->rendering_colors.assign(info.colors.begin(), info.colors.end());
            this->rendering_depth = info.depth;
            this->rendering_has_depth = info.has_depth;
            this->rendering_secondary_contents = info.secondary_contents;
            return this->rendering_result;
        }

        void end_rendering() noexcept override {
            ++this->rendering_ends;
        }

        [[nodiscard]] deren::promise::rhi::error bind_pipeline(deren::promise::rhi::pipeline const&) override {
            return deren::promise::rhi::error::ok;
        }

        [[nodiscard]] deren::promise::rhi::error bind_vertex_buffer(deren::promise::rhi::buffer const&, std::uint64_t) override {
            return deren::promise::rhi::error::ok;
        }

        [[nodiscard]] deren::promise::rhi::error bind_index_buffer(deren::promise::rhi::buffer const&, std::uint64_t, deren::promise::rhi::index_type) override {
            return deren::promise::rhi::error::ok;
        }

        void draw(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) noexcept override {
        }

        void draw_indexed(std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t, std::uint32_t) noexcept override {
        }

        void dispatch(std::uint32_t, std::uint32_t, std::uint32_t) noexcept override {
        }

        void draw_mesh_tasks(std::uint32_t, std::uint32_t, std::uint32_t) noexcept override {
        }

        [[nodiscard]] deren::promise::rhi::error draw_indirect(deren::promise::rhi::buffer const&, std::uint64_t, std::uint32_t, std::uint32_t) override {
            return deren::promise::rhi::error::unsupported;
        }
        [[nodiscard]] deren::promise::rhi::error draw_mesh_tasks_indirect(deren::promise::rhi::buffer const&, std::uint64_t, std::uint32_t, std::uint32_t) override {
            return deren::promise::rhi::error::ok;
        }

        // abi 24: the recording face's ray-tracing LAUNCH. A fake backend records nothing; the real one calls the
        // entry point it resolved at startup.
        void trace_rays(deren::promise::rhi::shader_binding_table_region const&, deren::promise::rhi::shader_binding_table_region const&,
                        deren::promise::rhi::shader_binding_table_region const&, deren::promise::rhi::shader_binding_table_region const&, std::uint32_t, std::uint32_t,
                        std::uint32_t) noexcept override {
        }

        void set_viewport(deren::promise::rhi::viewport const&) noexcept override {
        }

        void set_scissor(deren::promise::rhi::rect const&) noexcept override {
        }

        void set_cull_mode(deren::promise::rhi::cull_mode) noexcept override {
        }

        void set_depth_write(bool) noexcept override {
        }

        void set_depth_bias(float, float, float) noexcept override {
        }

        [[nodiscard]] deren::promise::rhi::error barrier(deren::promise::rhi::barrier_group const&) override {
            return deren::promise::rhi::error::ok;
        }

        [[nodiscard]] deren::promise::rhi::error barrier(deren::promise::rhi::image_barrier const&) override {
            return deren::promise::rhi::error::ok;
        }

        [[nodiscard]] deren::promise::rhi::error copy_image(deren::promise::rhi::image_copy const&) override {
            return deren::promise::rhi::error::ok;
        }

        [[nodiscard]] deren::promise::rhi::error copy_buffer(deren::promise::rhi::buffer&, deren::promise::rhi::buffer const&, std::uint64_t, std::uint64_t, std::uint64_t) override {
            return deren::promise::rhi::error::ok;
        }

        [[nodiscard]] deren::promise::rhi::error clear_color_image(deren::promise::rhi::image const&, deren::utility::vessel::array<float, 4> const&, deren::promise::rhi::subresource_range const&) override {
            return deren::promise::rhi::error::ok;
        }
    };

    /// abi 36: the multithreaded passes record into `mergeable_command_buffer` segments now, so the scene-pass
    /// lifecycle test needs one. Only the two lifecycle verbs carry state (the injected failure results); every
    /// recording verb is the no-op this fake's whole stance already is.
    struct contract_mergeable final : deren::promise::rhi::mergeable_command_buffer {
        deren::promise::rhi::error begin_result = deren::promise::rhi::error::ok;
        deren::promise::rhi::error end_result = deren::promise::rhi::error::ok;
        contract_mergeable() noexcept
            : deren::promise::rhi::mergeable_command_buffer("contract_mergeable") {
        }
        void release() noexcept override {
        }
        [[nodiscard]] deren::promise::rhi::error begin_recording(deren::promise::rhi::command_buffer_begin_info const&) override {
            return begin_result;
        }
        [[nodiscard]] deren::promise::rhi::error end_recording() noexcept override {
            return end_result;
        }
        [[nodiscard]] deren::promise::rhi::error use(deren::promise::rhi::image const&, deren::promise::rhi::image_use, deren::promise::rhi::image_use) noexcept override {
            return deren::promise::rhi::error::ok;
        }
        [[nodiscard]] deren::promise::rhi::error copy_image_to_buffer(deren::promise::rhi::buffer&, deren::promise::rhi::image const&,
                                                                      deren::promise::rhi::image_copy_region const&) noexcept override {
            return deren::promise::rhi::error::unsupported;
        }
        [[nodiscard]] deren::promise::rhi::error begin_gpu_timing() noexcept override {
            return deren::promise::rhi::error::unsupported;
        }
        [[nodiscard]] deren::promise::rhi::error mark_gpu_timing(std::uint32_t, std::string_view) noexcept override {
            return deren::promise::rhi::error::unsupported;
        }
        [[nodiscard]] deren::promise::rhi::error begin_rendering(deren::promise::rhi::rendering_info const&) override {
            return deren::promise::rhi::error::ok;
        }
        void end_rendering() noexcept override {
        }
        [[nodiscard]] deren::promise::rhi::error bind_pipeline(deren::promise::rhi::pipeline const&) override {
            return deren::promise::rhi::error::ok;
        }
        [[nodiscard]] deren::promise::rhi::error bind_vertex_buffer(deren::promise::rhi::buffer const&, std::uint64_t) override {
            return deren::promise::rhi::error::ok;
        }
        [[nodiscard]] deren::promise::rhi::error bind_index_buffer(deren::promise::rhi::buffer const&, std::uint64_t, deren::promise::rhi::index_type) override {
            return deren::promise::rhi::error::ok;
        }
        void draw(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) noexcept override {
        }
        void draw_indexed(std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t, std::uint32_t) noexcept override {
        }
        [[nodiscard]] deren::promise::rhi::error draw_indirect(deren::promise::rhi::buffer const&, std::uint64_t, std::uint32_t, std::uint32_t) override {
            return deren::promise::rhi::error::ok;
        }
        void dispatch(std::uint32_t, std::uint32_t, std::uint32_t) noexcept override {
        }
        void draw_mesh_tasks(std::uint32_t, std::uint32_t, std::uint32_t) noexcept override {
        }
        [[nodiscard]] deren::promise::rhi::error draw_mesh_tasks_indirect(deren::promise::rhi::buffer const&, std::uint64_t, std::uint32_t, std::uint32_t) override {
            return deren::promise::rhi::error::ok;
        }
        void trace_rays(deren::promise::rhi::shader_binding_table_region const&, deren::promise::rhi::shader_binding_table_region const&,
                        deren::promise::rhi::shader_binding_table_region const&, deren::promise::rhi::shader_binding_table_region const&,
                        std::uint32_t, std::uint32_t, std::uint32_t) noexcept override {
        }
        [[nodiscard]] deren::promise::rhi::error build_acceleration_structure(deren::promise::rhi::acceleration_structure&) override {
            return deren::promise::rhi::error::unsupported;
        }
        [[nodiscard]] deren::promise::rhi::error refit_acceleration_structure(deren::promise::rhi::acceleration_structure&) override {
            return deren::promise::rhi::error::unsupported;
        }
        [[nodiscard]] deren::promise::rhi::error build_micromap(deren::promise::rhi::micromap&) override {
            return deren::promise::rhi::error::unsupported;
        }
        void set_viewport(deren::promise::rhi::viewport const&) noexcept override {
        }
        void set_scissor(deren::promise::rhi::rect const&) noexcept override {
        }
        void set_cull_mode(deren::promise::rhi::cull_mode) noexcept override {
        }
        void set_depth_write(bool) noexcept override {
        }
        void set_depth_bias(float, float, float) noexcept override {
        }
        [[nodiscard]] deren::promise::rhi::error barrier(deren::promise::rhi::barrier_group const&) override {
            return deren::promise::rhi::error::ok;
        }
        [[nodiscard]] deren::promise::rhi::error barrier(deren::promise::rhi::image_barrier const&) override {
            return deren::promise::rhi::error::ok;
        }
        [[nodiscard]] deren::promise::rhi::error copy_image(deren::promise::rhi::image_copy const&) override {
            return deren::promise::rhi::error::ok;
        }
        [[nodiscard]] deren::promise::rhi::error copy_buffer(deren::promise::rhi::buffer&, deren::promise::rhi::buffer const&, std::uint64_t, std::uint64_t, std::uint64_t) override {
            return deren::promise::rhi::error::ok;
        }
        [[nodiscard]] deren::promise::rhi::error clear_color_image(deren::promise::rhi::image const&, deren::utility::vessel::array<float, 4> const&, deren::promise::rhi::subresource_range const&) override {
            return deren::promise::rhi::error::ok;
        }
        [[nodiscard]] deren::promise::rhi::error load_texture_group(deren::promise::rhi::texture_group_ref const&) override {
            return deren::promise::rhi::error::unsupported;
        }
    };

    /// fake handles, so a resolved pass can be told apart from an unresolved one without a device
    /// (not `constexpr`: a handle comes from `reinterpret_cast`, which is not a constant expression)
    deren::promise::rhi::shared_object<deren::promise::rhi::command_buffer> const fake_cmd = std::make_shared<contract_command_buffer>();
    class contract_sampler final : public deren::promise::rhi::sampler {
    public:
        contract_sampler() noexcept
            : deren::promise::rhi::sampler("contract_sampler") {
        }
        void release() noexcept override {
        }
    };
    contract_sampler shadow_sampler_object;
    contract_sampler gbuffer_sampler_object;
    auto* const fake_shadow_sampler = &shadow_sampler_object;
    static_assert(std::is_same_v<decltype(rr::shared::sampler_set{}.of(rr::sampler_hint::shadow)),
                                 deren::promise::rhi::sampler*>);

    /// the host's own state: the log every callback writes, plus what the fake host answers
    struct host_state {
        deren::utility::vessel::vector<deren::utility::vessel::string> log;
        vp::frame_identity frame = {.image_index = 3, .slot = 1, .image_count = 3, .extent = {640, 480}};
        deren::utility::vessel::vector<std::string_view> active_features = {"gi"};
        std::string_view failing_pass = {}; // resolve returns false for this pass's name
        deren::utility::vessel::array<vp::resolved_binding, 9> own = {};
        /// what a pass was handed at create time, recorded so the create interface can be asserted
        /// (THE DEVICE IS GONE FROM THE CONTEXT, abi 21: the builders are contract factories that take the face)
        deren::promise::rhi::sampler* created_with_sampler = nullptr;
        /// WHAT THE RESOLVER CAN READ OFF THE DECLARATION, recorded here because the extent rule is applied by
        /// the HOST (see host_resolve): a rule that names a resource and one of its elements is only usable if
        /// both halves of the pair reach the host that has to map them to a size.
        rr::resource_id last_extent_of = rr::resource_id::none;
        uint16_t last_extent_element = 0;
    };

    vp::pass_host make_host(host_state& state) {
        return vp::pass_host{.context = &state};
    }

    /// the create-time context: what a pass builds itself from, and the point of the split is that ANY owner can
    /// fill it - this fake one below needs no device, no runtime and no frame, which is exactly what the test
    /// asserts a pass may rely on
    std::span<uint8_t const> fake_shader(void* /*owner*/, std::string_view const name) {
        static deren::utility::vessel::array<uint8_t, 3> const bytes = {0x03, 0x02, 0x23};
        return name == "fake.comp.spv" ? std::span<uint8_t const>(bytes) : std::span<uint8_t const>{};
    }

    vp::pass_context make_context() {
        return vp::pass_context{
            .samplers = {.shadow = fake_shadow_sampler},
            .shader = fake_shader,
            // the two session-stable formats in the CONTRACT's spelling (abi 20's face): the pass layer no
            // longer names a VkFormat, and these are the values the runtime hands over for the two it fills
            .swap_chain_image_format = deren::promise::rhi::image_format::bgra8_srgb,
            .depth_format = deren::promise::rhi::image_format::depth,
            .owner = nullptr,
        };
    }

    /// position of an entry in the log, or a large number when it is absent (so ordering asserts read clearly)
    std::size_t at(deren::utility::vessel::vector<deren::utility::vessel::string> const& log, std::string_view const entry) {
        auto const it = std::find(log.begin(), log.end(), entry);
        return it == log.end() ? std::size_t{9999} : static_cast<std::size_t>(it - log.begin());
    }

    bool has(deren::utility::vessel::vector<deren::utility::vessel::string> const& log, std::string_view const entry) {
        return at(log, entry) != 9999;
    }

    /// a declaration the schema must refuse: a binding whose resource was never set
    rr::pass_io const& bad_io() {
        static constexpr deren::utility::vessel::array<rr::pass_binding, 1> bindings = {{
            {.binding = 0, .owner = rr::binding_owner::own, .kind = rr::binding_kind::sampled_image, .resource = rr::resource_id::none, .sampler = rr::sampler_hint::post},
        }};
        static constexpr rr::pass_io io = {.name = "bad", .bindings = bindings, .push = std::nullopt};
        return io;
    }

    /// a pass that records what the runner did to it, in the order it did it
    class fake_pass final : public vp::frame_pass {
    public:
        fake_pass(rr::pass_io io, vp::behaviour const behaviour, std::string_view const feature, host_state& state)
            : io_decl(io)
            , behaviour_decl(behaviour)
            , feature_name(feature)
            , state_ptr(&state) {
        }

        [[nodiscard]] rr::pass_io const& io() const noexcept override {
            return io_decl;
        }
        [[nodiscard]] vp::behaviour const& behaviour() const noexcept override {
            return behaviour_decl;
        }
        [[nodiscard]] std::string_view feature() const noexcept override {
            return feature_name;
        }
        /// the framework's generic readiness question: a fake that says it did not build anything (see the
        /// `chain.ready(name)` checks) - the DEFAULT (a pass that builds nothing of its own) is the interface's `true`
        [[nodiscard]] bool ready() const noexcept override {
            return this->is_ready;
        }
        bool is_ready = true;
        void create(vp::pass_context const& context) override {
            state_ptr->log.emplace_back(deren::utility::vessel::string("create:") + deren::utility::vessel::string(io_decl.name));
            // WHAT THE CONTEXT CARRIES NOW (abi 21): the contract face and the two session-stable formats - and
            // no device at all, because every pipeline builder takes the face alone.
            state_ptr->created_with_sampler = context.samplers.of(rr::sampler_hint::shadow);
        }
        void on_swapchain_recreated(vp::pass_host const&) override {
            state_ptr->log.emplace_back(deren::utility::vessel::string("recreate:") + deren::utility::vessel::string(io_decl.name));
        }

        deren::promise::rhi::shared_object<deren::promise::rhi::command_buffer> last_cmd = {};
        std::size_t last_pipelines = 0;
        std::size_t last_push_size = 0;
        deren::promise::rhi::image_extent last_extent = {};
        uint32_t last_image_index = 0;
        uint32_t last_slot = 0;
        uint32_t last_image_count = 0;
        std::size_t last_targets = 0;
        deren::promise::rhi::image_view* last_target_view = nullptr;
        deren::promise::rhi::image* last_target_image = nullptr;
        deren::promise::rhi::image_view* last_per_image_first = nullptr;
        std::size_t last_per_image_length = 0;

    private:
        // the members below cannot take the interface's plain names (`io()`, `behaviour()`, `feature()` are the
        // accessors, and the constructor's parameters already use them), so each carries a distinguishing suffix
        rr::pass_io io_decl;
        vp::behaviour behaviour_decl;
        std::string_view feature_name;
        host_state* state_ptr;
    };

    // the names are `deren.engine.runtime`'s own pipeline keys, which is what makes this cost nothing new
    constexpr deren::utility::vessel::array<std::string_view, 1> compute_pipeline_names = {"compute_skin"};
    constexpr deren::utility::vessel::array<std::string_view, 2> fullscreen_pipeline_names = {"post_composite", "fxaa"};
    /// the fullscreen fake pass also declares one render TARGET: an attachment is a use that cannot be a
    /// descriptor, so it is declared in its own list (see vulkan.render_resource::render_target)
    constexpr rr::render_target fullscreen_target = {.resource = rr::resource_id::hdr, .element = 0};
    constexpr deren::utility::vessel::array<rr::render_target, 1> fullscreen_targets = {fullscreen_target};
    constexpr vp::behaviour compute_behaviour = {.kind = vp::behaviour_kind::compute, .group_size_x = 4, .group_size_y = 4, .group_size_z = 4, .extent = vp::extent_rule::resource, .pipelines = compute_pipeline_names};
    constexpr vp::behaviour fullscreen_behaviour = {.kind = vp::behaviour_kind::fullscreen, .extent = vp::extent_rule::half, .pipelines = fullscreen_pipeline_names, .resync_viewport = true};
} // namespace

template <typename Chain>
void check_managed_resource_api() {
    if constexpr (requires(Chain& chain) { chain.resources(); }) {
        CHECK(true);
    } else {
        CHECK_MSG(false, "passchain must own the resource registry");
    }
}

template <typename Binding>
void check_structure_publication() {
    namespace vp = deren::engine::pass;
    namespace rr = deren::engine::render_resource;
    if constexpr (requires(Binding binding) { binding.structure_handle; }) {
        vp::resource_table table;
        auto* structure = reinterpret_cast<deren::promise::rhi::acceleration_structure*>(0x4400);
        Binding binding;
        binding.structure_handle = structure;
        table.publish(rr::resource_id::top_level_structure, 0, 1, binding);
        vp::frame_identity frame{.image_index = 0, .slot = 1};
        auto const slot = vp::instance_for(rr::find(rr::resource_id::top_level_structure)->scope, frame);
        Binding resolved = table.find(rr::resource_id::top_level_structure, 0, slot);
        CHECK(resolved.structure_handle == structure);
        Binding missing = table.find(rr::resource_id::top_level_structure, 0, 0);
        CHECK(missing.structure_handle == nullptr);
        table.publish(rr::resource_id::top_level_structure, 0, 1, {});
        Binding cleared = table.find(rr::resource_id::top_level_structure, 0, 1);
        CHECK(cleared.structure_handle == nullptr);
    } else {
        CHECK_MSG(false, "The frame resource table must publish typed AS handles");
    }
}

int32_t main() {
    check_structure_publication<deren::engine::pass::resolved_binding>();
    check_managed_resource_api<deren::engine::pass::pass_chain>();
    using namespace deren::engine::pass;

    // Culling every mesh must still replace last frame's surface, motion, emissive and depth.
    // Lighting and splats consume those targets even when there is no secondary draw to execute.
    {
        namespace rhi = deren::promise::rhi;
        scene_pass scene;
        scene_frame const empty_frame{.extent = {.width = 128u, .height = 64u}};
        scene.set_frame(empty_frame); // No leaves, secondary buffers, or draw environment are needed for a clear.
        contract_command_buffer commands;
        deren::utility::vessel::array<resolved_binding, 6> targets = {};
        for (std::size_t i = 0; i < targets.size(); ++i) {
            targets[i].view_handle = reinterpret_cast<rhi::image_view*>(std::uintptr_t{0x1000} + i * 16u);
        }
        resolved_io io;
        io.list = &commands;
        io.targets = targets;
        CHECK(scene.record_content(io) == rhi::error::ok);
        CHECK(commands.rendering_begins == 1u);
        CHECK(commands.rendering_ends == 1u);
        CHECK(commands.secondary_executions == 0u);
        CHECK(!commands.rendering_secondary_contents);
        CHECK(commands.rendering_area.width == 128u);
        CHECK(commands.rendering_area.height == 64u);
        CHECK(commands.rendering_colors.size() == 5u);
        for (auto const& color : commands.rendering_colors) {
            CHECK(color.load == rhi::load_op::clear);
            CHECK(color.store == rhi::store_op::store);
            CHECK(color.clear[0] == 0.0f && color.clear[1] == 0.0f && color.clear[2] == 0.0f && color.clear[3] == 0.0f);
        }
        CHECK(commands.rendering_has_depth);
        CHECK(commands.rendering_depth.load == rhi::load_op::clear);
        CHECK(commands.rendering_depth.store == rhi::store_op::store);
        CHECK(commands.rendering_depth.clear_depth == 1.0f);

        scene.set_frame(scene_frame{}); // An uninitialized frame must not open a zero-area instance.
        CHECK(scene.record_content(io) == rhi::error::invalid_argument);
        CHECK(commands.rendering_begins == 1u);
        scene.set_frame(empty_frame);
        commands.rendering_result = rhi::error::invalid_argument;
        CHECK(scene.record_content(io) == rhi::error::invalid_argument);
        CHECK(commands.rendering_begins == 2u);
        CHECK(commands.rendering_ends == 1u); // Never close a scope whose begin was refused.
        commands.rendering_result = rhi::error::ok;
        io.targets = std::span(targets).first(5);
        CHECK(scene.record_content(io) == rhi::error::invalid_argument);
        io.targets = targets;
        targets[0].view_handle = nullptr;
        CHECK(scene.record_content(io) == rhi::error::invalid_argument);
        CHECK(commands.rendering_begins == 2u);
    }

    // A secondary lifecycle failure must close the primary scope and refuse the graph callback.
    for (bool parallel : {false, true})
        for (unsigned failure = 0; failure < 4; ++failure) {
            namespace rhi = deren::promise::rhi;
            struct leaf final : deren::engine::primitive {
                void draw(deren::engine::render_environment&) const override {
                }
                void destroy() noexcept override {
                }
                bool is_valid() const noexcept override {
                    return true;
                }
            } leaf;
            std::array<deren::engine::primitive const*, 4> leaves{&leaf, &leaf, &leaf, &leaf};
            contract_command_buffer commands;
            contract_mergeable first, second;
            if (failure == 1)
                first.begin_result = rhi::error::operation_failed;
            if (failure == 2)
                first.end_result = rhi::error::operation_failed;
            if (failure == 3)
                commands.execute_result = rhi::error::operation_failed;
            rhi::mergeable_command_buffer_ref const first_ref = rhi::shared_object<rhi::mergeable_command_buffer>::borrow(first);
            rhi::mergeable_command_buffer_ref const second_ref = rhi::shared_object<rhi::mergeable_command_buffer>::borrow(second);
            std::array<segment_buffer, 2> segments{{{first_ref}, {second_ref}}};
            scene_frame frame{.leaves = parallel ? std::span<deren::engine::primitive const* const>(leaves) : std::span<deren::engine::primitive const* const>(leaves).first(1),
                              .segments = parallel ? std::span<segment_buffer const>(segments) : std::span<segment_buffer const>(segments).first(1),
                              .make_environment = [](void*, deren::engine::recording_target, bool) { return deren::engine::render_environment{}; },
                              .run_tasks = [](void*, std::span<std::function<void()>> tasks) { for (auto& task : tasks) task(); },
                              .extent = {16, 16}};
            scene_pass scene;
            scene.set_frame(frame);
            std::array<resolved_binding, 6> targets{};
            for (auto& target : targets)
                target.view_handle = reinterpret_cast<rhi::image_view*>(0x1000);
            resolved_io io;
            io.list = &commands;
            io.targets = targets;
            CHECK(scene.record_content(io) == (failure ? rhi::error::operation_failed : rhi::error::ok));
            CHECK(commands.rendering_begins == 1 && commands.rendering_ends == 1);
            if (failure == 1 || failure == 2)
                CHECK(commands.secondary_executions == (parallel ? 1U : 0U));
        }

    host_state state;
    pass_host const host = make_host(state);

    fake_pass probe{named_io("probe"), compute_behaviour, "gi", state};
    fake_pass tail{named_io("tail", fullscreen_targets), fullscreen_behaviour, "gi", state};
    fake_pass gated{named_io("gated"), compute_behaviour, "off", state};
    fake_pass bad{bad_io(), compute_behaviour, "gi", state};

    // ---- build: every declaration is validated first, and a bad one refuses the whole stage ----
    {
        deren::utility::vessel::array<frame_pass*, 2> passes = {&probe, &tail};
        stage const st = {.name = "scene", .passes = passes};
        state.log.clear();
        run_report const report = create_stage(st, make_context());
        CHECK(report.created == 2);
        CHECK(report.rejected.empty());
        CHECK(at(state.log, "create:probe") < at(state.log, "create:tail")); // declaration order, not container order
    }
    {
        deren::utility::vessel::array<frame_pass*, 2> passes = {&probe, &bad};
        stage const st = {.name = "scene", .passes = passes};
        state.log.clear();
        run_report const report = create_stage(st, make_context());
        CHECK(report.rejected == "bad"); // the pass's own name, for a startup message that says which one
        CHECK(report.created == 1);      // and the stage stops there rather than running a bad declaration
        CHECK(!has(state.log, "create:bad"));
    }
    {
        deren::utility::vessel::array<frame_pass*, 3> passes = {&probe, nullptr, &tail};
        stage const st = {.name = "scene", .passes = passes};
        state.log.clear();
        CHECK(create_stage(st, make_context()).created == 2); // a null slot is skipped, not counted
    }

    // ---- the create-time context on its own: a pass is built from THIS and nothing else ----
    {
        vp::pass_context const context = make_context();
        // a pass is built from the CONTEXT alone: no frame, no runner, no runtime - the property that makes a
        // pass constructible outside this renderer
        CHECK(context.samplers.of(rr::sampler_hint::shadow) == fake_shadow_sampler);
        // ... and it carries NO set layout and NO pipeline layout: every stage is heap-native, so a pass builds
        // its pipeline with a null layout and reaches its descriptors through the frame's heap. The context is
        // the create-time facts a pass cannot derive, and nothing that could bind a set.
        CHECK(context.shader(context.owner, "fake.comp.spv").size() == 3);
        CHECK(context.shader(context.owner, "missing.comp.spv").empty());
        // ... and the SURFACE's format, which a pipeline that renders into the swapchain must be created with:
        // a session-stable device fact the host hands over rather than one a pass could guess (the extent, which
        // DOES change, is deliberately not here - a pass that bakes one rebuilds in on_swapchain_recreated)
        CHECK(context.swap_chain_image_format == deren::promise::rhi::image_format::bgra8_srgb);
        // ... and the DEPTH format, the second session-stable format - the shadow pass's pipeline has a depth
        // attachment and no colour one, so the surface's format is the wrong fact for it
        CHECK(context.depth_format == deren::promise::rhi::image_format::depth);
    }

    // ---- what a pass is given at CREATE time: the contract face, the five samplers, and two lookups - and nothing
    //      allocates or runs a frame ----
    {
        deren::utility::vessel::array<frame_pass*, 1> passes = {&probe};
        stage const st = {.name = "scene", .passes = passes};
        state.log.clear();
        state.created_with_sampler = nullptr;
        run_report const built = create_stage(st, make_context());
        CHECK(built.created == 1);
        CHECK(state.created_with_sampler == fake_shadow_sampler); // chosen by hint, never named by the pass
    }

    // ---- a rebuilt swapchain tells EVERY pass: the hazard the manual reset list keeps missing ----
    {
        deren::utility::vessel::array<frame_pass*, 3> passes = {&probe, &tail, &gated};
        stage const st = {.name = "scene", .passes = passes};
        state.log.clear();
        run_report const report = recreate_stage(st, host);
        CHECK(report.recreated == 3);
        CHECK(has(state.log, "recreate:probe"));
        CHECK(has(state.log, "recreate:tail"));
        CHECK(has(state.log, "recreate:gated")); // even the pass this frame skipped
    }

    // ---- borrowed contract samplers are selected by the declaration's hint ----
    {
        rr::shared::sampler_set const samplers = {.gbuffer = &gbuffer_sampler_object, .shadow = fake_shadow_sampler};
        CHECK(samplers.of(rr::sampler_hint::shadow) == fake_shadow_sampler);
        CHECK(samplers.of(rr::sampler_hint::gbuffer) == &gbuffer_sampler_object);
        CHECK(samplers.of(rr::sampler_hint::none) == nullptr);
        CHECK(samplers.of(rr::sampler_hint::taa) == nullptr); // the owner published no object for this hint
        CHECK(samplers.of(static_cast<rr::sampler_hint>(255)) == nullptr);
        // a declaration's own bindings are exactly the bindings its shader declares, in order - checked against the
        // stochastic punctual lighting chain's temporal resolve, which is the shape the pipeline builder this
        // test covers still builds from (that declaration went with the removed chain)
        uint32_t own = 0;
        for (rr::pass_binding const& b : rr::taa_io.bindings) {
            if (b.owner == rr::binding_owner::own) {
                CHECK(b.binding == own); // contiguous from zero: the index IS the binding number
                ++own;
            }
        }
        CHECK(own == 4);
    }

    // ---- THE CHAIN: a value that holds a run of passes and its ORDER, plus the lookups an owner uses - the
    //      runner steps it used to wrap (`init`/`record`) went with the legacy runner, and the graph's builder takes
    //      the same pass list through `stage` ----
    {
        pass_chain chain{"gi"};
        CHECK(chain.empty());
        CHECK(chain.name() == "gi");
        chain.add(probe);
        chain.add(tail);
        CHECK(chain.size() == 2);
        CHECK(chain.find("tail") == static_cast<frame_pass*>(&tail));
        CHECK(chain.find("nothing") == nullptr);
        // ... and the stage it hands a resolver is its own name and its own list
        CHECK(chain.as_stage().name == "gi");
        CHECK(chain.as_stage().passes.size() == 2);
    }

    // ---- the chain's two convenience calls the runtime still uses: `init` is the whole create step and
    //      `ready(name)` is the readiness question asked in the DECLARATION's vocabulary ----
    {
        pass_chain chain{"gi"};
        chain.add(probe);
        chain.add(tail);
        state.log.clear();
        run_report const created = chain.init(make_context());
        CHECK(created.created == 2);
        CHECK(created.rejected.empty());
        CHECK(chain.ready("probe"));
        CHECK(chain.ready("tail"));
        CHECK(!chain.ready("absent"));
        probe.is_ready = false;
        CHECK(!chain.ready("probe"));
        CHECK(chain.ready("tail")); // ... independently of its neighbour
        probe.is_ready = true;
    }

    // ---- the chain also KEPT objects that are not passes (`keep`, whose only users were the renderer's two jobs).
    //      THE PROMISE WENT WITH THE FUNCTION: those two jobs are ordinary members of the renderer now, because they
    //      are the only non-pass GPU-owning objects left, and a container that exists to hold exactly two objects of
    //      two known types is more machinery than two members. Nothing tests it any more; `emplace`, the owning form
    //      for a PASS, is covered by the stage and list assertions above ----

    // ---- the resource table: what EXISTS, keyed the way a DECLARATION names it (resource + element + instance).
    //      The instance rule is the schema's own SCOPE, and it is one function so the publisher and the reader
    //      cannot disagree - which is the whole reason this container exists instead of sixteen resolvers ----
    {
        vp::frame_identity const frame = {.image_index = 2, .slot = 1, .image_count = 3, .extent = {64, 64}};
        CHECK(vp::instance_for(rr::resource_scope::per_swapchain_image, frame) == 2);
        CHECK(vp::instance_for(rr::resource_scope::per_frame_slot, frame) == 1);
        CHECK(vp::instance_for(rr::resource_scope::device_wide, frame) == 0); // one instance, whatever the frame says

        vp::resource_table table;
        deren::promise::rhi::image_view* const view_a = reinterpret_cast<deren::promise::rhi::image_view*>(0x1000);
        deren::promise::rhi::image* const image_a = reinterpret_cast<deren::promise::rhi::image*>(0x2000);
        deren::promise::rhi::image_view* const view_b = reinterpret_cast<deren::promise::rhi::image_view*>(0x3000);
        deren::promise::rhi::image* const image_b = reinterpret_cast<deren::promise::rhi::image*>(0x4000);
        deren::promise::rhi::buffer* const buffer_a = reinterpret_cast<deren::promise::rhi::buffer*>(0x5000);

        // an empty table answers all-null rather than failing: "the owner does not have it" and "this frame
        // cannot use it" are different statements, and only the second is a pass's business
        CHECK(table.size() == 0);
        CHECK(table.find(rr::resource_id::hdr, 0, 0).view_handle == nullptr);
        CHECK(table.find(rr::resource_id::hdr, 0, 0).image_handle == nullptr);
        CHECK(table.find(rr::resource_id::hdr, 0, 0).buffer_handle == nullptr);

        // one entry per IMAGE, which is what makes `own_per_image` and the bloom levels expressible
        table.publish(rr::resource_id::hdr, 0, 0, {.image_handle = image_a, .view_handle = view_a});
        table.publish(rr::resource_id::hdr, 0, 1, {.image_handle = image_b, .view_handle = view_b});
        CHECK(table.size() == 2);
        CHECK(table.find(rr::resource_id::hdr, 0, 0).view_handle == view_a);
        CHECK(table.find(rr::resource_id::hdr, 0, 1).view_handle == view_b);
        CHECK(table.find(rr::resource_id::hdr, 0, 2).view_handle == nullptr); // no third image this generation
        CHECK(table.instances_of(rr::resource_id::hdr, 0) == 2);
        // the ELEMENT is part of the key: the bloom chain's four levels are four entries of one family
        table.publish(rr::resource_id::bloom, 3, 0, {.image_handle = image_a, .view_handle = view_a});
        CHECK(table.size() == 3);
        CHECK(table.find(rr::resource_id::bloom, 3, 0).view_handle == view_a);
        CHECK(table.find(rr::resource_id::bloom, 2, 0).view_handle == nullptr);
        CHECK(table.instances_of(rr::resource_id::bloom, 3) == 1);
        // a buffer-only family carries the buffer and leaves the image lanes null, which is how a barrier
        // buffer is told apart from an image with a view
        table.publish(rr::resource_id::cluster_counts, 0, 1, {.buffer_handle = buffer_a});
        CHECK(table.find(rr::resource_id::cluster_counts, 0, 1).buffer_handle == buffer_a);
        CHECK(table.find(rr::resource_id::cluster_counts, 0, 1).view_handle == nullptr);
        CHECK(table.find(rr::resource_id::cluster_counts, 1, 1).buffer_handle == nullptr);

        // publishing the same key REPLACES it: a frame that re-publishes an alias (scene_color is TAA-or-HDR)
        // ends with the last value rather than with two entries that disagree
        table.publish(rr::resource_id::hdr, 0, 0, {.image_handle = image_b, .view_handle = view_b});
        CHECK(table.size() == 4);
        CHECK(table.find(rr::resource_id::hdr, 0, 0).view_handle == view_b);

        // ... and `clear` is what makes the next frame's publication the whole truth
        table.clear();
        CHECK(table.size() == 0);
        CHECK(table.find(rr::resource_id::hdr, 0, 0).view_handle == VK_NULL_HANDLE);
    }

    // ---- the frame constants ride with the resolved I/O: a value, defaulted, so a pass that reads it before
    //      the frame loop has filled it reads zeros rather than whatever was on the stack ----
    {
        vp::resolved_io io = {};
        CHECK(io.constants.scene_radius == 0.0f);
        CHECK(io.constants.view == glm::mat4(1.0f)); // the identity, not the frame's camera: nothing has run yet
    }

    // ---- what `frame_pass::resolve` does unless a pass overrides it: the DECLARATION-driven resolution, whose
    //      only source of handles is the resource table. This is the piece the renderer's sixteen hand-written
    //      `resolve_*_pass` functions are being replaced by, one pass at a time, and the rules it enforces are
    //      the ones every one of them wrote by hand: a missing resource means the pass does not run, the frame's
    //      instance comes from the SCHEMA's scope, and the push block is the pass's own business ----
    {
        /// a pass whose declaration is all it needs. The TAA resolve's declaration is reused rather than
        /// invented: four own bindings (all per-swapchain-image), one render target, and a declared push size
        /// that the resolver must NOT fill.
        struct declared_pass final : vp::frame_pass {
            rr::pass_io const* declaration = &rr::taa_io;
            vp::behaviour how = {.kind = vp::behaviour_kind::fullscreen, .extent = vp::extent_rule::full};
            deren::promise::rhi::pipeline* owned_contract = nullptr;
            [[nodiscard]] rr::pass_io const& io() const noexcept override {
                return *this->declaration;
            }
            [[nodiscard]] vp::behaviour const& behaviour() const noexcept override {
                return this->how;
            }
            [[nodiscard]] std::string_view feature() const noexcept override {
                return {};
            }
            // the passes that build their OWN pipeline answer this, and the resolver must prefer it over a
            // registry entry that happens to share a `behaviour::pipelines` name
            [[nodiscard]] deren::promise::rhi::pipeline* pipeline_handle() const noexcept override {
                return this->owned_contract;
            }
            void create(vp::pass_context const&) override {
            }
            void on_swapchain_recreated(vp::pass_host const&) override {
            }
        };
        /// the owner side of a resolve_context: a table plus the lookups the framework declares
        struct resolver_owner {
            vp::resource_table table;
            deren::promise::rhi::image_extent resource_extent = {7, 9};
            deren::promise::rhi::pipeline* pipeline = reinterpret_cast<deren::promise::rhi::pipeline*>(0x77);
            std::string_view unknown_pipeline = {};
        };
        resolver_owner owner;
        vp::frame_identity const frame = {.image_index = 1, .slot = 0, .image_count = 2, .extent = {64, 32}};
        vp::resolve_context const context = {
            .resources = &owner.table,
            .frame = frame,
            .cmd = fake_cmd,
            .extent_of = [](void* o, rr::resource_id, uint32_t) { return static_cast<resolver_owner*>(o)->resource_extent; },
            .pipeline =
                [](void* o, std::string_view const name) -> vp::owned_pipeline {
                // the owner answers a NAME with a pipeline, or with nothing for a name it does not own
                return name == static_cast<resolver_owner*>(o)->unknown_pipeline
                           ? vp::owned_pipeline{}
                           : vp::owned_pipeline{.contract = static_cast<resolver_owner*>(o)->pipeline};
            },
            .owner = &owner,
        };
        declared_pass pass;
        vp::resolved_io io = {};
        deren::utility::vessel::array<rr::pass_binding, 1> const as_binding = {{{.binding = 0,
                                                                                 .owner = rr::binding_owner::own,
                                                                                 .kind = rr::binding_kind::acceleration_structure,
                                                                                 .resource = rr::resource_id::top_level_structure,
                                                                                 .access = rr::binding_access::read}}};
        rr::pass_io const as_declaration{.name = "as_consumer", .bindings = as_binding};
        declared_pass as_pass;
        as_pass.declaration = &as_declaration;
        auto* structure = reinterpret_cast<deren::promise::rhi::acceleration_structure*>(0x4400);
        CHECK(!as_pass.resolve(context, io));
        owner.table.publish(rr::resource_id::top_level_structure, 0, 1, {.structure_handle = structure});
        CHECK(!as_pass.resolve(context, io)); // Another frame slot must not satisfy this frame's dependency.
        owner.table.publish(rr::resource_id::top_level_structure, 0, frame.slot, {.structure_handle = structure});
        CHECK(as_pass.resolve(context, io) && io.own.size() == 1 && io.own[0].structure_handle == structure);
        owner.table.clear();
        // the framework's DEFAULT readiness answer: this pass overrides `ready()` nowhere, because it builds nothing
        // the run has to wait for - a pass with nothing of its own has nothing to be unready ABOUT (see
        // frame_pass::ready)
        CHECK(pass.ready());

        // AN EMPTY TABLE: the frame does not have what the declaration names, so the pass does not run at all -
        // the rule that replaced every resolver's `if (images.empty() || index >= count) return false;`
        CHECK(!pass.resolve(context, io));

        // ... publish this frame's four bindings and its target AS FAMILIES (the run of views and images the owner
        // already holds - the shape `publish_family` exists for), and the SAME declaration resolves
        for (uint32_t image = 0; image < 2; ++image) {
            for (rr::resource_id const id : {rr::resource_id::scene_color, rr::resource_id::taa_history, rr::resource_id::velocity, rr::resource_id::gbuffer_depth}) {
                uint64_t const tag = static_cast<uint64_t>(id) + image;
                owner.table.publish(id, 0, image, {.image_handle = reinterpret_cast<deren::promise::rhi::image*>(0x9000 + tag), .view_handle = reinterpret_cast<deren::promise::rhi::image_view*>(0x8000 + tag)});
            }
        }
        deren::promise::rhi::image_view* const hdr_view = reinterpret_cast<deren::promise::rhi::image_view*>(0xAA);
        owner.table.publish(rr::resource_id::hdr, 0, frame.image_index, {.image_handle = reinterpret_cast<deren::promise::rhi::image*>(0xBB), .view_handle = hdr_view});
        CHECK(pass.resolve(context, io));
        CHECK(io.cmd == fake_cmd);
        CHECK(io.frame.image_index == frame.image_index);
        CHECK(io.own.size() == 4); // the four own bindings, in their own binding order
        CHECK(io.own[0].image_handle == reinterpret_cast<deren::promise::rhi::image*>(0x9000 + static_cast<uint64_t>(rr::resource_id::scene_color) + frame.image_index));
        CHECK(io.own[3].image_handle == reinterpret_cast<deren::promise::rhi::image*>(0x9000 + static_cast<uint64_t>(rr::resource_id::gbuffer_depth) + frame.image_index));
        CHECK(io.targets.size() == 1);
        CHECK(io.targets[0].view_handle == hdr_view);
        CHECK(io.extent.width == 64 && io.extent.height == 32); // the declaration's rule is `full`
        CHECK(io.push.empty());                                 // a pass composes its own push block
        CHECK(io.barrier_images.empty() && io.barrier_buffers.empty());
        // THE PER-IMAGE CHANNEL is empty here (these were published instance by instance), which is the shape the
        // one consumer reads: it checks each span's length before indexing it
        CHECK(io.own_per_image[0].empty());

        // ... and published AS A FAMILY, the same declaration also hands the pass every image's view - the run
        // `views_of` returns, in the owner's own storage, which is what a per-image descriptor family writes from
        deren::utility::vessel::array<deren::promise::rhi::image_view*, 2> const family_views = {reinterpret_cast<deren::promise::rhi::image_view*>(0xF0), reinterpret_cast<deren::promise::rhi::image_view*>(0xF1)};
        deren::utility::vessel::array<deren::promise::rhi::image*, 2> const family_images = {reinterpret_cast<deren::promise::rhi::image*>(0xE0), reinterpret_cast<deren::promise::rhi::image*>(0xE1)};
        owner.table.clear();
        for (rr::resource_id const id : {rr::resource_id::scene_color, rr::resource_id::taa_history, rr::resource_id::velocity, rr::resource_id::gbuffer_depth}) {
            owner.table.publish_family(id, 0, family_images, family_views);
        }
        owner.table.publish_family(rr::resource_id::hdr, 0, family_images, family_views);
        CHECK(pass.resolve(context, io));
        CHECK(io.own_per_image[0].size() == 2);                                          // one view per image of the generation
        CHECK(io.own_per_image[0][1] == family_views[1]);                                // ... in instance order
        CHECK(io.own_per_image[3].data() == family_views.data());                        // and it is the OWNER's run, not a copy
        CHECK(io.own[frame.image_index].view_handle == family_views[frame.image_index]); // find() answers from the family too
        CHECK(io.targets[0].image_handle == family_images[frame.image_index]);
        CHECK(owner.table.instances_of(rr::resource_id::hdr, 0) == 2);
        CHECK(owner.table.size() == 5); // the four bindings' families + the target's, one entry each

        // A MISSING TARGET is the same statement as a missing binding: do not record the pass
        owner.table.clear();
        for (rr::resource_id const id : {rr::resource_id::scene_color, rr::resource_id::taa_history, rr::resource_id::velocity, rr::resource_id::gbuffer_depth}) {
            owner.table.publish_family(id, 0, family_images, family_views);
        }
        CHECK(!pass.resolve(context, io));

        // THE EXTENT RULES, applied by the framework from the behaviour: half is the formula the half-size images
        // are created with, `resource` is the owner's answer, and `none` means the pass sizes its own work
        owner.table.publish_family(rr::resource_id::hdr, 0, family_images, family_views);
        pass.how.extent = vp::extent_rule::half;
        CHECK(pass.resolve(context, io));
        CHECK(io.extent.width == 32 && io.extent.height == 16);
        pass.how.extent = vp::extent_rule::resource;
        pass.how.extent_of = rr::resource_id::bloom;
        pass.how.extent_of_element = 2;
        CHECK(pass.resolve(context, io));
        CHECK(io.extent.width == 7 && io.extent.height == 9); // the owner's answer for that element
        pass.how.extent = vp::extent_rule::none;
        CHECK(pass.resolve(context, io));
        CHECK(io.extent.width == 0 && io.extent.height == 0); // "I size my own work", not "the frame's size"

        // THE PIPELINES the behaviour names: resolved by name through the owner, and a name the owner cannot
        // answer means the frame cannot bind what the pass declared - so the pass does not run
        constexpr deren::utility::vessel::array<std::string_view, 1> pipeline_names = {"taa"};
        pass.how.extent = vp::extent_rule::full;
        pass.how.pipelines = pipeline_names;
        CHECK(pass.resolve(context, io));
        CHECK(io.pipelines.size() == 1);
        CHECK(io.pipelines[0] == owner.pipeline);
        owner.unknown_pipeline = "taa";
        CHECK(!pass.resolve(context, io));
        owner.unknown_pipeline = {};

        // ... and a pass that BUILT its own pipeline is handed its own, never the same-named registry entry: that
        // is the whole reason the interface asks the pass first
        pass.owned_contract = reinterpret_cast<deren::promise::rhi::pipeline*>(0x1234);
        CHECK(pass.resolve(context, io));
        CHECK(io.pipelines.size() == 1);
        CHECK(io.pipelines[0] == pass.owned_contract);
        pass.owned_contract = nullptr;

        // A DECLARATION WITH NO BINDING OF ITS OWN IS STILL A DECLARATION: the framework has no set to hand a pass
        // (every stage reads its descriptors from the frame's heap), so such a declaration resolves with no own
        // binding and no pipeline, and the extent rule is still applied. (The shared-set entries this block used
        // to assert were declaration-only facts; they went with the vocabulary.)
        rr::pass_io const shared_only = {.name = "shared", .bindings = {}, .targets = {}, .push = std::nullopt};
        declared_pass shared_pass;
        shared_pass.declaration = &shared_only;
        vp::resolved_io shared_io = {};
        CHECK(shared_pass.resolve(context, shared_io));
        CHECK(shared_io.own.empty());                        // the declaration names no own binding
        CHECK(shared_io.pipelines.empty());                  // ... and no pipeline
        CHECK(shared_io.extent.width == frame.extent.width); // the extent rule is still applied
        CHECK(shared_pass.resolve(context, shared_io));      // resolving twice is idempotent (a per-frame contract)

        // The FAMILY/ELEMENT pair the post chain's stages used to name is gone with the shared-set vocabulary: those
        // stages now differ by the resource ELEMENT they render into and move, which the declarations carry as
        // `render_target::element` and `barrier_image::element` and the resolver hands over element by element.

        // A RUN OF ELEMENTS: ONE target entry claiming `count` consecutive elements resolves to ONE SLOT PER
        // ELEMENT - the shadow map's cascades are the case (`render_target::count`), and what makes it a
        // declaration fact rather than a resolver's is that the FRAME CAPS THE RUN: the family has the elements the
        // owner published (the layers the cascade knob asked for), and the pass renders what it is handed.
        deren::utility::vessel::array<rr::render_target, 1> const run_decl = {rr::render_target{.resource = rr::resource_id::shadow_map, .element = 0, .kind = rr::target_kind::depth, .count = 4}};
        rr::pass_io const run_io = {.name = "shadow", .bindings = {}, .targets = run_decl, .push = std::nullopt};
        declared_pass run_pass;
        run_pass.declaration = &run_io;
        // a per-FRAME-SLOT family, which is why the instance is the frame's SLOT rather than its image
        owner.table.clear();
        for (uint32_t layer = 0; layer < 3; ++layer) {
            // `0xC0 + layer` is 32 bits wide, and MSVC /W4 reports C4312 for a uint32_t reinterpret_cast to the
            // 64-bit VkImageView handle, so the fabricated handle is widened to pointer size first.
            owner.table.publish(rr::resource_id::shadow_map, layer, frame.slot,
                                {.image_handle = reinterpret_cast<deren::promise::rhi::image*>(0xD0), .view_handle = reinterpret_cast<deren::promise::rhi::image_view*>(static_cast<std::uintptr_t>(0xC0 + layer))});
        }
        vp::resolved_io run_out = {};
        CHECK(run_pass.resolve(context, run_out));
        CHECK(run_out.targets.size() == 3); // the run ends where the FRAME's elements end, not at the declaration's count
        CHECK(run_out.targets[0].view_handle == reinterpret_cast<deren::promise::rhi::image_view*>(0xC0));
        CHECK(run_out.targets[1].view_handle == reinterpret_cast<deren::promise::rhi::image_view*>(0xC1));
        CHECK(run_out.targets[2].view_handle == reinterpret_cast<deren::promise::rhi::image_view*>(0xC2));
        CHECK(run_out.targets[0].image_handle == reinterpret_cast<deren::promise::rhi::image*>(0xD0)); // ONE image behind every layer of the run
        // ... a frame with ONE layer hands over one target (the single-shadow-map configuration: the same
        // declaration, a different frame) ...
        owner.table.clear();
        owner.table.publish(rr::resource_id::shadow_map, 0, frame.slot, {.image_handle = reinterpret_cast<deren::promise::rhi::image*>(0xD0), .view_handle = reinterpret_cast<deren::promise::rhi::image_view*>(0xC0)});
        CHECK(run_pass.resolve(context, run_out));
        CHECK(run_out.targets.size() == 1);
        // ... and a frame with NONE does not run the pass at all, exactly like a missing single target
        owner.table.clear();
        CHECK(!run_pass.resolve(context, run_out));
    }

    return deren::vk_test::finish("test_pass");
}
