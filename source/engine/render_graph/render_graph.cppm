module;
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
export module deren.engine.render_graph;
export import :types;
export import :parameters;
export import :diagnostics;
export namespace deren::engine::render_graph {
    class builder final {
        struct identity_hash {
            std::size_t operator()(resource_identity const& id) const noexcept {
                auto const a = id.device ^ (id.device >> 33);
                return static_cast<std::size_t>(a ^ (id.object + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2)));
            }
        };
        uint64_t incarnation;
        bool frozen = false;
        deren::utility::vessel::vector<resource_record> resources;
        deren::utility::vessel::vector<pass_record> passes;
        deren::utility::vessel::unordered_map<resource_identity, uint32_t, identity_hash> identities;
        deren::utility::vessel::vector<resource_export> exports;
        deren::utility::vessel::vector<std::pair<uint32_t, uint32_t>> orders;
        graph_error error(error_code, uint32_t pass = invalid_index, uint32_t resource = invalid_index) const;
        std::expected<uint32_t, graph_error> import(resource_identity, initial_contents, std::string_view, resource_kind, resource_shape, std::optional<resource_state>);
        std::expected<void, graph_error> export_index(uint64_t owner, uint32_t index, resource_kind, resource_range);

    public:
        builder();
        builder(builder const&) = delete;
        builder& operator=(builder const&) = delete;
        builder(builder&&) = delete;
        builder& operator=(builder&&) = delete;
        [[nodiscard]] bool contains(pass_ref ref) const noexcept {
            return ref.incarnation_ == incarnation && ref.index_ < passes.size();
        }
        std::expected<texture_ref, graph_error> import_texture(resource_identity, initial_contents, std::string_view name, texture_shape = {}, std::optional<resource_state> = std::nullopt);
        std::expected<buffer_ref, graph_error> import_buffer(resource_identity, initial_contents, std::string_view name, buffer_shape = {}, std::optional<resource_state> = std::nullopt);
        std::expected<pass_ref, graph_error> add_pass(std::string_view, std::span<resource_access const>, pass_options = {});
        template <pass_parameters Parameters>
        std::expected<pass_ref, graph_error> add_pass(std::string_view name, Parameters parameters, pass_options options = {}) {
            if (frozen)
                return std::unexpected(error(error_code::frozen_builder));
            if (passes.size() >= invalid_index)
                return std::unexpected(error(error_code::capacity_exceeded));
            auto snapshot = parameter_snapshot::make(std::move(parameters));
            deren::utility::vessel::vector<resource_access> accesses;
            parameter_visitor visit(accesses);
            snapshot.template get<Parameters>()->visit_resources(visit);
            auto result = add_pass(name, std::span<resource_access const>(accesses), options);
            if (result)
                passes[result->index()].parameters = std::move(snapshot);
            return result;
        }
        std::expected<void, graph_error> export_resource(texture_ref, texture_range = {});
        std::expected<void, graph_error> export_resource(buffer_ref, buffer_range = {});
        std::expected<void, graph_error> order_after(pass_ref, pass_ref);
        std::expected<compiled_graph, graph_error> compile(compile_options = {});
    };
} // namespace deren::engine::render_graph
