module;
#include <array>
#include <expected>
#include <optional>
#include <utility>
export module deren.engine.render_graph.post;
export import deren.engine.render_graph.execute;
export namespace deren::engine::render_graph::post {
    enum class resolve { none,
                         fxaa,
                         upscale };
    struct bloom_parameters {
        texture_read source;
        texture_discard_write target;
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(source, target);
        }
    };
    struct clear_parameters {
        texture_discard_write target;
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(target);
        }
    };
    struct composite_parameters {
        deren::utility::vessel::vector<texture_read> reads;
        texture_discard_write target;
        void visit_resources(parameter_visitor& visit) const noexcept {
            for (auto const& read : reads) {
                visit(read);
            }
            visit(target);
        }
    };
    struct resolve_parameters {
        texture_read source;
        texture_discard_write target;
        void visit_resources(parameter_visitor& visit) const noexcept {
            visit(source, target);
        }
    };
    struct inputs {
        texture_ref hdr, frame;
        std::array<texture_ref, 4> bloom;
        std::optional<texture_ref> ldr, lut;
    };
    struct options {
        resolve resolution = resolve::none;
        bool bloom_enabled = true;
    };
    struct outputs {
        texture_ref color;
        std::array<pass_ref, 4> bloom;
        pass_ref composite;
        std::optional<pass_ref> resolve;
    };
    [[nodiscard]] std::expected<outputs, graph_error> append(builder& graph, inputs const& in, options const& settings) noexcept {
        if (settings.resolution != resolve::none && settings.resolution != resolve::fxaa && settings.resolution != resolve::upscale) {
            return std::unexpected(graph_error{.code = error_code::invalid_scope, .pass_name = {}, .resource_name = {}});
        }
        if (settings.resolution != resolve::none && !in.ldr) {
            return std::unexpected(graph_error{.code = error_code::invalid_ref, .pass_name = {}, .resource_name = "post LDR"});
        }
        deren::utility::vessel::vector<texture_ref> refs{in.hdr, in.frame};
        refs.insert(refs.end(), in.bloom.begin(), in.bloom.end());
        if (settings.resolution != resolve::none) {
            refs.push_back(*in.ldr);
        }
        if (in.lut) {
            refs.push_back(*in.lut);
        }
        for (std::size_t i = 0; i < refs.size(); ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                if (refs[i] == refs[j]) {
                    return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = {}, .resource_name = "post alias"});
                }
            }
        }
        outputs out{.color = in.frame, .bloom = {}, .composite = {}, .resolve = {}};
        constexpr access_scope sampled{resource_usage::sampled, pipeline_stage::fragment_shader};
        constexpr access_scope attachment{resource_usage::color_attachment, pipeline_stage::color_output};
        constexpr std::array names{"bloom prefilter", "bloom downsample 1", "bloom downsample 2", "bloom downsample 3"};
        for (unsigned i = 0; i < 4; ++i) {
            auto added = settings.bloom_enabled
                             ? graph.add_pass(names[i], bloom_parameters{{i ? in.bloom[i - 1] : in.hdr, {}, sampled}, {in.bloom[i], {}, attachment}})
                             : graph.add_pass(names[i], clear_parameters{{in.bloom[i], {}, attachment}});
            if (!added) {
                return std::unexpected(added.error());
            }
            out.bloom[i] = *added;
        }
        auto target = settings.resolution == resolve::none ? in.frame : *in.ldr;
        composite_parameters composite{{}, {target, {}, attachment}};
        composite.reads.push_back({in.hdr, {}, sampled});
        for (auto ref : in.bloom) {
            composite.reads.push_back({ref, {}, sampled});
        }
        if (in.lut) {
            composite.reads.push_back({*in.lut, {}, sampled});
        }
        auto added = graph.add_pass("post composite", std::move(composite));
        if (!added) {
            return std::unexpected(added.error());
        }
        out.composite = *added;
        if (settings.resolution != resolve::none) {
            auto resolved = graph.add_pass(settings.resolution == resolve::fxaa ? "post FXAA" : "post upscale",
                                           resolve_parameters{{target, {}, sampled}, {in.frame, {}, attachment}});
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            out.resolve = *resolved;
        }
        return out;
    }
    struct description {
        resource_identity input{}, output{}, ldr{};
        std::array<resource_identity, 4> levels{};
        std::optional<resource_identity> lut;
        resolve resolution = resolve::none;
        bool bloom_enabled = true;
        resource_state input_state{resource_layout::shader_read_only, pipeline_stage::fragment_shader, memory_access::read};
        resource_state ldr_state{};
        std::array<resource_state, 4> level_states{};
    };
    [[nodiscard]] std::expected<compiled_graph, graph_error> compile(description const& desc) {
        builder graph;
        deren::utility::vessel::vector<resource_identity> identities;
        auto import = [&](resource_identity id, initial_contents contents, char const* name, resource_state state) -> std::expected<texture_ref, graph_error> {
            for (auto prior : identities) {
                if (prior == id) {
                    return std::unexpected(graph_error{.code = error_code::conflicting_import, .pass_name = {}, .resource_name = name});
                }
            }
            identities.push_back(id);
            return graph.import_texture(id, contents, name, {}, state);
        };
        auto input = import(desc.input, initial_contents::defined, "post HDR", desc.input_state);
        if (!input) {
            return std::unexpected(input.error());
        }
        auto output = import(desc.output, initial_contents::undefined, "frame output", {});
        if (!output) {
            return std::unexpected(output.error());
        }
        inputs in{};
        in.hdr = *input;
        in.frame = *output;
        for (unsigned i = 0; i < 4; ++i) {
            auto level = import(desc.levels[i], initial_contents::undefined, "bloom level", desc.level_states[i]);
            if (!level) {
                return std::unexpected(level.error());
            }
            in.bloom[i] = *level;
        }
        if (desc.resolution != resolve::none) {
            auto ldr = import(desc.ldr, initial_contents::undefined, "post LDR", desc.ldr_state);
            if (!ldr) {
                return std::unexpected(ldr.error());
            }
            in.ldr = *ldr;
        }
        if (desc.lut) {
            auto lut = import(*desc.lut, initial_contents::defined, "post LUT", {resource_layout::shader_read_only, pipeline_stage::fragment_shader, memory_access::read});
            if (!lut) {
                return std::unexpected(lut.error());
            }
            in.lut = *lut;
        }
        auto appended = append(graph, in, {desc.resolution, desc.bloom_enabled});
        if (!appended) {
            return std::unexpected(appended.error());
        }
        auto exported = graph.export_resource(*output);
        if (!exported) {
            return std::unexpected(exported.error());
        }
        return graph.compile({true});
    }
    /// Screenshot/readback remains an external protocol between drawing and this final boundary.
    [[nodiscard]] std::expected<compiled_graph, graph_error> compile_finish(resource_identity output, bool present) {
        builder graph;
        auto target = graph.import_texture(output, initial_contents::defined, "frame handoff", {},
                                           resource_state{resource_layout::color_attachment, pipeline_stage::color_output | pipeline_stage::transfer, memory_access::read | memory_access::write});
        if (!target) {
            return std::unexpected(target.error());
        }
        std::array<resource_access, 1> access{{{*target, content_access::read, {}, present ? access_scope{resource_usage::present, pipeline_stage::none} : access_scope{resource_usage::sampled, pipeline_stage::fragment_shader}}}};
        auto boundary = graph.add_pass("frame handoff", access, {true});
        if (!boundary) {
            return std::unexpected(boundary.error());
        }
        return graph.compile({true});
    }
} // namespace deren::engine::render_graph::post
