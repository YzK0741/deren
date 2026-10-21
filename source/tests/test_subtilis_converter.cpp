#include "../../tools/subtilis/commands.hpp"
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
    std::filesystem::remove_all(dir, ec);
    CHECK(!ec);
    return deren::vk_test::finish("subtilis_converter");
}
