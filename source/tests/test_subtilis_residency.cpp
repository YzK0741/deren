#include "vk_test.h"

#include <array>
#include <cstring>
#include <span>

import deren.engine.subtilis.residency;
import deren.engine.subtilis.dag_import;
import deren.engine.subtilis.builder;
import deren.asset.subtilis.dag;
import deren.asset.subtilis.pages;
#include "subtilis_dag_fixtures.hpp"

namespace sub = deren::engine::subtilis;
namespace wire = deren::asset::subtilis;
template <class T>
using vector = deren::utility::vessel::vector<T>;

namespace {
    struct table {
        std::array<sub::page_entry, 7> pages{};
        std::array<std::uint32_t, 5> dependencies{};
    };

    table small_table() {
        table t;
        t.pages[0] = {1, 0, 1, 100, 1, 0, 0, 0, 0};
        t.pages[1] = {2, 0, 1, 100, 0, 0, 0, 1, 0};
        t.pages[2] = {3, 0, 1, 100, 0, 0, 1, 1, 0};
        t.pages[3] = {4, 0, 1, 100, 0, 0, 2, 1, 0};
        t.pages[4] = {5, 0, 1, 100, 1, 0, 0, 0, 0};
        t.pages[5] = {6, 0, 1, 100, 0, 0, 3, 1, 0};
        t.pages[6] = {2, 0, 1, 100, 0, 0, 1, 0, 0};
        t.dependencies = {0, 1, 1, 4};
        return t;
    }

    sub::residency_limits budget(std::uint64_t bytes) {
        return {bytes};
    }
} // namespace

int main() {
    {
        auto t = small_table();
        auto manager = sub::make_page_manager(t.pages, t.dependencies, budget(400));
        CHECK(manager);
        if (!manager) {
            return 1;
        }
        // Pinned pages are resident before the first request.
        CHECK(manager->resident(0) && manager->resident(4));
        CHECK(manager->stats().resident_bytes == 200 && manager->stats().resident_pages == 2);
        CHECK(manager->stats().installs == 0);

        // A request installs the complete, ordered dependency closure.
        auto installed = manager->request(2);
        CHECK(installed && *installed == 2);
        CHECK(manager->resident(0) && manager->resident(1) && manager->resident(2));
        CHECK(manager->stats().resident_bytes == 400 && manager->stats().resident_pages == 4);
        CHECK(manager->stats().installs == 2);

        // page 2 is the most recent eligible victim; page 1 cannot leave while page 2 needs it.
        auto again = manager->request(3);
        CHECK(again && *again == 1);
        CHECK(manager->resident(1) && manager->resident(3) && !manager->resident(2));
        CHECK(manager->stats().resident_bytes == 400 && manager->stats().resident_pages == 4);

        // Explicit eviction is refused while a resident page still depends on the target.
        CHECK(!manager->evict(1) && manager->evict(1).error() == sub::residency_error::dependency);
        CHECK(manager->evict(3));
        CHECK(manager->stats().resident_bytes == 300 && manager->stats().resident_pages == 3);

        CHECK(!manager->evict(0) && manager->evict(0).error() == sub::residency_error::pinned);
        CHECK(!manager->request(7) && manager->request(7).error() == sub::residency_error::invalid_page);
    }
    {
        // A failed request must not leave a partial installation behind.
        auto t = small_table();
        auto manager = sub::make_page_manager(t.pages, t.dependencies, budget(250));
        CHECK(manager);
        if (manager) {
            auto before = manager->stats();
            auto result = manager->request(1);
            CHECK(!result && result.error() == sub::residency_error::capacity);
            CHECK(manager->stats().resident_bytes == before.resident_bytes && manager->stats().resident_pages == before.resident_pages);
            CHECK(!manager->resident(1));
        }
    }
    {
        auto t = small_table();
        t.pages[0].bytes = 100;
        auto manager = sub::make_page_manager(t.pages, t.dependencies, budget(150));
        CHECK(!manager && manager.error() == sub::residency_error::capacity);

        t = small_table();
        t.dependencies[0] = 7;
        CHECK(!sub::make_page_manager(t.pages, t.dependencies, budget(300)));

        t = small_table();
        t.pages[0].flags = 0;
        t.pages[1].flags = 1;
        CHECK(!sub::make_page_manager(t.pages, t.dependencies, budget(300)));

        t = small_table();
        t.dependencies[0] = 1; // page 1 -> page 1
        CHECK(!sub::make_page_manager(t.pages, t.dependencies, budget(300)));

        t = small_table();
        t.pages[0].count = 0;
        CHECK(!sub::make_page_manager(t.pages, t.dependencies, budget(300)));

        t = small_table();
        t.pages[0].bytes = 0;
        CHECK(!sub::make_page_manager(t.pages, t.dependencies, budget(300)));

        t = small_table();
        t.pages[0].flags = 2;
        CHECK(!sub::make_page_manager(t.pages, t.dependencies, budget(300)));

        t = small_table();
        t.dependencies[0] = 2; // page 1 -> page 2 and page 2 -> page 1
        CHECK(!sub::make_page_manager(t.pages, t.dependencies, budget(300)));
    }
    {
        // The manager reads the real page table from the imported asset, not a copy of the
        // offline builder's private payload.
        auto fixture = subtilis_fixture::cross_dag();
        CHECK(wire::compute_pages(fixture.data, {}));
        auto validated = wire::make_asset(fixture.source, fixture.data, {});
        CHECK(validated);
        if (validated) {
            auto encoded = wire::encode(validated->data(), {});
            CHECK(encoded);
            if (encoded) {
                auto vertex_count = static_cast<std::uint32_t>(fixture.source.positions.size());
                vector<std::uint8_t> vertices(std::size_t(vertex_count) * 80, 0);
                for (std::uint32_t i = 0; i < vertex_count; ++i) {
                    auto* vertex = vertices.data() + std::size_t(i) * 80;
                    std::memcpy(vertex, fixture.source.positions[i].data(), 12);
                    std::memcpy(vertex + 12, fixture.source.normals[i].data(), 12);
                    std::memcpy(vertex + 24, fixture.source.uv0[i].data(), 8);
                }
                auto const* index_first = reinterpret_cast<std::uint8_t const*>(fixture.source.indices.data());
                std::span<std::uint8_t const> index_bytes{index_first, fixture.source.indices.size() * sizeof(std::uint32_t)};
                sub::import_input input;
                input.payload_bytes = std::span<std::uint8_t const>{reinterpret_cast<std::uint8_t const*>(encoded->data()), encoded->size()};
                input.vertex_bytes = vertices;
                input.vertex_count = vertex_count;
                input.index_bytes = index_bytes;
                input.index_count = static_cast<std::uint32_t>(fixture.source.indices.size());
                auto imported = sub::import_payload(input);
                CHECK(imported);
                if (imported) {
                    CHECK(!imported->pages().empty());
                    CHECK(imported->pages().size() == validated->data().pages.size());
                    CHECK(imported->page_dependencies().size() == validated->data().page_dependencies.size());
                    auto shared = std::make_shared<sub::imported_asset const>(std::move(*imported));
                    auto adopted = sub::built_geometry::adopt_imported(shared);
                    CHECK(adopted);
                    if (adopted) {
                        CHECK(adopted->pages().size() == shared->pages().size());
                        CHECK(adopted->page_dependencies().size() == shared->page_dependencies().size());
                    }
                    auto manager = sub::make_page_manager(shared->pages(), shared->page_dependencies(), budget(1ull << 20));
                    CHECK(manager);
                    if (manager) {
                        auto loaded = manager->request_all();
                        CHECK(loaded);
                        std::uint64_t bytes = 0;
                        for (std::uint32_t page = 0; page < shared->pages().size(); ++page) {
                            CHECK(manager->resident(page));
                            bytes += shared->pages()[page].bytes;
                        }
                        CHECK(manager->stats().resident_bytes == bytes);
                        CHECK(manager->stats().resident_pages == shared->pages().size());
                    }
                }
            }
        }
    }
    return 0;
}
