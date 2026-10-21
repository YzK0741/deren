#include "vk_test.h"

#include <cmath>
#include <cstdint>
import deren.asset.subtilis.builder;
namespace sub = deren::asset::subtilis;
namespace bd = sub::builder_detail;
namespace {
    sub::source_geometry grid(unsigned width = 16) {
        sub::source_geometry s;
        for (unsigned y = 0; y <= width; ++y)
            for (unsigned x = 0; x <= width; ++x) {
                s.positions.push_back({float(x) / width, float(y) / width, .02f * std::sin(float(x) * .2f) * std::cos(float(y) * .2f)});
                s.normals.push_back({0, 0, 1});
                s.uv0.push_back({float(x) / width, float(y) / width});
            }
        for (unsigned y = 0; y < width; ++y)
            for (unsigned x = 0; x < width; ++x) {
                auto a = y * (width + 1) + x, b = a + 1, c = a + width + 1, d = c + 1;
                s.indices.insert(s.indices.end(), {a, b, d, a, d, c});
            }
        return s;
    }
    auto triangles(std::span<std::uint32_t const> indices) {
        sub::vector<sub::array<std::uint32_t, 3>> out;
        for (size_t i = 0; i < indices.size(); i += 3) {
            sub::array<std::uint32_t, 3> a{indices[i], indices[i + 1], indices[i + 2]}, b{a[1], a[2], a[0]}, c{a[2], a[0], a[1]};
            out.push_back(std::min({a, b, c}));
        }
        std::sort(out.begin(), out.end());
        return out;
    }
    sub::vector<bd::cluster_input> inputs(bd::frontier const& f) {
        sub::vector<bd::cluster_input> out;
        for (auto const& c : f.clusters)
            out.push_back({std::span(f.indices).subspan(c.first_index, c.index_count)});
        return out;
    }
    void clusters_sound(sub::source_geometry const& source, bd::frontier const& f) {
        for (auto const& c : f.clusters) {
            CHECK(c.index_count && c.index_count <= 255 && c.index_count % 3 == 0);
            sub::vector<std::uint32_t> unique;
            for (auto id : std::span(f.indices).subspan(c.first_index, c.index_count)) {
                CHECK(id < source.positions.size());
                unique.push_back(id);
                auto p = source.positions[id];
                CHECK(std::hypot(double(p[0]) - c.geometry.x, double(p[1]) - c.geometry.y, double(p[2]) - c.geometry.z) <= double(c.geometry.radius));
            }
            std::sort(unique.begin(), unique.end());
            unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
            CHECK(unique.size() <= 256);
        }
    }
} // namespace
int main() {
    auto source = grid();
    auto original = source.indices;
    sub::build_options options;
    auto leaf = bd::cluster_lod0(source, options);
    CHECK(leaf);
    if (!leaf)
        return deren::vk_test::finish("subtilis_dag_builder");
    CHECK(triangles(leaf->indices) == triangles(source.indices));
    CHECK(leaf->clusters.size() > 1);
    clusters_sound(source, *leaf);
    auto fine = inputs(*leaf);
    auto scale = bd::measure_scale(source);
    auto topology = bd::analyze_topology(source);
    auto partitions = bd::partition_clusters(source, fine, options, scale);
    CHECK(partitions);
    if (partitions) {
        sub::vector<std::uint32_t> all;
        for (auto const& group : *partitions) {
            CHECK(!group.empty());
            sub::vector<bd::cluster_input> subset;
            sub::vector<std::uint32_t> before;
            for (auto id : group) {
                CHECK(id < fine.size());
                all.push_back(id);
                subset.push_back(fine[id]);
                before.insert(before.end(), fine[id].indices.begin(), fine[id].indices.end());
            }
            auto r = bd::reduce_group(source, subset, options, scale, &topology);
            CHECK(r);
            if (r && r->accepted())
                CHECK(bd::boundary_of(source, r->coarse.indices, topology) == bd::boundary_of(source, before, topology));
        }
        std::sort(all.begin(), all.end());
        CHECK(all.size() == fine.size());
        for (size_t i = 0; i < all.size(); ++i)
            CHECK(all[i] == i);
    }
    auto boundary = bd::boundary_of(source, source.indices);
    CHECK(boundary.valid);
    auto reduced = bd::reduce_group(source, fine, options, scale, &topology);
    CHECK(reduced && reduced->accepted());
    if (reduced && reduced->accepted()) {
        deren::vk_test::write_line("triangles {} -> {}, clusters {} -> {}", source.indices.size() / 3, reduced->coarse.indices.size() / 3, leaf->clusters.size(), reduced->coarse.clusters.size());
        CHECK(reduced->coarse.indices.size() < source.indices.size());
        CHECK(std::isfinite(reduced->local_error) && reduced->local_error >= 0);
        clusters_sound(source, reduced->coarse);
        CHECK(bd::boundary_of(source, reduced->coarse.indices) == boundary);
        CHECK(bd::preserves_region(source, topology, boundary, reduced->coarse.indices));
    }
    CHECK(source.indices == original);
    auto scaled = source;
    for (auto& p : scaled.positions)
        for (auto& x : p)
            x *= 8;
    auto scaled_candidate = bd::reduce_group(scaled, fine, options, bd::measure_scale(scaled));
    CHECK(scaled_candidate && scaled_candidate->accepted());
    if (reduced && reduced->accepted() && scaled_candidate && scaled_candidate->accepted()) {
        CHECK(scaled_candidate->coarse.indices == reduced->coarse.indices);
        CHECK(std::abs(double(scaled_candidate->local_error) - double(reduced->local_error) * 8) < 1e-7);
    }
    auto duplicate = source;
    duplicate.indices.insert(duplicate.indices.end(), source.indices.begin(), source.indices.begin() + 3);
    auto duplicate_leaf = bd::cluster_lod0(duplicate, options);
    CHECK(duplicate_leaf);
    if (duplicate_leaf) {
        auto in = inputs(*duplicate_leaf);
        auto bad = bd::reduce_group(duplicate, in, options, bd::measure_scale(duplicate));
        CHECK(bad && !bad->accepted() && bad->reason == bd::rejection_reason::unsafe_topology);
        CHECK(triangles(duplicate_leaf->indices) == triangles(duplicate.indices));
    }
    auto seamed = grid();
    auto count = static_cast<std::uint32_t>(seamed.positions.size());
    for (std::uint32_t i = 0; i < count; ++i) {
        seamed.positions.push_back(seamed.positions[i]);
        seamed.normals.push_back({0, 1, 0});
        auto uv = seamed.uv0[i];
        seamed.uv0.push_back({uv[0] + 1, uv[1]});
    }
    for (size_t triangle = 0; triangle < seamed.indices.size() / 3; ++triangle) {
        auto cell = triangle / 2;
        if (cell % 16 >= 8)
            for (unsigned k = 0; k < 3; ++k)
                seamed.indices[triangle * 3 + k] += count;
    }
    auto seam_leaf = bd::cluster_lod0(seamed, options);
    CHECK(seam_leaf);
    if (seam_leaf) {
        auto in = inputs(*seam_leaf);
        auto r = bd::reduce_group(seamed, in, options, bd::measure_scale(seamed));
        CHECK(r);
        if (r && r->accepted())
            CHECK(bd::boundary_of(seamed, r->coarse.indices) == bd::boundary_of(seamed, seamed.indices));
    }
    // Two independent surfaces: a candidate must retain both components.
    auto disconnected = source;
    for (auto p : source.positions) {
        p[0] += 2;
        disconnected.positions.push_back(p);
    }
    disconnected.normals.insert(disconnected.normals.end(), source.normals.begin(), source.normals.end());
    disconnected.uv0.insert(disconnected.uv0.end(), source.uv0.begin(), source.uv0.end());
    for (auto id : source.indices)
        disconnected.indices.push_back(id + static_cast<std::uint32_t>(source.positions.size()));
    auto disconnected_leaf = bd::cluster_lod0(disconnected, options);
    CHECK(disconnected_leaf);
    if (disconnected_leaf) {
        auto in = inputs(*disconnected_leaf);
        auto r = bd::reduce_group(disconnected, in, options, bd::measure_scale(disconnected));
        CHECK(r);
        if (r && r->accepted())
            CHECK(bd::preserves_region(disconnected, bd::analyze_topology(disconnected), bd::boundary_of(disconnected, disconnected.indices), r->coarse.indices));
    }
    auto no_progress = source;
    no_progress.indices = {0, 1, 18};
    auto tiny = bd::cluster_lod0(no_progress, options);
    CHECK(tiny);
    if (tiny) {
        auto in = inputs(*tiny);
        auto r = bd::reduce_group(no_progress, in, options, bd::measure_scale(no_progress));
        CHECK(r && !r->accepted());
    }
    auto invalid = options;
    invalid.max_rounds = 0;
    CHECK(!bd::cluster_lod0(source, invalid));
    invalid = options;
    invalid.target_ratio = 1;
    CHECK(!bd::cluster_lod0(source, invalid));
    invalid = options;
    invalid.relative_error = -1;
    CHECK(!bd::cluster_lod0(source, invalid));
    sub::source_geometry nonmanifold;
    nonmanifold.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}};
    nonmanifold.normals.resize(5, {0, 0, 1});
    nonmanifold.uv0.resize(5, {0, 0});
    nonmanifold.indices = {0, 1, 2, 1, 0, 3, 0, 1, 4};
    auto unsafe_leaf = bd::cluster_lod0(nonmanifold, options);
    CHECK(unsafe_leaf);
    if (unsafe_leaf) {
        auto in = inputs(*unsafe_leaf);
        auto r = bd::reduce_group(nonmanifold, in, options, bd::measure_scale(nonmanifold));
        CHECK(r && !r->accepted() && r->reason == bd::rejection_reason::unsafe_topology);
        CHECK(triangles(unsafe_leaf->indices) == triangles(nonmanifold.indices));
    }
    auto nan = source;
    nan.positions[0][0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!bd::cluster_lod0(nan, options));
    auto out_of_range = source;
    out_of_range.indices[0] = UINT32_MAX;
    CHECK(!bd::cluster_lod0(out_of_range, options));
    auto built = sub::build_serial(source, options, {});
    CHECK(built);
    if (built) {
        CHECK(!built->report.lod0_only && built->report.rounds <= options.max_rounds);
        CHECK(sub::validate_structure(built->asset.data(), {}));
        auto again = sub::build_serial(source, options, {});
        CHECK(again);
        if (again) {
            auto a = sub::encode(built->asset.data(), {}), b = sub::encode(again->asset.data(), {});
            CHECK(a && b && *a == *b);
        }
        sub::cut_parameters cut;
        cut.threshold = 100;
        auto selected = sub::select_cut(built->asset, cut);
        CHECK(selected && selected->size() == built->asset.data().roots.size());
    }
    bool crosses = false;
    for (auto size : {32u, 40u, 48u, 64u}) {
        auto large = grid(size);
        auto dag = sub::build_serial(std::move(large), options, {});
        CHECK(dag);
        if (dag && dag->report.cross_group_repartitions) {
            sub::vector<sub::vector<std::uint32_t>> consumers(dag->asset.data().groups.size());
            for (auto const& c : dag->asset.data().clusters)
                if (c.producer != sub::none && c.consumer != sub::none)
                    consumers[c.producer].push_back(c.consumer);
            bool witnessed = false;
            for (auto& ids : consumers) {
                std::sort(ids.begin(), ids.end());
                ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
                if (ids.size() > 1)
                    witnessed = true;
            }
            CHECK(witnessed);
            deren::vk_test::write_line("automatic DAG witness: grid={}, groups={}, crossing producers={}", size, dag->asset.data().groups.size(), dag->report.cross_group_repartitions);
            crosses = witnessed;
            break;
        }
    }
    CHECK(crosses);
    auto single = source;
    single.indices = {0, 1, 18};
    auto unchanged = sub::build_serial(single, options, {});
    CHECK(unchanged && unchanged->report.lod0_only && unchanged->report.rejected[static_cast<size_t>(bd::rejection_reason::no_progress)] > 0);
    sub::build_limits small;
    small.max_known_work_bytes = 1;
    CHECK(!sub::build_serial(source, options, small));
    small = {};
    small.max_output_bytes = 1;
    CHECK(!sub::build_serial(source, options, small));
    sub::execution_options parallel;
    parallel.threads = 2;
    CHECK(!sub::build(source, options, parallel));
    auto one_round = options;
    one_round.max_rounds = 1;
    auto bounded = sub::build_serial(source, one_round, {});
    CHECK(bounded && bounded->report.rounds == 1 && bounded->asset.data().groups.size() > 0);
    auto islands = sub::build_serial(disconnected, options, {});
    CHECK(islands && islands->asset.data().roots.size() > 1);
    return deren::vk_test::finish("subtilis_dag_builder");
}
