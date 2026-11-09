// The streaming page image's real-device contract: an imported asset's encoded page table must
// become one arena buffer, a page table whose state starts at the pinned set, and an empty bounded
// request buffer. The arena is checked against the wire record layout, not against the engine's
// padded DAG structs.
#include "vk_test.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

import deren.engine.backend_loader;
import deren.engine.subtilis;
import deren.engine.subtilis.builder;
import deren.engine.subtilis.dag_import;
import deren.engine.subtilis.page_streaming;
import deren.engine.subtilis.upload;
import deren.asset.subtilis.dag;
import deren.asset.subtilis.pages;
import deren.promise.rhi;

#include "subtilis_dag_fixtures.hpp"

namespace rhi = deren::promise::rhi;
namespace sub = deren::engine::subtilis;
namespace engine = deren::engine;
namespace wire = deren::asset::subtilis;

namespace {
    struct wire_page_cluster {
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        std::uint32_t producer = 0;
        std::uint32_t consumer = 0;
        float x = 0, y = 0, z = 0, radius = 0;
    };
    static_assert(sizeof(wire_page_cluster) == 32u);
    struct wire_page_group {
        std::uint32_t fine_first = 0;
        std::uint32_t fine_count = 0;
        std::uint32_t coarse_first = 0;
        std::uint32_t coarse_count = 0;
        float x = 0, y = 0, z = 0, radius = 0;
        float error = 0;
        std::array<std::byte, 12> padding{};
    };
    static_assert(sizeof(wire_page_group) == 48u);

    std::shared_ptr<sub::built_geometry const> make_paged_geometry() {
        auto fixture = subtilis_fixture::cross_dag();
        if (!wire::compute_pages(fixture.data, {})) {
            return nullptr;
        }
        auto validated = wire::make_asset(fixture.source, fixture.data, {});
        if (!validated) {
            return nullptr;
        }
        auto encoded = wire::encode(validated->data(), {});
        if (!encoded) {
            return nullptr;
        }
        auto const vertex_count = static_cast<std::uint32_t>(fixture.source.positions.size());
        std::vector<std::uint8_t> vertices(std::size_t(vertex_count) * 80, 0);
        for (std::uint32_t i = 0; i < vertex_count; ++i) {
            auto* vertex = vertices.data() + std::size_t(i) * 80;
            std::memcpy(vertex, fixture.source.positions[i].data(), 12);
            std::memcpy(vertex + 12, fixture.source.normals[i].data(), 12);
            std::memcpy(vertex + 24, fixture.source.uv0[i].data(), 8);
        }
        auto const* index_first = reinterpret_cast<std::uint8_t const*>(fixture.source.indices.data());
        sub::import_input input;
        input.payload_bytes = std::span<std::uint8_t const>{reinterpret_cast<std::uint8_t const*>(encoded->data()), encoded->size()};
        input.vertex_bytes = vertices;
        input.vertex_count = vertex_count;
        input.index_bytes = std::span<std::uint8_t const>{index_first, fixture.source.indices.size() * sizeof(std::uint32_t)};
        input.index_count = static_cast<std::uint32_t>(fixture.source.indices.size());
        auto imported = sub::import_payload(input);
        if (!imported) {
            return nullptr;
        }
        auto adopted = sub::built_geometry::adopt_imported(std::make_shared<sub::imported_asset const>(std::move(*imported)));
        if (!adopted) {
            return nullptr;
        }
        return std::make_shared<sub::built_geometry const>(std::move(*adopted));
    }

    template <class T>
    bool matches(std::byte const* arena, std::span<T const> source, std::uint32_t first, std::uint32_t count, std::uint32_t offset) {
        if (first > source.size() || count > source.size() - first) {
            return false;
        }
        return std::memcmp(arena + offset, source.data() + first, std::size_t(count) * sizeof(T)) == 0;
    }
} // namespace

int main() {
    rhi::create_info options{};
    options.window_width = options.window_height = 64;
    options.window_visible = false;
    options.validation_layers = true;
    auto core = engine::load_api_core(options);
    if (!core) {
        return deren::vk_test::skip("test_subtilis_page_gpu", "no Vulkan device");
    }

    auto geometry = make_paged_geometry();
    CHECK(geometry && !geometry->pages().empty());
    if (!geometry || geometry->pages().empty()) {
        return deren::vk_test::finish("test_subtilis_page_gpu");
    }
    auto const* dag = geometry->hierarchy_dag();
    CHECK(dag != nullptr);
    if (!dag) {
        return deren::vk_test::finish("test_subtilis_page_gpu");
    }

    auto uploaded = sub::upload_geometry(core, geometry, rhi::to_bits(rhi::buffer_flag::device_address), sub::geometry_upload_mode::synchronous);
    CHECK(uploaded);
    if (!uploaded) {
        return deren::vk_test::finish("test_subtilis_page_gpu");
    }
    auto const& image = **uploaded;
    CHECK(image.page_count() == geometry->pages().size());
    CHECK(image.page_arena() != nullptr);
    CHECK(image.page_table() != nullptr);
    CHECK(image.page_requests() != nullptr);
    if (!image.page_arena() || !image.page_table() || !image.page_requests()) {
        return deren::vk_test::finish("test_subtilis_page_gpu");
    }
    CHECK(image.page_request_capacity() > 0);
    CHECK(image.page_table()->size() == std::uint64_t(image.page_count()) * sizeof(sub::gpu_page_table_entry));
    CHECK(image.page_requests()->size() == sizeof(sub::gpu_page_request_header) + std::uint64_t(image.page_request_capacity()) * sizeof(sub::gpu_page_request));

    auto table_bytes = image.page_table()->mapped();
    auto arena_bytes = image.page_arena()->mapped();
    CHECK(table_bytes.size() == image.page_table()->size());
    CHECK(arena_bytes.size() == image.page_arena()->size());
    if (table_bytes.size() != image.page_table()->size() || arena_bytes.size() != image.page_arena()->size()) {
        return deren::vk_test::finish("test_subtilis_page_gpu");
    }
    auto const* table = reinterpret_cast<sub::gpu_page_table_entry const*>(table_bytes.data());
    auto const* arena = arena_bytes.data();
    std::uint64_t arena_end = 0;
    for (std::uint32_t i = 0; i < image.page_count(); ++i) {
        auto const& row = table[i];
        auto const& page = geometry->pages()[i];
        CHECK(row.page == i);
        CHECK(row.section == page.section && row.first == page.first && row.count == page.count && row.bytes == page.bytes);
        bool const pinned = (page.flags & static_cast<std::uint16_t>(wire::page_flags::always_resident)) != 0;
        CHECK(row.state == static_cast<std::uint32_t>(pinned ? sub::gpu_page_state::resident : sub::gpu_page_state::absent));
        CHECK(row.flags == page.flags);
        CHECK(row.offset % 64u == 0u);
        CHECK(std::uint64_t(row.offset) + row.bytes <= arena_bytes.size());
        if (std::uint64_t(row.offset) + row.bytes > arena_bytes.size()) {
            continue;
        }
        arena_end = std::max(arena_end, std::uint64_t(row.offset) + row.bytes);
        if (page.section == 1) {
            CHECK(matches<std::uint32_t>(arena, geometry->indices(), page.first, page.count, row.offset));
        } else if (page.section == 2) {
            for (std::uint32_t c = 0; c < page.count; ++c) {
                auto const& source = dag->clusters()[page.first + c];
                auto const& encoded = reinterpret_cast<wire_page_cluster const*>(arena + row.offset)[c];
                CHECK(encoded.first_index == source.record.first_index && encoded.index_count == source.record.index_count);
                CHECK(encoded.producer == source.producer && encoded.consumer == source.consumer);
                CHECK(encoded.x == source.record.center_x && encoded.y == source.record.center_y && encoded.z == source.record.center_z);
                CHECK(encoded.radius == source.record.radius);
            }
        } else if (page.section == 3) {
            for (std::uint32_t g = 0; g < page.count; ++g) {
                auto const& source = dag->groups()[page.first + g];
                auto const& encoded = reinterpret_cast<wire_page_group const*>(arena + row.offset)[g];
                CHECK(encoded.fine_first == source.fine.first && encoded.fine_count == source.fine.count);
                CHECK(encoded.coarse_first == source.coarse.first && encoded.coarse_count == source.coarse.count);
                CHECK(encoded.x == source.lod.x && encoded.y == source.lod.y && encoded.z == source.lod.z && encoded.radius == source.lod.radius);
                CHECK(encoded.error == source.error);
                CHECK(std::all_of(encoded.padding.begin(), encoded.padding.end(), [](std::byte b) { return b == std::byte{0}; }));
            }
        } else if (page.section == 4) {
            CHECK(matches<std::uint32_t>(arena, dag->references(), page.first, page.count, row.offset));
        } else if (page.section == 5) {
            CHECK(matches<std::uint32_t>(arena, dag->leaves(), page.first, page.count, row.offset));
        } else if (page.section == 6) {
            CHECK(matches<std::uint32_t>(arena, dag->roots(), page.first, page.count, row.offset));
        }
    }
    CHECK(arena_end > 0);
    CHECK(arena_end <= arena_bytes.size());

    auto request_bytes = image.page_requests()->mapped();
    CHECK(request_bytes.size() == image.page_requests()->size());
    if (request_bytes.size() == image.page_requests()->size()) {
        auto* header = reinterpret_cast<sub::gpu_page_request_header*>(request_bytes.data());
        CHECK(header->count == 0 && header->overflow == 0 && header->frame == 0);
        CHECK(header->capacity == image.page_request_capacity());

        // The cut's request lane promotes an absent page only when the host budget admits it. Exercise
        // both sides: a zero budget must keep the request queued and mark the row requested; a later
        // pass must publish the row as resident and drain the queue.
        std::uint32_t target = image.page_count();
        for (std::uint32_t i = 0; i < image.page_count(); ++i) {
            if (table[i].state == static_cast<std::uint32_t>(sub::gpu_page_state::absent) && table[i].bytes > 0u) {
                target = i;
                break;
            }
        }
        CHECK(target != image.page_count());
        if (target != image.page_count()) {
            auto* entry = request_bytes.data() + sizeof(*header);
            auto* request = reinterpret_cast<sub::gpu_page_request*>(entry);
            header->count = 1;
            request->page = target;
            request->priority = 0;
            request->item = 0;
            request->frame = 0;

            auto const queued = image.install_requested_pages(0, 7);
            CHECK(queued.requests == 1 && queued.installed == 0 && queued.pending == 1);
            CHECK(header->count == 1 && header->frame == 7);
            CHECK(reinterpret_cast<sub::gpu_page_table_entry const*>(table_bytes.data())[target].state ==
                  static_cast<std::uint32_t>(sub::gpu_page_state::requested));

            auto const installed = image.install_requested_pages(1ull << 30, 8);
            CHECK(installed.requests == 1 && installed.installed == 1 && installed.pending == 0);
            CHECK(header->count == 0);
            CHECK(reinterpret_cast<sub::gpu_page_table_entry const*>(table_bytes.data())[target].state ==
                  static_cast<std::uint32_t>(sub::gpu_page_state::resident));
        }
    }
    deren::vk_test::write_line("page image: {} pages, {} arena bytes, table {} bytes", image.page_count(), arena_bytes.size(), table_bytes.size());
    return deren::vk_test::finish("test_subtilis_page_gpu");
}
