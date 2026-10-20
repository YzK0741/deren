#include "vk_test.h"

#include <array>
#include <cstring>
#include <memory>
#include <span>
import deren.utility.shared;
import deren.promise.rhi;
import deren.engine.subtilis.builder;
import deren.engine.subtilis.upload;
#include "probe_backend.hpp"
namespace sub = deren::engine::subtilis;
namespace rhi = deren::promise::rhi;
namespace {
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
    auto device = deren::vk_test::probe_make_core({}, rhi::abi_version, true);
    auto original = source();
    CHECK(original && original->statistics().depth > 0);
    if (!original) {
        return deren::vk_test::finish("test_subtilis_upload");
    }
    CHECK(!sub::upload_geometry({}, original));
    CHECK(!sub::upload_geometry(device, {}));
    auto uploaded = sub::upload_geometry(device, original);
    CHECK(uploaded);
    if (!uploaded) {
        return deren::vk_test::finish("test_subtilis_upload");
    }
    auto const& generation = **uploaded;
    CHECK(&generation.source() == original.get());
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
    std::weak_ptr<sub::built_geometry const> weak = original;
    original.reset();
    CHECK(!weak.expired());
    uploaded->reset();
    CHECK(weak.expired());
    return deren::vk_test::finish("test_subtilis_upload");
}
