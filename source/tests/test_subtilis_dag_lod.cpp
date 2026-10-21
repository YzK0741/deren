#include "../engine/geometry/subtilis_dag_cut_closure.hpp"
#include "vk_test.h"

#include <bit>
#include <filesystem>
#include <fstream>
#include <limits>
#include <yyjson.h>
import deren.asset.subtilis.dag;
import deren.engine.subtilis.dag_lod;
#include "subtilis_dag_fixtures.hpp"
namespace wire = deren::asset::subtilis;
namespace sub = deren::engine::subtilis;
using deren::utility::vessel::vector;
namespace {
    vector<std::byte> read(std::filesystem::path const& path) {
        std::ifstream f(path, std::ios::binary);
        std::string s((std::istreambuf_iterator<char>(f)), {});
        auto bytes = std::as_bytes(std::span(s));
        return {bytes.begin(), bytes.end()};
    }
    wire::source_geometry source(vector<std::byte> const& bytes) {
        size_t cursor = 0;
        auto u32 = [&]() {uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(std::to_integer<unsigned>(bytes[cursor++]))<<(i*8);return v; };
        wire::source_geometry s;
        CHECK(bytes.size() >= 8);
        if (bytes.size() < 8)
            return s;
        auto n = u32(), m = u32();
        CHECK(8 + uint64_t(n) * 32 + uint64_t(m) * 4 == bytes.size());
        if (8 + uint64_t(n) * 32 + uint64_t(m) * 4 != bytes.size())
            return s;
        s.positions.resize(n);
        s.normals.resize(n);
        s.uv0.resize(n);
        s.indices.resize(m);
        for (auto& row : s.positions)
            for (auto& v : row)
                v = std::bit_cast<float>(u32());
        for (auto& row : s.normals)
            for (auto& v : row)
                v = std::bit_cast<float>(u32());
        for (auto& row : s.uv0)
            for (auto& v : row)
                v = std::bit_cast<float>(u32());
        for (auto& v : s.indices)
            v = u32();
        return s;
    }
    auto translate(wire::validated_asset const& asset) {
        auto const& p = asset.data();
        vector<sub::dag_cluster> clusters;
        vector<sub::dag_group> groups;
        for (auto const& c : p.clusters) {
            deren::engine::meshlet r{};
            r.first_index = c.first_index;
            r.index_count = c.index_count;
            r.center_x = c.geometry.x;
            r.center_y = c.geometry.y;
            r.center_z = c.geometry.z;
            r.radius = c.geometry.radius;
            r.cone_cos = 0;
            clusters.push_back({r, c.producer, c.consumer});
        }
        for (auto const& g : p.groups)
            groups.push_back({{g.fine_first, g.fine_count}, {g.coarse_first, g.coarse_count}, {g.lod.x, g.lod.y, g.lod.z, g.lod.radius}, g.error});
        return sub::dag_asset::make(clusters, groups, p.references, p.leaves, p.roots, static_cast<uint32_t>(asset.geometry().indices.size()), static_cast<uint32_t>(p.indices.size()));
    }
    uint32_t comparisons = 0;
    void exercise(wire::validated_asset const& reference) {
        auto asset = translate(reference);
        CHECK(asset);
        if (!asset)
            return;
        if (reference.data().groups.size() == 4 && reference.data().clusters.size() == 10) {
            vector<bool> decisions{false, true, true, true};
            sub::dag_lod_detail::close_coarsening(*asset, decisions);
            CHECK(decisions == vector<bool>({false, true, false, false}));
            sub::dag_lod_detail::close_coarsening(*asset, decisions);
            CHECK(decisions == vector<bool>({false, true, false, false}));
        }
        sub::view_parameters view;
        view.width = 2;
        view.height = 2;
        view.view[14] = -20;
        view.projection[10] = -1;
        view.projection[14] = -.1f;
        auto compare = [&]() {
            wire::cut_parameters parameters;
            parameters.mode = wire::cut_mode::screen_error;
            parameters.model = view.model;
            parameters.view = view.view;
            parameters.projection = view.projection;
            parameters.extent = {view.width, view.height};
            parameters.near_plane = view.near_plane;
            parameters.threshold = view.target_error_px;
            if (view.force_finest || !std::isfinite(view.target_error_px) || view.target_error_px < 0) {
                parameters.threshold = 0;
                parameters.projection[0] = std::numeric_limits<float>::quiet_NaN();
            }
            auto expected = wire::select_cut(reference, parameters);
            vector<deren::engine::meshlet> out;
            auto cut = sub::select_cut(*asset, view, UINT32_MAX, out);
            CHECK(cut && expected);
            if (!cut || !expected)
                return;
            CHECK(out.size() == expected->size());
            if (out.size() == expected->size())
                for (size_t i = 0; i < out.size(); ++i) {
                    auto const& c = asset->clusters()[(*expected)[i]];
                    CHECK((sub::dag_cluster{out[i], c.producer, c.consumer} == c));
                }
            vector<deren::engine::meshlet> repeat;
            auto again = sub::select_cut(*asset, view, UINT32_MAX, repeat);
            CHECK(again && repeat.size() == out.size());
            if (repeat.size() == out.size())
                for (size_t i = 0; i < out.size(); ++i)
                    CHECK(repeat[i].first_index == out[i].first_index);
            ++comparisons;
        };
        for (float threshold : {0.f, .1f, .2f, 100.f}) {
            view.target_error_px = threshold;
            compare();
        }
        for (auto const& g : reference.data().groups) {
            view.target_error_px = g.error;
            compare();
            view.target_error_px = std::nextafter(g.error, 0.f);
            compare();
            view.target_error_px = std::nextafter(g.error, std::numeric_limits<float>::infinity());
            compare();
        }
        view.kind = sub::projection_kind::perspective;
        view.width = 1280;
        view.height = 720;
        view.projection = {1.2f, 0, 0, 0, 0, 1.6f, 0, 0, 0, 0, -1.01f, -1, 0, 0, -.101f, 0};
        uint32_t seed = 17;
        for (unsigned i = 0; i < 128; ++i) {
            seed = seed * 1664525u + 1013904223u;
            view.view[14] = -5 - float(seed % 5000);
            view.target_error_px = float(i % 16) * .125f;
            compare();
        }
        view.model[0] = 2;
        view.model[5] = .5f;
        view.model[4] = .25f;
        compare();
        view.view[14] = -.11f;
        compare();
        view.model[0] = std::numeric_limits<float>::quiet_NaN();
        compare();
        view.model = sub::identity_matrix();
        view.projection[0] = 0;
        compare();
        view.force_finest = true;
        compare();
        vector<deren::engine::meshlet> out(3);
        auto insufficient = sub::select_cut(*asset, view, 0, out);
        CHECK(!insufficient && out.empty());
    }
} // namespace
int main() {
    auto cross = subtilis_fixture::cross_dag();
    auto reference = wire::make_asset(cross.source, cross.data, {});
    CHECK(reference);
    if (reference)
        exercise(*reference);
    auto directory = std::filesystem::path(VR_TEST_SOURCE_DIR) / "source/tests/fixtures/subtilis";
    auto manifest_bytes = read(directory / "fixtures.json");
    auto* manifest = yyjson_read(reinterpret_cast<char const*>(manifest_bytes.data()), manifest_bytes.size(), 0);
    CHECK(manifest);
    if (manifest) {
        auto* fixtures = yyjson_obj_get(yyjson_doc_get_root(manifest), "fixtures");
        for (size_t i = 0; i < yyjson_arr_size(fixtures); ++i) {
            auto* entry = yyjson_arr_get(fixtures, i);
            auto name = std::string(yyjson_get_str(yyjson_obj_get(entry, "name")));
            auto* files = yyjson_obj_get(entry, "files");
            for (size_t j = 0; j < yyjson_arr_size(files); ++j) {
                auto* file = yyjson_arr_get(files, j);
                auto bytes = read(directory / yyjson_get_str(yyjson_obj_get(file, "name")));
                CHECK(bytes.size() == yyjson_get_uint(yyjson_obj_get(file, "bytes")));
                auto hash = wire::sha256(bytes);
                std::string hex;
                for (auto b : hash)
                    hex += std::format("{:02x}", std::to_integer<unsigned>(b));
                CHECK(hex == yyjson_get_str(yyjson_obj_get(file, "sha256")));
            }
            auto p = wire::decode(read(directory / (name + ".payload")), {});
            CHECK(p);
            if (!p)
                continue;
            auto a = wire::make_asset(source(read(directory / (name + ".source.bin"))), std::move(*p), {});
            CHECK(a);
            if (a && name == "cross_dag") {
                vector<uint32_t> consumers(a->data().groups.size(), wire::none);
                bool crossing = false;
                for (auto const& c : a->data().clusters) {
                    if (c.producer == wire::none || c.consumer == wire::none)
                        continue;
                    if (consumers[c.producer] != wire::none && consumers[c.producer] != c.consumer)
                        crossing = true;
                    consumers[c.producer] = c.consumer;
                }
                CHECK(crossing);
            }
            if (a)
                exercise(*a);
        }
        yyjson_doc_free(manifest);
    }
    deren::vk_test::write_line("engine cut == converter reference cut: {} cases", comparisons);
    return deren::vk_test::finish("subtilis_dag_lod");
}
