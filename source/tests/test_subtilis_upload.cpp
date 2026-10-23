#include "vk_test.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
import deren.utility.shared;
import deren.promise.rhi;
import deren.engine.subtilis.builder;
import deren.engine.subtilis.upload;
import deren.engine.primitive;
import deren.engine.runtime;
import deren.engine.scene_tree;
#include "probe_backend.hpp"
namespace sub = deren::engine::subtilis;
namespace rhi = deren::promise::rhi;
namespace {
    template <class T>
    concept forgeable_generation = requires { T({}, {}, {}, {}, {}); };
    static_assert(!forgeable_generation<sub::uploaded_geometry>);
    template <class T>
    auto bytes(std::span<T> data) {
        return std::span<std::uint8_t const>{reinterpret_cast<std::uint8_t const*>(data.data()), data.size_bytes()};
    }
    std::shared_ptr<sub::built_geometry const> source(float offset = 0) {
        deren::utility::vessel::vector<std::array<float, 8>> vertices;
        deren::utility::vessel::vector<std::uint32_t> indices;
        constexpr unsigned side = 17;
        for (unsigned y = 0; y < side; ++y) {
            for (unsigned x = 0; x < side; ++x) {
                vertices.push_back({float(x) + offset, float(y), 0, 0, 0, 1, float(x) / 16, float(y) / 16});
            }
        }
        for (unsigned y = 0; y + 1 < side; ++y) {
            for (unsigned x = 0; x + 1 < side; ++x) {
                auto a = y * side + x;
                for (auto id : {a, a + 1, a + side, a + 1, a + side + 1, a + side}) {
                    indices.push_back(id);
                }
            }
        }
        auto built = sub::build({{bytes(std::span(vertices)), 32, static_cast<unsigned>(vertices.size()), bytes(std::span(indices)), 4, 0, static_cast<unsigned>(indices.size()), 0}});
        if (!built) {
            return {};
        }
        return std::make_shared<sub::built_geometry const>(std::move(*built));
    }
} // namespace
int main() {
    {
        std::array<std::array<float, 3>, 3> positions{{{0, 0, 0}, {3, 0, 0}, {0, 4, 0}}};
        std::array<std::uint32_t, 3> indices{0, 1, 2};
        deren::engine::meshlet record{};
        record.index_count = 3;
        auto edge = sub::maximum_triangle_edge(record, bytes(std::span<std::array<float, 3>>(positions)), indices, 12u);
        CHECK(edge >= 5.0 && edge < 5.00001);
        record.index_count = 4;
        CHECK(!std::isfinite(sub::maximum_triangle_edge(record, bytes(std::span<std::array<float, 3>>(positions)), indices, 12u)));
        record.index_count = 3;
        indices[2] = 99u;
        CHECK(!std::isfinite(sub::maximum_triangle_edge(record, bytes(std::span<std::array<float, 3>>(positions)), indices, 12u)));
        indices[2] = 2u;
        positions[1][0] = std::numeric_limits<float>::quiet_NaN();
        CHECK(!std::isfinite(sub::maximum_triangle_edge(record, bytes(std::span<std::array<float, 3>>(positions)), indices, 12u)));
    }
    auto device = deren::vk_test::probe_make_core({}, rhi::abi_version, true);
    auto original = source();
    CHECK(original && original->statistics().depth > 0);
    if (!original) {
        return deren::vk_test::finish("test_subtilis_upload");
    }
    CHECK(!sub::upload_geometry({}, original));
    CHECK(!sub::upload_geometry(device, {}));
    auto empty = sub::build({});
    CHECK(empty);
    if (empty) {
        CHECK(!sub::upload_geometry(device, std::make_shared<sub::built_geometry const>(std::move(*empty))));
    }
#ifdef _WIN32
    _putenv_s("DEREN_SOFTWARE_RASTER_EDGE", "1");
#else
    setenv("DEREN_SOFTWARE_RASTER_EDGE", "1", 1);
#endif
    auto uploaded = sub::upload_geometry(device, original);
#ifdef _WIN32
    _putenv_s("DEREN_SOFTWARE_RASTER_EDGE", "");
#else
    unsetenv("DEREN_SOFTWARE_RASTER_EDGE");
#endif

    CHECK(uploaded);
    if (!uploaded) {
        return deren::vk_test::finish("test_subtilis_upload");
    }
    auto const& generation = **uploaded;
    CHECK(&generation.source() == original.get());
    for (auto const& record : original->hierarchy().clusters()) {
        auto const edge = generation.triangle_edge(record);
        CHECK(std::isfinite(edge) && edge > 0.0);
        CHECK(edge == sub::maximum_triangle_edge(record, original->vertices(), original->indices(), original->vertex_stride()));
    }
    deren::engine::meshlet missing{};
    missing.first_index = UINT32_MAX;
    CHECK(!std::isfinite(generation.triangle_edge(missing)));

    CHECK(generation.vertices()->size() == original->vertices().size());
    CHECK(generation.indices()->size() == original->indices().size_bytes());
    auto vertex_bytes = deren::vk_test::probe_buffer_initial_bytes(*device, *generation.vertices());
    auto index_bytes = deren::vk_test::probe_buffer_initial_bytes(*device, *generation.indices());
    CHECK(vertex_bytes.size() == original->vertices().size() && std::memcmp(vertex_bytes.data(), original->vertices().data(), vertex_bytes.size()) == 0);
    CHECK(index_bytes.size() == original->indices().size_bytes() && std::memcmp(index_bytes.data(), original->indices().data(), index_bytes.size()) == 0);
    CHECK(index_bytes.size() > original->lod0_index_count() * 4);
    auto second_source = source(100);
    auto second = sub::upload_geometry(device, second_source);
    CHECK(second && (*second)->vertices() != generation.vertices() && (*second)->indices() != generation.indices());
    CHECK(generation.source().statistics().source_hash != (*second)->source().statistics().source_hash);
    for (int success : {0, 1}) {
        deren::vk_test::probe_resource_failure(*device, success);
        auto failed = sub::upload_geometry(device, second_source);
        CHECK(!failed && failed.error() == rhi::error::initialization_failed);
        CHECK(generation.vertices()->size() == original->vertices().size());
    }
    deren::vk_test::probe_resource_failure(*device, -1);
    auto transient = source(200);
    std::weak_ptr<sub::built_geometry const> failed_source = transient;
    deren::vk_test::probe_resource_failure(*device, 1);
    CHECK(!sub::upload_geometry(device, transient));
    transient.reset();
    CHECK(failed_source.expired());
    deren::vk_test::probe_resource_failure(*device, -1);
    std::weak_ptr<sub::built_geometry const> weak = original;
    original.reset();
    CHECK(!weak.expired());
    uploaded->reset();
    CHECK(weak.expired());
    deren::engine::primitive_create_info explicit_source;
    explicit_source.subtilis_geometry = second_source;
    deren::engine::normal_draw_primitive leaf;
    CHECK(!leaf.bind_subtilis_geometry({}));
    CHECK(leaf.bind_subtilis_geometry(*second));
    CHECK(leaf.vertex_buffer.get() == (*second)->vertices() && leaf.index_buffer.get() == (*second)->indices());
    CHECK(leaf.vertex_buffer.lease() == leaf.index_buffer.lease());
    CHECK(leaf.draw_index_count == second_source->lod0_index_count() && leaf.index_type == rhi::index_type::uint32);
    CHECK(leaf.vertex_count == second_source->vertex_count() && leaf.vertex_stride == second_source->vertex_stride());
    CHECK(leaf.subtilis_geometry == *second && leaf.is_valid());
    CHECK(!leaf.bind_subtilis_geometry(*second));
    auto old_lease = leaf.vertex_buffer.lease();
    std::weak_ptr<sub::uploaded_geometry const> old = *second;
    second->reset();
    leaf.destroy();
    CHECK(!leaf.vertex_buffer && !leaf.index_buffer && !leaf.subtilis_geometry && !leaf.geometry_resources);
    CHECK(!old.expired());
    old_lease.reset();
    CHECK(old.expired());
    {
        rhi::create_info options{};
        options.window_width = options.window_height = 4;
        deren::vk_test::probe_graphics_enabled(*device, true);
        deren::engine::scene_tree::scene scene;
        deren::engine::runtime runtime{device, options};
        runtime.set_scene(scene);
        std::array<std::uint8_t, 4> shader{3, 2, 35, 7};
        auto pipeline = runtime.make_pipeline("upload_probe", shader, shader);
        CHECK(pipeline);
        if (pipeline) {
            // Deliberately contradictory raw metadata must never leak into this
            // generation: the supplied immutable source is authoritative.
            explicit_source.vertex_count = explicit_source.index_count = 1;
            explicit_source.vertex_stride = 1;
            explicit_source.index_type = rhi::index_type::uint16;
            auto* created = runtime.make_primitive("upload_probe", explicit_source);
            CHECK(created && created->subtilis_geometry);
            if (created) {
                CHECK(created->draw_index_count == second_source->lod0_index_count());
                CHECK(created->vertex_count == second_source->vertex_count() && created->vertex_stride == second_source->vertex_stride());
                CHECK(created->index_type == rhi::index_type::uint32 && created->index_buffer->size() == second_source->indices().size_bytes());
                CHECK(created->has_bounds && created->local_aabb_min.x == 100);
                CHECK(created->meshlet_count == second_source->hierarchy().lod0_count());
                auto lease = created->index_buffer.lease();
                deren::vk_test::probe_resource_failure(*device, 1);
                CHECK(runtime.make_primitive("upload_probe", explicit_source) == nullptr);
                deren::vk_test::probe_resource_failure(*device, -1);
                CHECK(created->index_buffer.lease() == lease && created->is_valid());
                auto raw_info = explicit_source;
                raw_info.subtilis_geometry.reset();
                raw_info.vertex_data = second_source->vertices();
                raw_info.vertex_stride = second_source->vertex_stride();
                raw_info.vertex_count = second_source->vertex_count();
                std::array<std::uint16_t, 3> raw_indices{0, 1, 2};
                raw_info.index_data = {reinterpret_cast<std::uint8_t const*>(raw_indices.data()), sizeof(raw_indices)};
                raw_info.index_count = 3;
                auto* raw = runtime.make_primitive("upload_probe", raw_info);
                CHECK(raw && !raw->subtilis_geometry && raw->index_type == rhi::index_type::uint16 && raw->draw_index_count == 3);
                CHECK(raw && raw->index_buffer->size() == sizeof(raw_indices));
            }
        }
    }
    return deren::vk_test::finish("test_subtilis_upload");
}
