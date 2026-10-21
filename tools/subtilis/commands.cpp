#include "commands.hpp"

#include "document_internal.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
namespace deren::tools::subtilis {
    namespace {
        int diagnostic(int code, asset::error const& e) {
            std::fprintf(stderr, "deren-subtilis: %s\n", e.context.c_str());
            return code;
        }
        result<void> identity(asset::payload const& p) {
            auto allocator = json_allocator();
            auto* raw = yyjson_read_opts(const_cast<char*>(p.build_meta.data()), p.build_meta.size(), 0, &allocator, nullptr);
            std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc(raw, yyjson_doc_free);
            if (!doc)
                return fail("invalid build metadata");
            auto* root = yyjson_doc_get_root(doc.get());
            auto string_is = [](yyjson_val* v, std::string_view expected) { return yyjson_is_str(v) && std::string_view(yyjson_get_str(v)) == expected; };
            if (!string_is(yyjson_obj_get(root, "algorithm"), "subtilis-dag-v1") || !string_is(yyjson_obj_get(root, "profile"), "shared-vertices"))
                return fail("unsupported build algorithm/profile");
            auto* parameters = yyjson_obj_get(root, "parameters");
            asset::build_options options;
            auto u32 = [&](char const* name, uint32_t& out) {auto* v=yyjson_obj_get(parameters,name);if(!yyjson_is_uint(v) || yyjson_get_uint(v)>UINT32_MAX)return false;out=static_cast<uint32_t>(yyjson_get_uint(v));return true; };
            auto f32 = [&](char const* name, float& out) {auto* v=yyjson_obj_get(parameters,name);if(!yyjson_is_num(v))return false;out=static_cast<float>(yyjson_get_num(v));return std::isfinite(out); };
            if (!u32("triangleLimit", options.triangle_limit) || !u32("vertexLimit", options.vertex_limit) || !u32("targetGroupSize", options.target_group_size) || !u32("maxRounds", options.max_rounds) || !f32("targetRatio", options.target_ratio) || !f32("relativeError", options.relative_error) || !f32("normalWeight", options.normal_weight) || !f32("uvWeight", options.uv_weight))
                return fail("incomplete build parameters");
            auto digest = asset::build_digest(options);
            if (!digest || *digest != p.build_hash)
                return fail("build fingerprint mismatch");
            auto* dependencies = yyjson_obj_get(root, "dependencies");
            if (!string_is(yyjson_obj_get(dependencies, "meshoptimizer"), "9e1f07b159d3cb777f1c67ed31fc11fd117986f4") || !string_is(yyjson_obj_get(dependencies, "fastgltf"), "0.9.0") || !string_is(yyjson_obj_get(dependencies, "simdjson"), "4.6.9") || !string_is(yyjson_obj_get(dependencies, "yyjson"), "0.12.0"))
                return fail("build dependency identity mismatch");
            return {};
        }
        vector<primitive_key> asset_keys(document const& d) {
            vector<primitive_key> keys;
            auto const& a = d.state->parsed;
            for (uint32_t m = 0; m < a.meshes.size(); ++m)
                for (uint32_t p = 0; p < a.meshes[m].primitives.size(); ++p)
                    if (member(member(primitive_value(d, {m, p}), "extensions"), "DEREN_subtilis"))
                        keys.push_back({m, p});
            return keys;
        }
        result<asset::validated_asset> full_asset(document const& d, primitive_key key) {
            auto bytes = payload_bytes(d, key);
            if (!bytes)
                return std::unexpected(bytes.error());
            auto p = asset::decode(*bytes, d.state->limits);
            if (!p)
                return std::unexpected(p.error());
            auto digest = identity(*p);
            if (!digest)
                return std::unexpected(digest.error());
            auto source = extract_geometry(d, key);
            if (!source)
                return std::unexpected(source.error());
            return asset::make_asset(std::move(*source), std::move(*p), d.state->limits);
        }
        result<uint64_t> verify_file(path const& file, bool full) {
            auto d = read_document(file, {});
            if (!d)
                return std::unexpected(d.error());
            auto keys = asset_keys(*d);
            for (auto key : keys) {
                if (full) {
                    auto a = full_asset(*d, key);
                    if (!a)
                        return std::unexpected(a.error());
                } else {
                    auto bytes = payload_bytes(*d, key);
                    if (!bytes)
                        return std::unexpected(bytes.error());
                    auto p = asset::decode(*bytes, {});
                    if (!p)
                        return std::unexpected(p.error());
                    auto valid = asset::validate_structure(*p, {});
                    if (!valid)
                        return std::unexpected(valid.error());
                }
            }
            return keys.size();
        }
        result<void> write_staged(path const& target, std::span<byte const> bytes, bool verify_glb) {
            std::error_code ec;
            auto absolute = std::filesystem::absolute(target, ec);
            if (ec || absolute.filename().empty())
                return fail("invalid output path", asset::error_code::io);
            if (std::filesystem::exists(absolute, ec) || ec)
                return fail("output already exists or cannot be inspected", asset::error_code::io);
            auto parent = absolute.parent_path();
            std::filesystem::create_directories(parent, ec);
            if (ec)
                return fail("cannot create output directory", asset::error_code::io);
            static std::atomic<uint64_t> sequence{0};
            path staging;
            bool created = false;
            for (unsigned attempt = 0; attempt < 32 && !created; ++attempt) {
                auto name = utility::vessel::format(".subtilis-{}-{}", std::chrono::steady_clock::now().time_since_epoch().count(), sequence.fetch_add(1));
                staging = parent / path(name.c_str());
                created = std::filesystem::create_directory(staging, ec);
                if (ec)
                    return fail("cannot create staging directory", asset::error_code::io);
            }
            if (!created)
                return fail("cannot reserve staging directory", asset::error_code::io);
            auto temporary = staging / "artifact";
            auto cleanup = [&] {std::error_code ignored;std::filesystem::remove(temporary,ignored);std::filesystem::remove(staging,ignored); };
            {
                std::ofstream out(temporary, std::ios::binary);
                if (!out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
                    out.close();
                    cleanup();
                    return fail("output write failed", asset::error_code::io);
                }
                out.close();
                if (!out) {
                    cleanup();
                    return fail("output close failed", asset::error_code::io);
                }
            }
            if (verify_glb) {
                auto checked = verify_file(temporary, true);
                if (!checked) {
                    cleanup();
                    return std::unexpected(checked.error());
                }
            }
            auto published = publish_no_replace(temporary, absolute);
            cleanup();
            return published;
        }
        struct report_row {
            primitive_key key;
            asset::build_report report;
            uint64_t source_triangles = 0, root_triangles = 0;
        };
        string report_json(vector<report_row> const& rows, uint64_t skipped) {
            auto allocator = json_allocator();
            auto* d = yyjson_mut_doc_new(&allocator);
            auto* root = yyjson_mut_obj(d);
            yyjson_mut_doc_set_root(d, root);
            uint64_t lod0 = 0;
            for (auto const& r : rows)
                if (r.report.lod0_only)
                    ++lod0;
            yyjson_mut_obj_add_uint(d, root, "converted", rows.size());
            yyjson_mut_obj_add_uint(d, root, "skipped", skipped);
            yyjson_mut_obj_add_uint(d, root, "lod0Only", lod0);
            yyjson_mut_obj_add_uint(d, root, "threads", 1);
            auto* primitives = yyjson_mut_arr(d);
            yyjson_mut_obj_add_val(d, root, "primitives", primitives);
            for (auto const& row : rows) {
                auto* p = yyjson_mut_obj(d);
                yyjson_mut_obj_add_uint(d, p, "mesh", row.key.mesh);
                yyjson_mut_obj_add_uint(d, p, "primitive", row.key.primitive);
                yyjson_mut_obj_add_uint(d, p, "sourceTriangles", row.source_triangles);
                yyjson_mut_obj_add_uint(d, p, "rootTriangles", row.root_triangles);
                yyjson_mut_obj_add_uint(d, p, "rounds", row.report.rounds);
                yyjson_mut_obj_add_uint(d, p, "crossGroupRepartitions", row.report.cross_group_repartitions);
                yyjson_mut_obj_add_uint(d, p, "knownWorkPeakBytes", row.report.known_work_peak_bytes);
                yyjson_mut_obj_add_real(d, p, "wallMs", row.report.wall_ms);
                auto* levels = yyjson_mut_arr(d);
                yyjson_mut_obj_add_val(d, p, "levels", levels);
                for (auto const& l : row.report.levels) {
                    auto* v = yyjson_mut_obj(d);
                    yyjson_mut_obj_add_uint(d, v, "triangles", l.triangles);
                    yyjson_mut_obj_add_uint(d, v, "clusters", l.clusters);
                    yyjson_mut_obj_add_uint(d, v, "groups", l.groups);
                    yyjson_mut_obj_add_uint(d, v, "roots", l.roots);
                    yyjson_mut_arr_append(levels, v);
                }
                yyjson_mut_arr_append(primitives, p);
            }
            size_t size = 0;
            auto* text = yyjson_mut_write_opts(d, 0, &allocator, &size, nullptr);
            if (!text)
                utility::fast_panic("Subtilis report serialization failed");
            string out(text, size);
            utility::memory::deallocate(text);
            yyjson_mut_doc_free(d);
            return out;
        }
        int execute(build_command const& c) {
            std::error_code ec;
            if (std::filesystem::exists(c.output, ec) || ec)
                return diagnostic(5, asset::error{asset::error_code::io, "output already exists", {}, {}});
            if (c.report && (std::filesystem::exists(*c.report, ec) || ec))
                return diagnostic(5, asset::error{asset::error_code::io, "report already exists", {}, {}});
            auto d = read_document(c.input, {});
            if (!d)
                return diagnostic(3, d.error());
            auto keys = eligible_primitives(*d);
            if (!keys)
                return diagnostic(3, keys.error());
            uint64_t total = 0;
            for (auto const& mesh : d->state->parsed.meshes)
                total += mesh.primitives.size();
            auto skipped = total - keys->size();
            if (keys->empty() || (c.strict && skipped))
                return diagnostic(4, asset::error{asset::error_code::invalid_input, "no eligible targets, or --strict rejected skipped primitives", {}, {}});
            vector<report_row> rows;
            for (auto key : *keys) {
                auto source = extract_geometry(*d, key);
                if (!source)
                    return diagnostic(3, source.error());
                auto built = asset::build(std::move(*source), c.options, c.execution);
                if (!built)
                    return diagnostic(4, built.error());
                auto wire = asset::encode(built->asset.data(), {});
                if (!wire)
                    return diagnostic(4, wire.error());
                auto attached = attach_payload(*d, key, *wire);
                if (!attached)
                    return diagnostic(4, attached.error());
                uint64_t root_triangles = 0;
                for (auto id : built->asset.data().roots)
                    root_triangles += built->asset.data().clusters[id].index_count / 3;
                rows.push_back({key, std::move(built->report), built->asset.geometry().indices.size() / 3, root_triangles});
            }
            auto bytes = serialize_embedded_glb(*d);
            if (!bytes)
                return diagnostic(5, bytes.error());
            auto written = write_staged(c.output, *bytes, true);
            if (!written)
                return diagnostic(5, written.error());
            auto report = report_json(rows, skipped);
            if (c.report) {
                auto written_report = write_staged(*c.report, std::as_bytes(std::span(report)), false);
                if (!written_report)
                    return diagnostic(5, written_report.error());
            }
            std::printf("%s\n", report.c_str());
            return 0;
        }
        int execute(inspect_command const& c) {
            auto d = read_document(c.input, {});
            if (!d)
                return diagnostic(3, d.error());
            auto keys = asset_keys(*d);
            for (auto key : keys) {
                auto raw = payload_bytes(*d, key);
                if (!raw)
                    return diagnostic(3, raw.error());
                auto p = asset::decode(*raw, {});
                if (!p)
                    return diagnostic(3, p.error());
                auto valid = asset::validate_structure(*p, {});
                if (!valid)
                    return diagnostic(3, valid.error());
                std::printf("mesh %u primitive %u: clusters=%zu groups=%zu leaves=%zu roots=%zu\n", key.mesh, key.primitive, p->clusters.size(), p->groups.size(), p->leaves.size(), p->roots.size());
            }
            return 0;
        }
        int execute(validate_command const& c) {
            auto checked = verify_file(c.input, c.full);
            if (!checked)
                return diagnostic(3, checked.error());
            if (!*checked)
                return diagnostic(3, asset::error{asset::error_code::invalid_input, "no Subtilis payload to validate", {}, {}});
            std::printf("validated %llu payload(s), full=%s\n", static_cast<unsigned long long>(*checked), c.full ? "true" : "false");
            return 0;
        }
        int execute(export_cut_command const& c) {
            auto d = read_document(c.input, {});
            if (!d)
                return diagnostic(3, d.error());
            auto keys = asset_keys(*d);
            if (keys.empty())
                return diagnostic(3, asset::error{asset::error_code::invalid_input, "no Subtilis payload to export", {}, {}});
            for (auto key : keys) {
                auto a = full_asset(*d, key);
                if (!a)
                    return diagnostic(3, a.error());
                asset::cut_parameters parameters;
                parameters.threshold = c.error;
                auto cut = asset::select_cut(*a, parameters);
                if (!cut)
                    return diagnostic(4, cut.error());
                vector<uint32_t> indices;
                for (auto id : *cut) {
                    auto const& record = a->data().clusters[id];
                    auto corners = std::span(a->data().indices).subspan(record.first_index, record.index_count);
                    indices.insert(indices.end(), corners.begin(), corners.end());
                }
                auto replaced = replace_indices(*d, key, indices);
                if (!replaced)
                    return diagnostic(4, replaced.error());
            }
            auto bytes = serialize_embedded_glb(*d);
            if (!bytes)
                return diagnostic(5, bytes.error());
            auto written = write_staged(c.output, *bytes, true);
            if (!written)
                return diagnostic(5, written.error());
            return 0;
        }
    } // namespace
    result<command> parse_arguments(std::span<std::string_view const> args) {
        auto usage = []() { return fail("usage: build INPUT -o OUTPUT.glb [--threads 1] [--report JSON] [--strict] | inspect INPUT | validate INPUT [--full] | export-cut INPUT --error MODEL_ERROR -o OUTPUT.glb"); };
        if (args.size() < 2 || args[1].empty() || args[1].starts_with('-'))
            return usage();
        path input(args[1]);
        if (args[0] == "inspect") {
            if (args.size() != 2)
                return usage();
            return command{inspect_command{input}};
        }
        if (args[0] == "validate") {
            if (args.size() > 3 || (args.size() == 3 && args[2] != "--full"))
                return usage();
            return command{validate_command{input, args.size() == 3}};
        }
        if (args[0] != "build" && args[0] != "export-cut")
            return usage();
        build_command build;
        build.input = input;
        export_cut_command cut;
        cut.input = input;
        bool output = false, error = false;
        for (size_t i = 2; i < args.size(); ++i) {
            auto option = args[i];
            if (option == "--strict" && args[0] == "build") {
                build.strict = true;
                continue;
            }
            if (i + 1 >= args.size())
                return usage();
            auto value = args[++i];
            if (option == "-o") {
                build.output = path(value);
                cut.output = path(value);
                output = true;
            } else if (option == "--report" && args[0] == "build")
                build.report = path(value);
            else if (option == "--threads" && args[0] == "build") {
                uint32_t threads = 0;
                auto converted = std::from_chars(value.data(), value.data() + value.size(), threads);
                if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() || threads != 1)
                    return fail("only --threads 1 is available before parallel scheduling");
                build.execution.threads = threads;
            } else if (option == "--error" && args[0] == "export-cut") {
                auto converted = std::from_chars(value.data(), value.data() + value.size(), cut.error);
                if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() || !std::isfinite(cut.error) || cut.error < 0)
                    return fail("invalid model-space error");
                error = true;
            } else
                return usage();
        }
        if (!output || build.output.empty() || build.output.extension() != ".glb")
            return fail("embedded converter output must be a .glb file");
        if (args[0] == "build") {
            if (build.report && build.report->lexically_normal() == build.output.lexically_normal())
                return fail("asset and report paths must differ");
            return command{std::move(build)};
        }
        if (!error)
            return usage();
        return command{std::move(cut)};
    }
    int run(command const& c) {
        return std::visit([](auto const& item) { return execute(item); }, c);
    }
} // namespace deren::tools::subtilis
