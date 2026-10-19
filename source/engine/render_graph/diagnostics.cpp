module;
#include <array>
#include <charconv>
#include <optional>
#include <string_view>
#include <variant>
module deren.engine.render_graph;
namespace deren::engine::render_graph {
    namespace {
        namespace vessel = deren::utility::vessel;
        template <typename E, std::size_t N>
        std::string_view enum_name(E value, char const* const (&names)[N]) noexcept {
            auto index = static_cast<uint32_t>(value);
            return index < N ? names[index] : "unknown";
        }
        std::string_view label(resource_kind value) noexcept {
            static constexpr char const* names[] = {"texture", "buffer"};
            return enum_name(value, names);
        }
        std::string_view label(initial_contents value) noexcept {
            static constexpr char const* names[] = {"undefined", "defined"};
            return enum_name(value, names);
        }
        std::string_view label(content_access value) noexcept {
            static constexpr char const* names[] = {"read", "discard_write", "preserve_write", "read_write", "invalidate"};
            return enum_name(value, names);
        }
        std::string_view label(resource_usage value) noexcept {
            static constexpr char const* names[] = {"unspecified", "sampled", "storage", "color_attachment", "depth_stencil_attachment", "depth_stencil_read", "transfer_source", "transfer_destination", "uniform_buffer", "vertex_buffer", "index_buffer", "indirect_buffer", "present"};
            return enum_name(value, names);
        }
        std::string_view label(resource_layout value) noexcept {
            static constexpr char const* names[] = {"undefined", "general", "shader_read_only", "color_attachment", "depth_stencil_attachment", "depth_stencil_read_only", "transfer_source", "transfer_destination", "present"};
            return enum_name(value, names);
        }
        std::string_view label(dependency_reason value) noexcept {
            static constexpr char const* names[] = {"value", "raw", "war", "waw", "explicit_order", "state_transition"};
            return enum_name(value, names);
        }
        std::string_view label(error_code value) noexcept {
            static constexpr char const* names[] = {"invalid_identity", "foreign_ref", "invalid_ref", "conflicting_import", "conflicting_access", "undefined_read", "undefined_export", "cycle", "frozen_builder", "capacity_exceeded", "invalid_shape", "invalid_range", "invalid_scope", "missing_scope", "missing_initial_state", "unsupported_access"};
            return enum_name(value, names);
        }
        struct writer {
            vessel::string output;
            void raw(std::string_view value) {
                output.append(value);
            }
            void number(uint64_t value) {
                char bytes[32];
                auto result = std::to_chars(bytes, bytes + sizeof(bytes), value);
                output.append(bytes, static_cast<std::size_t>(result.ptr - bytes));
            }
            void decimal_string(uint64_t value) {
                raw("\"");
                number(value);
                raw("\"");
            }
            void index(uint32_t value) {
                if (value == invalid_index) {
                    raw("null");
                } else {
                    number(value);
                }
            }
            void boolean(bool value) {
                raw(value ? "true" : "false");
            }
            void string(std::string_view text) {
                raw("\"");
                for (std::size_t i = 0; i < text.size();) {
                    auto c = static_cast<unsigned char>(text[i]);
                    if (c < 0x80) {
                        ++i;
                        switch (c) {
                        case '"':
                            raw("\\\"");
                            break;
                        case '\\':
                            raw("\\\\");
                            break;
                        case '\n':
                            raw("\\n");
                            break;
                        case '\r':
                            raw("\\r");
                            break;
                        case '\t':
                            raw("\\t");
                            break;
                        default:
                            if (c < 0x20) {
                                constexpr char hex[] = "0123456789abcdef";
                                raw("\\u00");
                                output.push_back(hex[c >> 4]);
                                output.push_back(hex[c & 15]);
                            } else {
                                output.push_back(static_cast<char>(c));
                            }
                        }
                        continue;
                    }
                    unsigned width = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3
                                                              : c >= 0xf0 && c <= 0xf4   ? 4
                                                                                         : 0;
                    bool valid = width && width <= text.size() - i;
                    for (unsigned j = 1; valid && j < width; ++j) {
                        valid = (static_cast<unsigned char>(text[i + j]) & 0xc0) == 0x80;
                    }
                    if (valid) {
                        auto second = static_cast<unsigned char>(text[i + 1]);
                        valid = !(c == 0xe0 && second < 0xa0) && !(c == 0xed && second >= 0xa0) && !(c == 0xf0 && second < 0x90) && !(c == 0xf4 && second >= 0x90);
                    }
                    if (valid) {
                        raw(text.substr(i, width));
                        i += width;
                    } else {
                        raw("\\ufffd");
                        ++i;
                    }
                }
                raw("\"");
            }
            template <typename Range, typename F>
            void array(Range const& values, F emit) {
                raw("[");
                bool first = true;
                for (auto const& value : values) {
                    if (!first) {
                        raw(",");
                    }
                    first = false;
                    emit(value);
                }
                raw("]");
            }
            template <typename T, typename F>
            void optional(std::optional<T> const& value, F emit) {
                if (value) {
                    emit(*value);
                } else {
                    raw("null");
                }
            }
            void stages(pipeline_stage value) {
                constexpr std::array names{"vertex_shader", "fragment_shader", "compute_shader", "color_output", "depth_stencil", "transfer", "vertex_input", "draw_indirect", "task_shader", "mesh_shader"};
                raw("[");
                bool first = true;
                for (unsigned bit = 0; bit < names.size(); ++bit) {
                    if (static_cast<uint32_t>(value) & (1U << bit)) {
                        if (!first) {
                            raw(",");
                        }
                        first = false;
                        string(names[bit]);
                    }
                }
                raw("]");
            }
            void state(resource_state const& value) {
                raw("{\"layout\":");
                string(label(value.layout));
                raw(",\"stages\":");
                stages(value.stages);
                raw(",\"stage_mask\":");
                number(static_cast<uint32_t>(value.stages));
                raw(",\"access\":[");
                auto bits = static_cast<uint32_t>(value.access);
                if (bits & 1) {
                    string("read");
                }
                if ((bits & 3) == 3) {
                    raw(",");
                }
                if (bits & 2) {
                    string("write");
                }
                raw("],\"access_mask\":");
                number(bits);
                raw("}");
            }
            void scope(access_scope const& value) {
                raw("{\"usage\":");
                string(label(value.usage));
                raw(",\"stages\":");
                stages(value.stages);
                raw(",\"stage_mask\":");
                number(static_cast<uint32_t>(value.stages));
                raw("}");
            }
            void range(resource_range const& value) {
                if (auto image = std::get_if<texture_range>(&value)) {
                    raw("{\"kind\":\"texture\",\"base_mip\":");
                    number(image->base_mip);
                    raw(",\"mip_count\":");
                    number(image->mip_count);
                    raw(",\"base_layer\":");
                    number(image->base_layer);
                    raw(",\"layer_count\":");
                    number(image->layer_count);
                    raw(",\"aspect_mask\":");
                    number(static_cast<uint32_t>(image->aspects));
                    raw("}");
                } else {
                    auto bytes = std::get<buffer_range>(value);
                    raw("{\"kind\":\"buffer\",\"offset\":");
                    decimal_string(bytes.offset);
                    raw(",\"size\":");
                    decimal_string(bytes.size);
                    raw("}");
                }
            }
            void shape(resource_shape const& value) {
                if (auto image = std::get_if<texture_shape>(&value)) {
                    raw("{\"kind\":\"texture\",\"mip_levels\":");
                    number(image->mip_levels);
                    raw(",\"array_layers\":");
                    number(image->array_layers);
                    raw(",\"aspect_mask\":");
                    number(static_cast<uint32_t>(image->aspects));
                    raw("}");
                } else {
                    raw("{\"kind\":\"buffer\",\"byte_size\":");
                    decimal_string(std::get<buffer_shape>(value).byte_size);
                    raw("}");
                }
            }
            void access(resolved_access const& value) {
                raw("{\"resource\":");
                index(value.resource);
                raw(",\"mode\":");
                string(label(value.mode));
                raw(",\"range\":");
                range(value.range);
                raw(",\"scope\":");
                scope(value.scope);
                raw("}");
            }
            void contents(range_contents const& value) {
                raw("{\"range\":");
                range(value.range);
                raw(",\"producer\":");
                index(value.producer);
                raw(",\"version\":");
                decimal_string(value.version);
                raw(",\"defined\":");
                boolean(value.defined);
                raw("}");
            }
            struct interval {
                std::size_t first, last;
            };
            void resource(resource_record const& value, std::size_t i, std::optional<interval> use) {
                raw("{\"index\":");
                number(i);
                raw(",\"identity\":{\"device\":");
                decimal_string(value.identity.device);
                raw(",\"object\":");
                decimal_string(value.identity.object);
                raw("},\"kind\":");
                string(label(value.kind));
                raw(",\"name\":");
                string(value.name);
                raw(",\"initial_contents\":");
                string(label(value.initial));
                raw(",\"shape\":");
                shape(value.shape);
                raw(",\"initial_state\":");
                optional(value.initial_state, [&](auto const& v) { state(v); });
                raw(",\"declared_final\":{\"last_producer\":");
                index(value.last_producer);
                raw(",\"last_version\":");
                decimal_string(value.last_version);
                raw(",\"defined\":");
                boolean(value.defined);
                raw(",\"ranges\":");
                array(value.final_ranges, [&](auto const& v) { contents(v); });
                raw("},\"use_interval\":");
                optional(use, [&](auto const& v) { raw("{\"first\":"); number(v.first); raw(",\"last\":"); number(v.last); raw("}"); });
                raw("}");
            }
            void pass(pass_record const& value, std::size_t i) {
                raw("{\"index\":");
                number(i);
                raw(",\"name\":");
                string(value.name);
                raw(",\"side_effect\":");
                boolean(value.side_effect);
                raw(",\"live\":");
                boolean(value.live);
                raw(",\"cull_reason\":");
                if (value.live) {
                    raw("null");
                } else {
                    string("not_reachable_from_outputs_or_side_effects");
                }
                raw(",\"parameters_present\":");
                boolean(static_cast<bool>(value.parameters));
                raw(",\"accesses\":");
                array(value.accesses, [&](auto const& v) { access(v); });
                raw("}");
            }
            void edge(dependency const& value) {
                raw("{\"from\":");
                index(value.from);
                raw(",\"to\":");
                index(value.to);
                raw(",\"resource\":");
                index(value.resource);
                raw(",\"reason\":");
                string(label(value.reason));
                raw(",\"range\":");
                optional(value.range, [&](auto const& v) { range(v); });
                raw("}");
            }
            void read(content_read const& value) {
                raw("{\"pass\":");
                index(value.pass);
                raw(",\"resource\":");
                index(value.resource);
                raw(",\"producer\":");
                index(value.producer);
                raw(",\"version\":");
                decimal_string(value.version);
                raw(",\"defined\":");
                boolean(value.defined);
                raw(",\"range\":");
                range(value.range);
                raw("}");
            }
            void exported(resource_export const& value) {
                raw("{\"resource\":");
                index(value.resource);
                raw(",\"range\":");
                range(value.range);
                raw("}");
            }
            void barrier(resource_barrier const& value) {
                raw("{\"resource\":");
                index(value.resource);
                raw(",\"before_pass\":");
                index(value.before_pass);
                raw(",\"range\":");
                range(value.range);
                raw(",\"before\":");
                state(value.before);
                raw(",\"after\":");
                state(value.after);
                raw(",\"layout_transition\":");
                boolean(value.layout_transition);
                raw(",\"memory_dependency\":");
                boolean(value.memory_dependency);
                raw("}");
            }
            void final_state(final_resource_state const& value) {
                raw("{\"resource\":");
                index(value.resource);
                raw(",\"range\":");
                range(value.range);
                raw(",\"state\":");
                state(value.state);
                raw("}");
            }
            void error(graph_error const& value) {
                raw("{\"schema_version\":");
                number(diagnostic_schema_version);
                raw(",\"kind\":\"graph_error\",\"code\":");
                string(label(value.code));
                raw(",\"pass\":");
                index(value.pass);
                raw(",\"resource\":");
                index(value.resource);
                raw(",\"pass_name\":");
                string(value.pass_name);
                raw(",\"resource_name\":");
                string(value.resource_name);
                raw(",\"range\":");
                optional(value.range, [&](auto const& v) { range(v); });
                raw("}");
            }
        };
        auto intervals(compiled_graph const& graph) {
            vessel::vector<std::optional<writer::interval>> result(graph.resources.size());
            for (std::size_t position = 0; position < graph.execution_order.size(); ++position) {
                auto pass = graph.execution_order[position];
                if (pass >= graph.passes.size()) {
                    continue;
                }
                for (auto const& access : graph.passes[pass].accesses) {
                    if (access.resource >= result.size()) {
                        continue;
                    }
                    auto& value = result[access.resource];
                    if (!value) {
                        value = writer::interval{position, position};
                    } else {
                        value->last = position;
                    }
                }
            }
            return result;
        }
    } // namespace
    vessel::string dump_json(compiled_graph const& graph) {
        writer w;
        auto uses = intervals(graph);
        w.raw("{\"schema_version\":");
        w.number(diagnostic_schema_version);
        w.raw(",\"kind\":\"compiled_graph\",\"synchronization_planned\":");
        w.boolean(graph.synchronization_planned);
        w.raw(",\"resources\":[");
        for (std::size_t i = 0; i < graph.resources.size(); ++i) {
            if (i) {
                w.raw(",");
            }
            w.resource(graph.resources[i], i, uses[i]);
        }
        w.raw("],\"passes\":[");
        for (std::size_t i = 0; i < graph.passes.size(); ++i) {
            if (i) {
                w.raw(",");
            }
            w.pass(graph.passes[i], i);
        }
        w.raw("],\"dependencies\":");
        w.array(graph.dependencies, [&](auto const& v) { w.edge(v); });
        w.raw(",\"reads\":");
        w.array(graph.reads, [&](auto const& v) { w.read(v); });
        w.raw(",\"execution_order\":");
        w.array(graph.execution_order, [&](auto v) { w.index(v); });
        w.raw(",\"outputs\":");
        w.array(graph.outputs, [&](auto const& v) { w.exported(v); });
        w.raw(",\"barriers\":");
        w.array(graph.barriers, [&](auto const& v) { w.barrier(v); });
        w.raw(",\"final_states\":");
        w.array(graph.final_states, [&](auto const& v) { w.final_state(v); });
        w.raw("}\n");
        return std::move(w.output);
    }
    vessel::string dump_json(graph_error const& error) {
        writer w;
        w.error(error);
        w.raw("\n");
        return std::move(w.output);
    }
    vessel::string dump_text(compiled_graph const& graph) {
        writer w;
        auto uses = intervals(graph);
        w.raw("render_graph schema=");
        w.number(diagnostic_schema_version);
        w.raw(" synchronization=");
        w.raw(graph.synchronization_planned ? "planned\n" : "not_planned\n");
        w.raw("execution_order: ");
        w.array(graph.execution_order, [&](auto v) { w.index(v); });
        w.raw("\n");
        auto line = [&](std::string_view name, std::size_t i, auto emit) { w.raw(name); w.raw("["); w.number(i); w.raw("]: "); emit(); w.raw("\n"); };
        for (std::size_t i = 0; i < graph.resources.size(); ++i) {
            line("resource", i, [&] { w.resource(graph.resources[i], i, uses[i]); });
        }
        for (std::size_t i = 0; i < graph.passes.size(); ++i) {
            line("pass", i, [&] { w.pass(graph.passes[i], i); });
        }
        auto lines = [&](std::string_view name, auto const& values, auto emit) { for (std::size_t i = 0; i < values.size(); ++i) { line(name, i, [&] { emit(values[i]); }); 
} };
        lines("dependency", graph.dependencies, [&](auto const& v) { w.edge(v); });
        lines("read", graph.reads, [&](auto const& v) { w.read(v); });
        lines("output", graph.outputs, [&](auto const& v) { w.exported(v); });
        lines("barrier", graph.barriers, [&](auto const& v) { w.barrier(v); });
        lines("final_state", graph.final_states, [&](auto const& v) { w.final_state(v); });
        return std::move(w.output);
    }
    vessel::string dump_text(graph_error const& error) {
        writer w;
        w.raw("render_graph error: ");
        w.error(error);
        w.raw("\n");
        return std::move(w.output);
    }
} // namespace deren::engine::render_graph
