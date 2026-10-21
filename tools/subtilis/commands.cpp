#include "commands.hpp"

#include "document_internal.hpp"
#include "package_internal.hpp"

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
        struct report_row {
            primitive_key key;
            asset::build_report report;
            uint64_t source_triangles = 0, root_triangles = 0;
            bool reused = false;
        };
        string report_json(vector<report_row> const& rows, uint64_t skipped) {
            auto allocator = json_allocator();
            auto* d = yyjson_mut_doc_new(&allocator);
            auto* root = yyjson_mut_obj(d);
            yyjson_mut_doc_set_root(d, root);
            uint64_t lod0 = 0, reused = 0;
            for (auto const& r : rows) {
                reused += r.reused;
                if (r.report.lod0_only)
                    ++lod0;
            }
            yyjson_mut_obj_add_uint(d, root, "converted", rows.size() - reused);
            yyjson_mut_obj_add_uint(d, root, "reused", reused);
            yyjson_mut_obj_add_uint(d, root, "skipped", skipped);
            yyjson_mut_obj_add_uint(d, root, "lod0Only", lod0);
            yyjson_mut_obj_add_uint(d, root, "threads", 1);
            auto* primitives = yyjson_mut_arr(d);
            yyjson_mut_obj_add_val(d, root, "primitives", primitives);
            for (auto const& row : rows) {
                auto* p = yyjson_mut_obj(d);
                yyjson_mut_obj_add_uint(d, p, "mesh", row.key.mesh);
                yyjson_mut_obj_add_uint(d, p, "primitive", row.key.primitive);
                yyjson_mut_obj_add_bool(d, p, "reused", row.reused);
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
        struct conversion_failure {
            asset::error detail;
            int exit_code;
        };
        std::expected<conversion_report, conversion_failure> prepare_conversion(document* d, conversion_options const& options) {
            auto keys = eligible_primitives(*d);
            if (!keys)
                return std::unexpected(conversion_failure{keys.error(), 3});
            for (auto key : asset_keys(*d)) {
                if (std::find(keys->begin(), keys->end(), key) != keys->end())
                    continue;
                auto existing = full_asset(*d, key);
                auto requested = asset::build_digest(options.build);
                if (!existing || !requested || existing->data().build_hash != *requested)
                    return std::unexpected(conversion_failure{asset::error{asset::error_code::invalid_input, "existing Subtilis payload is invalid or incompatible; rebuild from the original source", {}, {}}, 4});
            }
            uint64_t total = 0;
            for (auto const& mesh : d->state->parsed.meshes)
                total += mesh.primitives.size();
            auto skipped = total - keys->size();
            if (keys->empty() || (options.strict && skipped))
                return std::unexpected(conversion_failure{asset::error{asset::error_code::invalid_input, "no eligible targets, or --strict rejected skipped primitives", {}, {}}, 4});
            vector<report_row> rows;
            for (auto key : *keys) {
                if (member(member(primitive_value(*d, key), "extensions"), "DEREN_subtilis")) {
                    auto existing = full_asset(*d, key);
                    auto requested = asset::build_digest(options.build);
                    if (!existing || !requested || existing->data().build_hash != *requested)
                        return std::unexpected(conversion_failure{asset::error{asset::error_code::invalid_input, "existing Subtilis payload is invalid or incompatible; rebuild from the original source", {}, {}}, 4});
                    uint64_t root_triangles = 0;
                    for (auto id : existing->data().roots)
                        root_triangles += existing->data().clusters[id].index_count / 3;
                    asset::build_report reused_report;
                    reused_report.lod0_only = existing->data().groups.empty();
                    rows.push_back({key, std::move(reused_report), existing->geometry().indices.size() / 3, root_triangles, true});
                    continue;
                }
                auto source = extract_geometry(*d, key);
                if (!source)
                    return std::unexpected(conversion_failure{source.error(), 3});
                auto built = asset::build(std::move(*source), options.build, options.execution);
                if (!built)
                    return std::unexpected(conversion_failure{built.error(), 4});
                auto wire = asset::encode(built->asset.data(), {});
                if (!wire)
                    return std::unexpected(conversion_failure{wire.error(), 4});
                auto attached = attach_payload(*d, key, *wire);
                if (!attached)
                    return std::unexpected(conversion_failure{attached.error(), 4});
                uint64_t root_triangles = 0;
                for (auto id : built->asset.data().roots)
                    root_triangles += built->asset.data().clusters[id].index_count / 3;
                rows.push_back({key, std::move(built->report), built->asset.geometry().indices.size() / 3, root_triangles});
            }
            conversion_report report;
            report.skipped = skipped;
            for (auto const& row : rows) {
                report.reused += row.reused;
                report.lod0_only += row.report.lod0_only;
            }
            report.converted = rows.size() - report.reused;
            report.json = report_json(rows, skipped);
            return report;
        }
        int execute(build_command const& c) {
            std::error_code ec;
            bool const package = c.format == output_format::gltf;
            path const target = package ? package_detail::package_dir_for_cli_output(c.output) : c.output;
            if (target.empty())
                return diagnostic(5, asset::error{asset::error_code::io, "invalid package output path", {}, {}});
            if (same_path(c.input, target) || (c.report && (same_path(c.input, *c.report) || same_path(target, *c.report))))
                return diagnostic(5, asset::error{asset::error_code::io, "input, output and report paths must differ", {}, {}});
            if (package && c.report && package_detail::path_inside(*c.report, target))
                return diagnostic(5, asset::error{asset::error_code::io, "report file cannot live inside the package directory", {}, {}});
            if ((std::filesystem::exists(target, ec) && !c.overwrite) || ec)
                return diagnostic(5, asset::error{asset::error_code::io, "output already exists", {}, {}});
            if (c.report && ((std::filesystem::exists(*c.report, ec) && !c.overwrite) || ec))
                return diagnostic(5, asset::error{asset::error_code::io, "report already exists", {}, {}});
            auto d = read_document(c.input, {});
            if (!d)
                return diagnostic(3, d.error());
            conversion_options options{c.options, c.execution, {c.format, c.overwrite}, c.strict};
            auto prepared = prepare_conversion(&*d, options);
            if (!prepared)
                return diagnostic(prepared.error().exit_code, prepared.error().detail);
            auto written = write_package(*d, target, {c.format, c.overwrite});
            if (!written)
                return diagnostic(5, written.error());
            auto report = std::move(prepared->json);
            if (c.report) {
                auto written_report = write_bytes_staged(*c.report, std::as_bytes(std::span(report)), false, c.overwrite);
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
            std::error_code ec;
            bool const package = c.format == output_format::gltf;
            path const target = package ? package_detail::package_dir_for_cli_output(c.output) : c.output;
            if (target.empty())
                return diagnostic(5, asset::error{asset::error_code::io, "invalid package output path", {}, {}});
            if (same_path(c.input, target))
                return diagnostic(5, asset::error{asset::error_code::io, "source file cannot be overwritten", {}, {}});
            if ((std::filesystem::exists(target, ec) && !c.overwrite) || ec)
                return diagnostic(5, asset::error{asset::error_code::io, "output already exists", {}, {}});
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
            auto written = write_package(*d, target, {c.format, c.overwrite});
            if (!written)
                return diagnostic(5, written.error());
            return 0;
        }
    } // namespace
    result<conversion_report> convert_file(path const& input, path const& output, conversion_options const& options) {
        path const target = options.write.format == output_format::gltf ? package_detail::package_dir_for_cli_output(output) : output;
        if (target.empty())
            return fail("invalid package output path", asset::error_code::io);
        if (same_path(input, target) || (options.write.format == output_format::gltf && package_detail::path_inside(input, target)))
            return fail("source file cannot be overwritten", asset::error_code::io);
        std::error_code ec;
        if ((std::filesystem::exists(target, ec) && !options.write.overwrite) || ec)
            return fail("output already exists or cannot be inspected", asset::error_code::io);
        auto d = read_document(input, {});
        if (!d)
            return std::unexpected(d.error());
        auto report = prepare_conversion(&*d, options);
        if (!report)
            return std::unexpected(report.error().detail);
        auto written = write_package(*d, target, options.write);
        if (!written)
            return std::unexpected(written.error());
        return std::move(*report);
    }
    result<command> parse_arguments(std::span<std::string_view const> args) {
        auto usage = []() { return fail("usage: build INPUT -o OUTPUT.{glb|gltf} [--format glb|gltf] [--threads 1] [--report JSON] [--strict] [--overwrite] | inspect INPUT | validate INPUT [--full] | export-cut INPUT --error MODEL_ERROR -o OUTPUT.{glb|gltf} [--format glb|gltf] [--overwrite]"); };
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
        output_format format = output_format::glb;
        bool output = false, error = false;
        for (size_t i = 2; i < args.size(); ++i) {
            auto option = args[i];
            if (option == "--overwrite") {
                build.overwrite = true;
                cut.overwrite = true;
                continue;
            }
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
            } else if (option == "--format") {
                if (value == "glb")
                    format = output_format::glb;
                else if (value == "gltf")
                    format = output_format::gltf;
                else
                    return fail("unknown output format (expected glb or gltf)");
            } else if (option == "--error" && args[0] == "export-cut") {
                auto converted = std::from_chars(value.data(), value.data() + value.size(), cut.error);
                if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() || !std::isfinite(cut.error) || cut.error < 0)
                    return fail("invalid model-space error");
                error = true;
            } else
                return usage();
        }
        if (!output || build.output.empty() || cut.output.empty())
            return usage();
        auto const output_ok = [&](path const& output_path) {
            if (format == output_format::gltf)
                return output_path.extension() == ".gltf" && !package_detail::package_dir_for_cli_output(output_path).empty();
            return output_path.extension() == ".glb";
        };
        if (!output_ok(build.output))
            return fail(format == output_format::gltf ? "glTF package output must be a .gltf path (its stem names the package directory)" : "embedded converter output must be a .glb file");
        build.format = cut.format = format;
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
