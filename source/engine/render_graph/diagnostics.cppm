module;
#include <cstdint>
export module deren.engine.render_graph:diagnostics;
export import :types;
export namespace deren::engine::render_graph {
    inline constexpr uint32_t diagnostic_schema_version = 1;
    [[nodiscard]] deren::utility::vessel::string dump_json(compiled_graph const&);
    [[nodiscard]] deren::utility::vessel::string dump_json(graph_error const&);
    [[nodiscard]] deren::utility::vessel::string dump_text(compiled_graph const&);
    [[nodiscard]] deren::utility::vessel::string dump_text(graph_error const&);
} // namespace deren::engine::render_graph
