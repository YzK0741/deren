#include "../../tools/subtilis/commands.hpp"
#include "../../tools/subtilis/package_limits.hpp"
#include "subtilis_dag_fixtures.hpp"
#include "vk_test.h"

#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <yyjson.h>
namespace tool = deren::tools::subtilis;
namespace sub = deren::asset::subtilis;
#include "subtilis_document_fixtures.hpp"
using namespace subtilis_document_fixture;
int main() {
    CHECK(!tool::package_detail::glb_length(UINT64_MAX, 0, {}));
    CHECK(!tool::package_detail::glb_length(20, (uint64_t(1) << 32) + 1, {}));
    CHECK(!tool::package_detail::glb_length(UINT32_MAX - 22u, 0, {}));
    auto boundary = tool::package_detail::glb_length(UINT32_MAX - 23u, 0, {});
    CHECK(boundary && *boundary == UINT32_MAX - 3u);
    sub::limits tiny_limit;
    tiny_limit.max_input_bytes = 32;
    CHECK(!tool::package_detail::glb_length(20, 0, tiny_limit));
    auto dir = std::filesystem::current_path() / tool::path(deren::utility::vessel::format("subtilis-converter-{}", std::chrono::steady_clock::now().time_since_epoch().count()).c_str());
    std::error_code ec;
    std::filesystem::create_directory(dir, ec);
    CHECK(!ec);
    auto f = make_fixture(subtilis_fixture::cross_dag().source);
    write(dir / "input.glb", glb(f.json, f.bin));
    auto input = (dir / "input.glb").generic_string(), cooked = (dir / "cooked.glb").generic_string(), report = (dir / "report.json").generic_string();
    std::array<std::string_view, 8> build_args{"build", input, "-o", cooked, "--threads", "1", "--report", report};
    auto parsed = tool::parse_arguments(build_args);
    CHECK(parsed);
    if (!parsed) {
        std::filesystem::remove_all(dir, ec);
        return deren::vk_test::finish("subtilis_converter");
    }
    CHECK(tool::run(*parsed) == 0);
    CHECK(std::filesystem::exists(dir / "report.json"));
    {
        std::ifstream in(dir / "report.json", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), {});
        auto* r = yyjson_read(text.data(), text.size(), 0);
        CHECK(r);
        if (r) {
            auto* root = yyjson_doc_get_root(r);
            CHECK(yyjson_get_uint(yyjson_obj_get(root, "converted")) == 1);
            CHECK(yyjson_get_uint(yyjson_obj_get(root, "skipped")) == 0);
            auto* row = yyjson_arr_get_first(yyjson_obj_get(root, "primitives"));
            CHECK(yyjson_get_uint(yyjson_obj_get(row, "sourceTriangles")) == 24);
            yyjson_doc_free(r);
        }
    }
    std::array<std::string_view, 2> inspect_args{"inspect", cooked};
    auto inspect = tool::parse_arguments(inspect_args);
    CHECK(inspect && tool::run(*inspect) == 0);
    std::array<std::string_view, 3> validate_args{"validate", cooked, "--full"};
    auto validate = tool::parse_arguments(validate_args);
    CHECK(validate && tool::run(*validate) == 0);
    auto doc = tool::read_document(dir / "cooked.glb", {});
    CHECK(doc);
    if (doc) {
        auto raw = tool::payload_bytes(*doc, {0, 0});
        CHECK(raw);
        if (raw) {
            auto p = sub::decode(*raw, {});
            CHECK(p && !p->groups.empty());
        }
    }
    unsigned n = 0;
    for (auto threshold : {"0", "0.01", "100"}) {
        auto out = (dir / tool::path(deren::utility::vessel::format("cut-{}.glb", n++).c_str())).generic_string();
        std::array<std::string_view, 6> cut_args{"export-cut", cooked, "--error", threshold, "-o", out};
        auto cut = tool::parse_arguments(cut_args);
        CHECK(cut && tool::run(*cut) == 0);
        auto preview = tool::read_document(tool::path(out), {});
        CHECK(preview);
        if (preview) {
            auto json = tool::serialize_json(*preview);
            CHECK(json && json->find("DEREN_subtilis") == sub::string::npos);
            auto geometry = tool::extract_geometry(*preview, {0, 0});
            CHECK(geometry);
            if (geometry && doc) {
                auto raw = tool::payload_bytes(*doc, {0, 0});
                auto source = tool::extract_geometry(*doc, {0, 0});
                CHECK(raw && source);
                if (raw && source) {
                    auto p = sub::decode(*raw, {});
                    CHECK(p);
                    if (p) {
                        auto a = sub::make_asset(std::move(*source), std::move(*p), {});
                        CHECK(a);
                        if (a) {
                            sub::cut_parameters parameters;
                            parameters.threshold = std::strtof(threshold, nullptr);
                            auto selected = sub::select_cut(*a, parameters);
                            CHECK(selected);
                            sub::vector<uint32_t> expected;
                            if (selected)
                                for (auto id : *selected) {
                                    auto const& c = a->data().clusters[id];
                                    auto window = std::span(a->data().indices).subspan(c.first_index, c.index_count);
                                    expected.insert(expected.end(), window.begin(), window.end());
                                }
                            CHECK(geometry->indices == expected);
                            CHECK(geometry->positions == a->geometry().positions);
                            CHECK(geometry->normals == a->geometry().normals);
                            CHECK(geometry->uv0 == a->geometry().uv0);
                        }
                    }
                }
            }
        }
    }
    auto skipped = f;
    replace(skipped.json, "\"NORMAL\":1,", "");
    write(dir / "ineligible.glb", glb(skipped.json, skipped.bin));
    auto ineligible = (dir / "ineligible.glb").generic_string();
    auto missing_out = (dir / "ineligible-cooked.glb").generic_string();
    std::array<std::string_view, 4> no_targets{"build", ineligible, "-o", missing_out};
    auto no_targets_command = tool::parse_arguments(no_targets);
    CHECK(no_targets_command && tool::run(*no_targets_command) == 4);
    CHECK(!std::filesystem::exists(dir / "ineligible-cooked.glb"));
    auto mixed_json = edit_json(f.json, [](auto* d, auto* root) {
        auto* primitives = yyjson_mut_obj_get(yyjson_mut_arr_get_first(yyjson_mut_obj_get(root, "meshes")), "primitives");
        auto* skipped = yyjson_mut_val_mut_copy(d, yyjson_mut_arr_get_first(primitives));
        yyjson_mut_obj_remove_key(yyjson_mut_obj_get(skipped, "attributes"), "NORMAL");
        yyjson_mut_arr_append(primitives, skipped);
    });
    write(dir / "mixed.glb", glb(mixed_json, f.bin));
    auto mixed_input = (dir / "mixed.glb").generic_string();
    auto strict_output = (dir / "strict.glb").generic_string();
    std::array<std::string_view, 5> strict_args{"build", mixed_input, "-o", strict_output, "--strict"};
    auto strict_command = tool::parse_arguments(strict_args);
    CHECK(strict_command && tool::run(*strict_command) == 4);
    CHECK(!std::filesystem::exists(dir / "strict.glb"));
    auto partial_report = (dir / "partial.json").generic_string();
    std::array<std::string_view, 6> partial_args{"build", mixed_input, "-o", strict_output, "--report", partial_report};
    auto partial_command = tool::parse_arguments(partial_args);
    CHECK(partial_command && tool::run(*partial_command) == 0);
    {
        std::ifstream in(dir / "partial.json", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), {});
        auto* r = yyjson_read(text.data(), text.size(), 0);
        CHECK(r);
        if (r) {
            CHECK(yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(r), "skipped")) == 1);
            yyjson_doc_free(r);
        }
    }
    auto tiny = subtilis_fixture::cross_dag().source;
    tiny.indices.resize(3);
    auto tiny_fixture = make_fixture(tiny);
    write(dir / "tiny.glb", glb(tiny_fixture.json, tiny_fixture.bin));
    auto tiny_input = (dir / "tiny.glb").generic_string();
    auto tiny_output = (dir / "tiny-cooked.glb").generic_string();
    auto tiny_report = (dir / "tiny-report.json").generic_string();
    std::array<std::string_view, 6> tiny_args{"build", tiny_input, "-o", tiny_output, "--report", tiny_report};
    auto tiny_command = tool::parse_arguments(tiny_args);
    CHECK(tiny_command && tool::run(*tiny_command) == 0);
    {
        std::ifstream in(dir / "tiny-report.json", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), {});
        auto* r = yyjson_read(text.data(), text.size(), 0);
        CHECK(r);
        if (r) {
            CHECK(yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(r), "lod0Only")) == 1);
            yyjson_doc_free(r);
        }
    }
    std::array<std::string_view, 6> zero_threads{"build", input, "-o", cooked, "--threads", "0"};
    CHECK(!tool::parse_arguments(zero_threads));
    zero_threads[5] = "2";
    CHECK(!tool::parse_arguments(zero_threads));
    zero_threads[5] = "x";
    CHECK(!tool::parse_arguments(zero_threads));
    auto unchanged = tool::read_document(dir / "input.glb", {});
    CHECK(unchanged);
    if (unchanged)
        CHECK(!tool::payload_bytes(*unchanged, {0, 0}));
    CHECK(tool::run(*parsed) == 5); // default refuses an existing target
    auto reused_output = (dir / "reused.glb").generic_string();
    auto reused_report = (dir / "reused.json").generic_string();
    std::array<std::string_view, 6> reuse_args{"build", cooked, "-o", reused_output, "--report", reused_report};
    auto reuse = tool::parse_arguments(reuse_args);
    CHECK(reuse && tool::run(*reuse) == 0);
    if (std::filesystem::exists(dir / "reused.glb")) {
        CHECK(std::filesystem::file_size(dir / "reused.glb") == std::filesystem::file_size(dir / "cooked.glb"));
        std::ifstream in(dir / "reused.json", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), {});
        auto* r = yyjson_read(text.data(), text.size(), 0);
        CHECK(r);
        if (r) {
            CHECK(yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(r), "reused")) == 1);
            CHECK(yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(r), "converted")) == 0);
            yyjson_doc_free(r);
        }
    }
    if (reuse) {
        auto different = *reuse;
        std::get<tool::build_command>(different).options.relative_error *= .5f;
        auto mismatch_output = dir / "mismatch.glb";
        std::get<tool::build_command>(different).output = mismatch_output;
        std::get<tool::build_command>(different).report.reset();
        CHECK(tool::run(different) == 4);
        CHECK(!std::filesystem::exists(mismatch_output));
    }
    auto read_bytes = [](auto const& file) {
        std::ifstream in(file, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), {});
        sub::vector<std::byte> out;
        auto bytes = std::as_bytes(std::span(text));
        out.assign(bytes.begin(), bytes.end());
        return out;
    };
    auto original_cooked = read_bytes(dir / "cooked.glb");
    if (doc) {
        auto raw = tool::payload_bytes(*doc, {0, 0});
        CHECK(raw);
        if (raw) {
            auto p = sub::decode(*raw, {});
            CHECK(p);
            if (p) {
                p->source_hash[0] ^= std::byte{1};
                auto bad_wire = sub::encode(*p, {});
                CHECK(bad_wire && bad_wire->size() == raw->size());
                if (bad_wire && bad_wire->size() == raw->size()) {
                    auto corrupt = original_cooked;
                    auto at = std::search(corrupt.begin(), corrupt.end(), raw->begin(), raw->end());
                    CHECK(at != corrupt.end());
                    if (at != corrupt.end())
                        std::copy(bad_wire->begin(), bad_wire->end(), at);
                    write(dir / "corrupt.glb", corrupt);
                    auto corrupt_input = (dir / "corrupt.glb").generic_string();
                    auto corrupt_output = (dir / "corrupt-output.glb").generic_string();
                    std::array<std::string_view, 4> args{"build", corrupt_input, "-o", corrupt_output};
                    auto command = tool::parse_arguments(args);
                    CHECK(command && tool::run(*command) == 4);
                    CHECK(!std::filesystem::exists(dir / "corrupt-output.glb"));
                }
            }
        }
    }
    CHECK(read_bytes(dir / "cooked.glb") == original_cooked);
    auto corrupt_source = read_bytes(dir / "corrupt.glb");
    if (!corrupt_source.empty()) {
        auto json_length = get(corrupt_source, 12);
        sub::string json(reinterpret_cast<char const*>(corrupt_source.data() + 20), json_length);
        auto hidden_json = edit_json(json, [](auto* d, auto* root) {
            auto* primitives = yyjson_mut_obj_get(yyjson_mut_arr_get_first(yyjson_mut_obj_get(root, "meshes")), "primitives");
            auto* plain = yyjson_mut_val_mut_copy(d, yyjson_mut_arr_get_first(primitives));
            yyjson_mut_obj_remove_key(plain, "extensions");
            yyjson_mut_obj_remove_key(plain, "material");
            yyjson_mut_arr_append(primitives, plain);
            yyjson_mut_obj_add_str(d, yyjson_mut_arr_get_first(yyjson_mut_obj_get(root, "materials")), "alphaMode", "BLEND");
        });
        sub::vector<std::byte> bin(corrupt_source.begin() + 28 + json_length, corrupt_source.end());
        write(dir / "hidden-corrupt.glb", glb(hidden_json, bin));
        auto hidden_input = (dir / "hidden-corrupt.glb").generic_string(), hidden_output = (dir / "hidden-output.glb").generic_string();
        std::array<std::string_view, 4> hidden_args{"build", hidden_input, "-o", hidden_output};
        auto hidden_command = tool::parse_arguments(hidden_args);
        CHECK(hidden_command && tool::run(*hidden_command) == 4);
        CHECK(!std::filesystem::exists(dir / "hidden-output.glb"));
    }
    auto external_json = edit_json(f.json, [](auto* d, auto* root) {
        yyjson_mut_obj_remove_key(root, "extensions");
        yyjson_mut_obj_remove_key(root, "extensionsUsed");
        auto* buffer = yyjson_mut_arr_get_first(yyjson_mut_obj_get(root, "buffers"));
        yyjson_mut_obj_add_str(d, buffer, "uri", "geometry%20data.bin");
    });
    write(dir / "geometry data.bin", f.bin);
    {
        std::ofstream out(dir / "external.gltf", std::ios::binary);
        out << external_json.c_str();
    }
    auto external_input = (dir / "external.gltf").generic_string();
    auto external_output = (dir / "new-dir" / "external.glb").generic_string();
    std::array<std::string_view, 4> external_args{"build", external_input, "-o", external_output};
    auto external_command = tool::parse_arguments(external_args);
    CHECK(external_command && tool::run(*external_command) == 0);
    CHECK(read_bytes(dir / "geometry data.bin") == f.bin);
    auto packaged = tool::read_document(tool::path(external_output), {});
    CHECK(packaged);
    if (packaged) {
        auto geometry = tool::extract_geometry(*packaged, {0, 0});
        auto original = subtilis_fixture::cross_dag().source;
        CHECK(geometry && geometry->indices == original.indices && geometry->positions == original.positions);
    }
    sub::vector<std::byte> png;
    std::string_view png_hex = "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000b49444154789c636000020000050001a5f645400000000049454e44ae426082";
    auto hex = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (size_t i = 0; i < png_hex.size(); i += 2)
        png.push_back(std::byte(hex(png_hex[i]) * 16 + hex(png_hex[i + 1])));
    write(dir / "pixel.png", png);
    auto textured_json = edit_json(external_json, [](auto* d, auto* root) {
        auto* images = yyjson_mut_arr(d);
        auto* image = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, image, "uri", "pixel.png");
        yyjson_mut_arr_append(images, image);
        yyjson_mut_obj_add_val(d, root, "images", images);
    });
    {
        std::ofstream out(dir / "images.gltf", std::ios::binary);
        out << textured_json.c_str();
    }
    auto image_input = (dir / "images.gltf").generic_string();
    auto image_output = (dir / "images.glb").generic_string();
    std::array<std::string_view, 4> image_args{"build", image_input, "-o", image_output};
    auto image_command = tool::parse_arguments(image_args);
    CHECK(image_command && tool::run(*image_command) == 0);
    if (std::filesystem::exists(dir / "images.glb")) {
        auto bytes = read_bytes(dir / "images.glb");
        auto jl = get(bytes, 12);
        auto* j = yyjson_read(reinterpret_cast<char const*>(bytes.data() + 20), jl, 0);
        CHECK(j);
        if (j) {
            auto* root = yyjson_doc_get_root(j);
            auto* image = yyjson_arr_get_first(yyjson_obj_get(root, "images"));
            CHECK(!yyjson_obj_get(image, "uri"));
            CHECK(std::string_view(yyjson_get_str(yyjson_obj_get(image, "mimeType"))) == "image/png");
            auto* view = yyjson_arr_get(yyjson_obj_get(root, "bufferViews"), yyjson_get_uint(yyjson_obj_get(image, "bufferView")));
            auto at = 28 + jl + yyjson_get_uint(yyjson_obj_get(view, "byteOffset"));
            CHECK(yyjson_get_uint(yyjson_obj_get(view, "byteLength")) == png.size());
            CHECK(at + png.size() <= bytes.size());
            if (at + png.size() <= bytes.size())
                CHECK(std::equal(png.begin(), png.end(), bytes.begin() + at));
            yyjson_doc_free(j);
        }
    }
    auto base64 = [](std::span<std::byte const> bytes) {
        constexpr std::string_view chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        sub::string out;
        for (size_t i = 0; i < bytes.size(); i += 3) {
            uint32_t value = uint32_t(std::to_integer<unsigned>(bytes[i])) << 16;
            if (i + 1 < bytes.size())
                value |= uint32_t(std::to_integer<unsigned>(bytes[i + 1])) << 8;
            if (i + 2 < bytes.size())
                value |= std::to_integer<unsigned>(bytes[i + 2]);
            out += chars[(value >> 18) & 63];
            out += chars[(value >> 12) & 63];
            out += i + 1 < bytes.size() ? chars[(value >> 6) & 63] : '=';
            out += i + 2 < bytes.size() ? chars[value & 63] : '=';
        }
        return out;
    };
    auto image_uri = sub::string("data:image/png;base64,") + base64(png);
    auto embedded_json = edit_json(textured_json, [&](auto* d, auto* root) {
        auto* image = yyjson_mut_arr_get_first(yyjson_mut_obj_get(root, "images"));
        yyjson_mut_obj_remove_key(image, "uri");
        yyjson_mut_obj_add_strcpy(d, image, "uri", image_uri.c_str());
        auto* buffer = yyjson_mut_arr_get_first(yyjson_mut_obj_get(root, "buffers"));
        auto uri = sub::string("data:application/octet-stream;base64,") + base64(f.bin);
        yyjson_mut_obj_remove_key(buffer, "uri");
        yyjson_mut_obj_add_strcpy(d, buffer, "uri", uri.c_str());
    });
    {
        std::ofstream out(dir / "data.gltf", std::ios::binary);
        out << embedded_json.c_str();
    }
    auto data_input = (dir / "data.gltf").generic_string();
    auto data_output = (dir / "data.glb").generic_string();
    std::array<std::string_view, 4> data_args{"build", data_input, "-o", data_output};
    auto data_command = tool::parse_arguments(data_args);
    CHECK(data_command && tool::run(*data_command) == 0);
    // Two independent original buffers must merge without changing accessor IDs.
    auto original_source = subtilis_fixture::cross_dag().source;
    size_t split = original_source.positions.size() * 32;
    write(dir / "attributes.bin", sub::vector<std::byte>(f.bin.begin(), f.bin.begin() + split));
    write(dir / "indices.bin", sub::vector<std::byte>(f.bin.begin() + split, f.bin.end()));
    auto multiple_json = edit_json(external_json, [&](auto* d, auto* root) {
        auto* buffers = yyjson_mut_obj_get(root, "buffers");
        auto* first = yyjson_mut_arr_get_first(buffers);
        yyjson_mut_obj_remove_key(first, "uri");
        yyjson_mut_obj_add_str(d, first, "uri", "attributes.bin");
        yyjson_mut_set_uint(yyjson_mut_obj_get(first, "byteLength"), split);
        auto* second = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, second, "uri", "indices.bin");
        yyjson_mut_obj_add_uint(d, second, "byteLength", f.bin.size() - split);
        yyjson_mut_arr_append(buffers, second);
        auto* view = yyjson_mut_arr_get(yyjson_mut_obj_get(root, "bufferViews"), 3);
        yyjson_mut_set_uint(yyjson_mut_obj_get(view, "buffer"), 1);
        yyjson_mut_set_uint(yyjson_mut_obj_get(view, "byteOffset"), 0);
    });
    {
        std::ofstream out(dir / "multiple.gltf", std::ios::binary);
        out << multiple_json.c_str();
    }
    auto multiple_input = (dir / "multiple.gltf").generic_string(), multiple_output = (dir / "multiple.glb").generic_string();
    std::array<std::string_view, 4> multiple_args{"build", multiple_input, "-o", multiple_output};
    auto multiple_command = tool::parse_arguments(multiple_args);
    CHECK(multiple_command && tool::run(*multiple_command) == 0);
    auto multiple = tool::read_document(dir / "multiple.glb", {});
    CHECK(multiple);
    if (multiple) {
        auto geometry = tool::extract_geometry(*multiple, {0, 0});
        CHECK(geometry && geometry->positions == original_source.positions && geometry->indices == original_source.indices);
    }
    auto unknown_json = edit_json(external_json, [](auto* d, auto* root) {
        auto* used = yyjson_mut_arr(d);
        yyjson_mut_arr_add_str(d, used, "VENDOR_binary");
        yyjson_mut_obj_add_val(d, root, "extensionsUsed", used);
    });
    {
        std::ofstream out(dir / "unknown.gltf", std::ios::binary);
        out << unknown_json.c_str();
    }
    auto unknown_input = (dir / "unknown.gltf").generic_string(), unknown_output = (dir / "unknown.glb").generic_string();
    std::array<std::string_view, 4> unknown_args{"build", unknown_input, "-o", unknown_output};
    auto unknown_command = tool::parse_arguments(unknown_args);
    CHECK(unknown_command && tool::run(*unknown_command) == 5);
    CHECK(!std::filesystem::exists(dir / "unknown.glb"));
    for (auto uri : {"geometry%GGdata.bin", "https://example.com/data.bin", "geometry%00data.bin", "data:application/octet-stream;base64,===="}) {
        auto bad_json = edit_json(external_json, [&](auto* d, auto* root) {auto* b=yyjson_mut_arr_get_first(yyjson_mut_obj_get(root,"buffers"));yyjson_mut_obj_remove_key(b,"uri");yyjson_mut_obj_add_strcpy(d,b,"uri",uri); });
        {
            std::ofstream out(dir / "bad-uri.gltf", std::ios::binary);
            out << bad_json.c_str();
        }
        CHECK(!tool::read_document(dir / "bad-uri.gltf", {}));
    }
    auto overwrite_output = (dir / "overwrite.glb").generic_string();
    {
        std::ofstream old(dir / "overwrite.glb");
        old << "old target";
    }
    std::array<std::string_view, 5> overwrite_args{"build", input, "-o", overwrite_output, "--overwrite"};
    auto overwrite = tool::parse_arguments(overwrite_args);
    CHECK(overwrite && tool::run(*overwrite) == 0);
    CHECK(tool::read_document(dir / "overwrite.glb", {}));
    std::array<std::string_view, 5> inplace_args{"build", cooked, "-o", cooked, "--overwrite"};
    auto inplace = tool::parse_arguments(inplace_args);
    CHECK(inplace && tool::run(*inplace) == 5);
    CHECK(read_bytes(dir / "cooked.glb") == original_cooked);
    auto directory_output = (dir / "target-directory.glb").generic_string();
    std::filesystem::create_directory(dir / "target-directory.glb", ec);
    {
        std::ofstream marker(dir / "target-directory.glb" / "keep");
        marker << "preserve";
    }
    std::array<std::string_view, 5> directory_args{"build", input, "-o", directory_output, "--overwrite"};
    auto directory_command = tool::parse_arguments(directory_args);
    CHECK(directory_command && tool::run(*directory_command) == 5);
    CHECK(std::filesystem::exists(dir / "target-directory.glb" / "keep"));
    for (auto const& entry : std::filesystem::directory_iterator(dir))
        CHECK(!entry.path().filename().string().starts_with(".subtilis-"));
    auto api = tool::convert_file(dir / "input.glb", dir / "api.glb", {});
    CHECK(api && api->converted == 1 && api->reused == 0 && api->skipped == 0);
    auto api_reuse = tool::convert_file(dir / "api.glb", dir / "api-reused.glb", {});
    CHECK(api_reuse && api_reuse->converted == 0 && api_reuse->reused == 1);
    auto alias_report = input;
    auto alias_output = (dir / "alias-output.glb").generic_string();
    std::array<std::string_view, 7> alias_args{"build", input, "-o", alias_output, "--report", alias_report, "--overwrite"};
    auto alias = tool::parse_arguments(alias_args);
    CHECK(alias && tool::run(*alias) == 5);
    CHECK(!std::filesystem::exists(dir / "alias-output.glb"));
    auto excessive_json = edit_json(external_json, [](auto*, auto* root) {
        auto* b = yyjson_mut_arr_get_first(yyjson_mut_obj_get(root, "buffers"));
        yyjson_mut_set_uint(yyjson_mut_obj_get(b, "byteLength"), UINT64_MAX);
    });
    {
        std::ofstream out(dir / "excessive.gltf");
        out << excessive_json.c_str();
    }
    auto excessive = tool::read_document(dir / "excessive.gltf", {});
    CHECK(!excessive && excessive.error().code == sub::error_code::capacity);
#ifdef _WIN32
    auto readonly_output = (dir / "readonly.glb").generic_string();
    {
        std::ofstream out(dir / "readonly.glb");
        out << "keep original";
    }
    auto readonly_bytes = read_bytes(dir / "readonly.glb");
    std::filesystem::permissions(dir / "readonly.glb", std::filesystem::perms::owner_read, ec);
    CHECK(!ec);
    std::array<std::string_view, 5> readonly_args{"build", input, "-o", readonly_output, "--overwrite"};
    auto readonly_command = tool::parse_arguments(readonly_args);
    CHECK(readonly_command && tool::run(*readonly_command) == 5);
    CHECK(read_bytes(dir / "readonly.glb") == readonly_bytes);
    std::filesystem::permissions(dir / "readonly.glb", std::filesystem::perms::owner_all, ec);
    CHECK(!ec);
#endif
    std::filesystem::remove_all(dir, ec);
    CHECK(!ec);
    return deren::vk_test::finish("subtilis_converter");
}
