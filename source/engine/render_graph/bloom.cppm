module;
#include <array>
#include <expected>
#include <memory>
#include <type_traits>
#include <utility>
export module deren.engine.render_graph.bloom;
export import deren.engine.render_graph.execute;
export namespace deren::engine::render_graph::bloom {
    inline constexpr unsigned level_count = 4;
    /// Incoming states are explicit handoffs, including outstanding accesses on reused images.
    struct description {
        resource_identity input;
        std::array<resource_identity, level_count> levels;
        resource_state input_state;
        std::array<resource_state, level_count> level_states{};
    };
    [[nodiscard]] std::expected<compiled_graph, graph_error> compile(description const& desc) {
        // Bloom reads one image and clears four distinct single-mip/layer color targets.
        // Aliasing any of these changes that algorithm, even if a general graph can model it.
        for (unsigned i = 0; i < level_count; ++i) {
            if (desc.levels[i] == desc.input) {
                return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = {}, .resource_name = {}});
            }
            for (unsigned j = 0; j < i; ++j) {
                if (desc.levels[i] == desc.levels[j]) {
                    return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = {}, .resource_name = {}});
                }
            }
        }
        builder graph;
        auto input = graph.import_texture(desc.input, initial_contents::defined, "bloom input", {}, desc.input_state);
        if (!input) {
            return std::unexpected(input.error());
        }
        constexpr std::array names{"bloom prefilter", "bloom downsample 1", "bloom downsample 2", "bloom downsample 3"};
        std::array<texture_ref, level_count> levels;
        for (unsigned i = 0; i < level_count; ++i) {
            auto target = graph.import_texture(desc.levels[i], initial_contents::undefined, names[i], {}, desc.level_states[i]);
            if (!target) {
                return std::unexpected(target.error());
            }
            levels[i] = *target;
            std::array<resource_access, 2> accesses{{{i ? levels[i - 1] : *input, content_access::read, {}, {resource_usage::sampled, pipeline_stage::fragment_shader}},
                                                     {*target, content_access::discard_write, {}, {resource_usage::color_attachment, pipeline_stage::color_output}}}};
            auto pass = graph.add_pass(names[i], accesses);
            if (!pass) {
                return std::unexpected(pass.error());
            }
            auto output = graph.export_resource(*target);
            if (!output) {
                return std::unexpected(output.error());
            }
        }
        // export_resource roots contents but emits no epilogue. An explicit boundary reader
        // orders the final color write before the external composite's fragment reads.
        std::array<resource_access, level_count> handoff{{{levels[0], content_access::read, {}, {resource_usage::sampled, pipeline_stage::fragment_shader}},
                                                          {levels[1], content_access::read, {}, {resource_usage::sampled, pipeline_stage::fragment_shader}},
                                                          {levels[2], content_access::read, {}, {resource_usage::sampled, pipeline_stage::fragment_shader}},
                                                          {levels[3], content_access::read, {}, {resource_usage::sampled, pipeline_stage::fragment_shader}}}};
        auto boundary = graph.add_pass("bloom to composite", handoff, {true});
        if (!boundary) {
            return std::unexpected(boundary.error());
        }
        return graph.compile({true});
    }
    class prepared_chain {
        execution::prepared_graph graph_;

    public:
        using image_lease = resource_lease<execution::rhi::image>;
        [[nodiscard]] static std::expected<prepared_chain, execution::execution_error> prepare(
            execution::rhi::shared_object<execution::rhi::api_core> core, image_lease const& input,
            std::array<image_lease, level_count> const& levels, resource_state input_state,
            std::array<resource_state, level_count> const& level_states) {
            description desc{execution::identify(input), {}, input_state, level_states};
            std::array<execution::resource_binding, level_count + 1> bindings;
            bindings[0] = input;
            for (unsigned i = 0; i < level_count; ++i) {
                desc.levels[i] = execution::identify(levels[i]);
                bindings[i + 1] = levels[i];
            }
            auto plan = compile(desc);
            if (!plan) {
                return std::unexpected(execution::execution_error{execution::rhi::error::invalid_argument, plan.error().pass, plan.error().resource});
            }
            auto prepared = execution::prepared_graph::prepare(std::move(core), std::move(*plan), bindings);
            if (!prepared) {
                return std::unexpected(prepared.error());
            }
            prepared_chain result;
            result.graph_ = std::move(*prepared);
            return result;
        }
        /// The caller binds the existing post pipeline/viewport/heap and records only drawing content.
        template <typename F>
            requires std::is_nothrow_invocable_r_v<execution::rhi::error, F const&, unsigned, execution::record_context const&>
        execution::rhi::error record(execution::rhi::command_buffer& commands, F const& content) const noexcept {
            return graph_.record(commands, [&](execution::record_context const& context) noexcept {
                return context.pass_index() < level_count ? content(context.pass_index(), context) : execution::rhi::error::ok;
            });
        }
        [[nodiscard]] std::shared_ptr<void const> retention() const noexcept {
            return graph_.retention();
        }
    };
} // namespace deren::engine::render_graph::bloom
