#include "vk_test.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <glm/glm.hpp>
#include <memory>
import deren.engine.subtilis.dag_import;
import deren.asset.subtilis.dag;
import deren.gltf_loader;
import deren.engine.runtime;
import deren.engine.primitive;
import deren.engine.scene_tree;
import deren.engine.subtilis.selection;
import deren.engine.subtilis;
import deren.engine.subtilis.dag_lod;
import deren.promise.rhi;
#include "probe_backend.hpp"
#include "subtilis_dag_fixtures.hpp"
namespace sub = deren::engine::subtilis;
namespace wire = deren::asset::subtilis;
template <class T>
using vector = deren::utility::vessel::vector<T>;
namespace {
    struct observing_drawable : deren::gltf::drawable_iterator {
        uint32_t* observations = nullptr;
        observing_drawable() = default;
        observing_drawable(deren::gltf::scenes const& s, std::span<deren::gltf::resolved_material const> m, uint32_t& count)
            : deren::gltf::drawable_iterator(s, m)
            , observations(&count) {
        }
        observing_drawable& operator++() {
            deren::gltf::drawable_iterator::operator++();
            return *this;
        }
        deren::gltf::subtilis_payload_view get_subtilis() const {
            if (observations)
                ++*observations;
            return deren::gltf::drawable_iterator::get_subtilis();
        }
    };
    vector<uint8_t> read(std::filesystem::path const& path) {
        std::ifstream f(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(f), {}};
    }
    uint32_t u32(vector<uint8_t> const& bytes, size_t at) {
        uint32_t v = 0;
        for (unsigned i = 0; i < 4; ++i)
            v |= uint32_t(bytes[at + i]) << (i * 8);
        return v;
    }
    struct fixture {
        vector<uint8_t> payload, vertices, indices;
        uint32_t vertex_count = 0, index_count = 0;
        sub::import_input input() const {
            return {payload, 1, "shared-vertices", true, true, vertices, 64, vertex_count, indices, 4, index_count};
        }
    };
    fixture load(std::string_view name) {
        auto dir = std::filesystem::path(VR_TEST_SOURCE_DIR) / "source/tests/fixtures/subtilis";
        fixture f;
        f.payload = read(dir / (std::string(name) + ".payload"));
        auto s = read(dir / (std::string(name) + ".source.bin"));
        CHECK(s.size() >= 8);
        if (s.size() < 8)
            return f;
        f.vertex_count = u32(s, 0);
        f.index_count = u32(s, 4);
        CHECK(s.size() == 8 + uint64_t(f.vertex_count) * 32 + uint64_t(f.index_count) * 4);
        if (s.size() != 8 + uint64_t(f.vertex_count) * 32 + uint64_t(f.index_count) * 4)
            return f;
        f.vertices.resize(size_t(f.vertex_count) * 64);
        for (uint32_t i = 0; i < f.vertex_count; ++i) {
            std::memcpy(f.vertices.data() + i * 64, s.data() + 8 + i * 12, 12);
            std::memcpy(f.vertices.data() + i * 64 + 12, s.data() + 8 + f.vertex_count * 12 + i * 12, 12);
            std::memcpy(f.vertices.data() + i * 64 + 24, s.data() + 8 + f.vertex_count * 24 + i * 8, 8);
        }
        f.indices.assign(s.begin() + 8 + f.vertex_count * 32, s.end());
        return f;
    }
    void replace_payload(fixture& f, wire::payload const& p) {
        auto bytes = wire::encode(p, {});
        CHECK(bytes);
        if (bytes) {
            auto* first = reinterpret_cast<uint8_t const*>(bytes->data());
            f.payload.assign(first, first + bytes->size());
        }
    }
    fixture growing_cut() {
        auto input = subtilis_fixture::cross_dag();
        auto& p = input.data;
        auto split = p.clusters[4];
        p.clusters[4].index_count = 6;
        split.first_index += 6;
        split.index_count = 6;
        p.clusters.push_back(split);
        auto old = p.references;
        p.references.clear();
        for (auto& g : p.groups) {
            auto append = [&](uint32_t& first, uint32_t& count) {
                auto start = first, size = count;
                first = static_cast<uint32_t>(p.references.size());
                for (uint32_t i = 0; i < size; ++i) {
                    auto id = old[start + i];
                    p.references.push_back(id);
                    if (id == 4)
                        p.references.push_back(10);
                }
                count = static_cast<uint32_t>(p.references.size()) - first;
            };
            append(g.fine_first, g.fine_count);
            append(g.coarse_first, g.coarse_count);
        }
        // Same geometry, but a legal intermediate cut needs five records for four leaves.
        CHECK(wire::make_asset(input.source, p, {}));
        fixture f;
        f.vertex_count = static_cast<uint32_t>(input.source.positions.size());
        f.index_count = static_cast<uint32_t>(input.source.indices.size());
        f.vertices.resize(f.vertex_count * 64);
        for (uint32_t i = 0; i < f.vertex_count; ++i) {
            std::memcpy(f.vertices.data() + i * 64, input.source.positions[i].data(), 12);
            std::memcpy(f.vertices.data() + i * 64 + 12, input.source.normals[i].data(), 12);
            std::memcpy(f.vertices.data() + i * 64 + 24, input.source.uv0[i].data(), 8);
        }
        f.indices.resize(f.index_count * 4);
        std::memcpy(f.indices.data(), input.source.indices.data(), f.indices.size());
        replace_payload(f, p);
        return f;
    }
} // namespace
int main() {
    {
        namespace rhi = deren::promise::rhi;
        rhi::create_info options{};
        options.window_width = options.window_height = 32;
        auto core = deren::vk_test::probe_make_core(options, rhi::abi_version, true);
        deren::vk_test::probe_graphics_enabled(*core, true);
        deren::engine::scene_tree::scene scene;
        deren::engine::runtime runtime{core, options};
        runtime.set_scene(scene);
        std::array<uint8_t, 4> shader{3, 2, 35, 7};
        CHECK(runtime.make_pipeline("pbr", shader, shader, shader));
        auto loaded = deren::gltf::load_model(VR_TEST_SOURCE_DIR "/source/tests/fixtures/subtilis/cross_dag.glb", {true});
        CHECK(loaded);
        if (loaded) {
            auto materials = deren::gltf::resolve_materials(*loaded);
            uint32_t observations = 0;
            auto perform = [&]() { return runtime.import_scene(loaded->nodes_begin(), deren::gltf::scene_node_iterator{}, observing_drawable(*loaded, materials, observations), observing_drawable{}, glm::vec3(0)); };
            auto off = perform();
            CHECK(off.primitive_count == 2 && off.subtilis_imported == 0 && off.subtilis_fallback_lod0 == 0);
            CHECK(observations == 0);
            runtime.set_subtilis(true);
            auto on = perform();
            CHECK(on.subtilis_imported == 2 && on.subtilis_cache_hits == 1 && on.subtilis_fallback_lod0 == 0);
            CHECK(observations == 2);
            CHECK(scene.roots.size() == 4);
            if (scene.roots.size() == 4) {
                auto* first = static_cast<deren::engine::primitive*>(scene.roots[2].primitive_leaf.get());
                auto* second = static_cast<deren::engine::primitive*>(scene.roots[3].primitive_leaf.get());
                CHECK(first && second && first->subtilis_import && first->subtilis_import == second->subtilis_import);
                CHECK(first && second && first->subtilis_geometry && first->subtilis_geometry == second->subtilis_geometry);
                if (first && first->subtilis_geometry && first->subtilis_import) {
                    CHECK(sub::matches_generation(*first));
                    CHECK(first->meshlet_count == first->subtilis_import->lod0_clusters().size());
                    CHECK(first->draw_index_count == first->subtilis_import->lod0_index_count());
                    for (size_t i = 0; i < first->meshlets.size(); ++i)
                        CHECK(first->meshlets[i].first_index == first->subtilis_import->lod0_clusters()[i].first_index);
                    auto projection = glm::mat4(1);
                    projection[2][2] = -.01f;
                    projection[3][2] = -.001f;
                    auto camera = glm::mat4(1);
                    camera[3][2] = -20;
                    auto view = sub::make_item_view(glm::mat4(1), camera, projection, 2, 2);
                    view.selection.target_error_px = 100;
                    vector<deren::engine::meshlet> records;
                    uint32_t published = 0;
                    auto run = sub::prepare_item(*first, {}, view, records, {}, [&](auto base, auto data) {CHECK(base==first->meshlet_base);published=static_cast<uint32_t>(data.size());return true; });
                    CHECK(run.source == sub::run_source::visible && published == run.count && run.count < first->meshlet_count);
                }
            }
            auto invalid = std::make_shared<deren::gltf::subtilis_payload>();
            invalid->profile_attributes = true;
            loaded->scene[0].nodes[0].meshes[0].primitives[0].subtilis = invalid;
            auto mixed = perform();
            CHECK(mixed.subtilis_imported == 1 && mixed.subtilis_fallback_lod0 == 1 && mixed.subtilis_failures[static_cast<size_t>(sub::import_failure::invalid_payload)] == 1);
        }
        auto growth = growing_cut();
        auto imported = sub::import_payload(growth.input());
        CHECK(imported);
        if (imported) {
            deren::engine::primitive_create_info info{};
            info.subtilis_import = std::make_shared<sub::imported_asset const>(std::move(*imported));
            auto* leaf = runtime.make_primitive("pbr", info);
            CHECK(leaf && sub::matches_generation(*leaf));
            if (leaf) {
                auto projection = glm::mat4(1);
                projection[2][2] = -.01f;
                projection[3][2] = -.001f;
                auto camera = glm::mat4(1);
                camera[3][2] = -20;
                auto view = sub::make_item_view(glm::mat4(1), camera, projection, 2, 2);
                view.selection.target_error_px = .1f;
                vector<deren::engine::meshlet> records;
                auto reference = sub::select_cut(info.subtilis_import->hierarchy(), view.selection, 5, records);
                CHECK(reference && records.size() == 5);
                bool wrote = false;
                auto run = sub::prepare_item(*leaf, {}, view, records, {}, [&](auto, auto) {wrote=true;return true; });
                CHECK(!wrote && records.empty() && run.source == sub::run_source::shared && run.count == 4 && run.reason == sub::fallback_reason::capacity);
            }
        }
    }
    auto f = load("cross_dag");
    auto a = sub::import_payload(f.input());
    CHECK(a);
    if (a) {
        auto count = a->lod0_clusters().size();
        CHECK(sub::admit_table(*a, 65536 - count, 65536));
        auto no_room = sub::admit_table(*a, 65537 - count, 65536);
        CHECK(!no_room && no_room.error() == sub::import_failure::capacity_table);
        CHECK(!sub::admit_table(*a, UINT64_MAX, 65536));
        auto p = wire::decode(std::as_bytes(std::span(f.payload)), {});
        CHECK(p);
        CHECK(a->lod0_index_count() == f.index_count && a->indices().size() > f.index_count);
        CHECK(a->vertices().size() == f.vertices.size() && a->vertex_stride() == 64);
        if (p) {
            CHECK(a->lod0_clusters().size() == p->leaves.size());
            CHECK(std::equal(a->indices().begin(), a->indices().end(), p->indices.begin(), p->indices.end()));
            CHECK(a->cache_key().source_hash == p->source_hash);
            CHECK(a->build_hash() == p->build_hash);
        }
        CHECK(a->cache_key().payload_hash == wire::sha256(std::as_bytes(std::span(f.payload))));
        for (auto const& c : a->hierarchy().clusters())
            CHECK(c.record.base_vertex == 0 && c.record._pad == 0 && c.record.axis_x == 0 && c.record.axis_y == 0 && c.record.axis_z == 0 && c.record.cone_cos == 0);
        auto repeated = sub::import_payload(f.input());
        CHECK(repeated && repeated->cache_key() == a->cache_key());
        auto shared = std::make_shared<sub::imported_asset const>(std::move(*a));
        auto second = shared;
        auto before = shared->vertices()[0];
        f.vertices[0] ^= 1;
        CHECK(second->vertices()[0] == before);
        f.vertices[0] ^= 1;
    }
    auto reject = [&](sub::import_input const& input, sub::import_failure reason) {auto r=sub::import_payload(input);CHECK(!r && r.error()==reason); };
    auto bad = f.input();
    bad.version = 2;
    reject(bad, sub::import_failure::invalid_payload);
    bad = f.input();
    bad.profile = "forest";
    reject(bad, sub::import_failure::invalid_payload);
    bad = f.input();
    bad.profile_attributes = false;
    reject(bad, sub::import_failure::profile_mismatch);
    bad = f.input();
    bad.material_opaque = false;
    reject(bad, sub::import_failure::material_not_opaque);
    bad = f.input();
    bad.payload_bytes = {};
    reject(bad, sub::import_failure::invalid_payload);
    bad = f.input();
    bad.vertex_bytes = bad.vertex_bytes.first(4);
    reject(bad, sub::import_failure::source_mismatch);
    bad = f.input();
    bad.index_width = 3;
    reject(bad, sub::import_failure::source_mismatch);
    bad = f.input();
    bad.payload_bytes = bad.payload_bytes.first(bad.payload_bytes.size() / 2);
    reject(bad, sub::import_failure::invalid_payload);
    bad = f.input();
    bad.vertex_stride = 32;
    reject(bad, sub::import_failure::source_mismatch);
    bad = f.input();
    bad.vertex_count = UINT32_MAX;
    reject(bad, sub::import_failure::capacity_item);
    bad = f.input();
    bad.index_count = UINT32_MAX;
    reject(bad, sub::import_failure::capacity_item);
    vector<uint8_t> narrow(size_t(f.index_count) * 2);
    for (uint32_t i = 0; i < f.index_count; ++i) {
        auto v = static_cast<uint16_t>(u32(f.indices, size_t(i) * 4));
        std::memcpy(narrow.data() + size_t(i) * 2, &v, 2);
    }
    bad = f.input();
    bad.index_bytes = narrow;
    bad.index_width = 2;
    auto small_indices = sub::import_payload(bad);
    CHECK(small_indices);
    if (small_indices) {
        auto wide_indices = sub::import_payload(f.input());
        CHECK(wide_indices && wide_indices->cache_key() == small_indices->cache_key());
    }
    auto old_position = u32(f.vertices, 0), nan = uint32_t(0x7fc00000);
    std::memcpy(f.vertices.data(), &nan, 4);
    reject(f.input(), sub::import_failure::source_mismatch);
    std::memcpy(f.vertices.data(), &old_position, 4);
    f.vertices[0] ^= 1;
    reject(f.input(), sub::import_failure::source_mismatch);
    f.vertices[0] ^= 1;
    f.indices[0] = 255;
    f.indices[1] = 255;
    f.indices[2] = 255;
    f.indices[3] = 255;
    reject(f.input(), sub::import_failure::source_mismatch);
    f = load("cross_dag");
    auto p = wire::decode(std::as_bytes(std::span(f.payload)), {});
    CHECK(p);
    if (p) {
        p->groups[0].fine_count = 0;
        replace_payload(f, *p);
        reject(f.input(), sub::import_failure::invalid_dag);
        f = load("cross_dag");
        p = wire::decode(std::as_bytes(std::span(f.payload)), {});
        if (p) {
            p->indices[0] = UINT32_MAX;
            replace_payload(f, *p);
            reject(f.input(), sub::import_failure::source_mismatch);
        }
    }
    auto tiny = load("lod0_only");
    reject(tiny.input(), sub::import_failure::lod0_only);
    auto seam = load("seam");
    CHECK(sub::import_payload(seam.input()));
    // Syntactically valid LOD0-only wire with >4096 leaves: capacity takes
    // precedence and cannot be admitted to a packed item, even without groups.
    wire::payload large;
    large.build_meta = "{}";
    for (uint32_t i = 0; i < 4097; ++i) {
        large.indices.insert(large.indices.end(), {0, 1, 2});
        large.clusters.push_back({i * 3, 3, wire::none, wire::none, {0, 0, 0, 2}});
        large.leaves.push_back(i);
        large.roots.push_back(i);
    }
    replace_payload(tiny, large);
    reject(tiny.input(), sub::import_failure::capacity_item);
    deren::vk_test::write_line("import fallback coverage: invalid_payload/profile/material/source/DAG/LOD0-only/item capacity; table/frame policy follows in runtime tasks");
    return deren::vk_test::finish("subtilis_import");
}
