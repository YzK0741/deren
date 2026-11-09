#include "vk_test.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

import deren.engine.subtilis.page_streaming;

namespace sub = deren::engine::subtilis;

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
        // A plan is pure: it names the closure and the victims, and apply() is the only mutation.
        auto t = small_table();
        auto manager = sub::make_page_manager(t.pages, t.dependencies, budget(400));
        CHECK(manager);
        if (!manager) {
            return 1;
        }
        auto plain = manager->plan_request(2);
        CHECK(plain);
        if (plain) {
            CHECK(plain->target == 2);
            CHECK(plain->installs.size() == 2 && plain->installs[0] == 1 && plain->installs[1] == 2);
            CHECK(plain->evictions.empty());
            CHECK(plain->installed_bytes == 200);
            CHECK(!manager->resident(1) && !manager->resident(2));
        }
        auto applied = manager->apply(std::move(*plain));
        CHECK(applied);
        CHECK(manager->resident(1) && manager->resident(2));
        CHECK(manager->stats().resident_bytes == 400);

        // A plan computed against an older resident set is rejected, not partially applied.
        auto stale = manager->plan_request(3);
        CHECK(stale);
        if (stale) {
            CHECK(stale->installs.size() == 1 && stale->installs[0] == 3);
            CHECK(manager->request(3));
            auto again = manager->apply(std::move(*stale));
            CHECK(!again && again.error() == sub::residency_error::invalid_page);
            CHECK(manager->resident(3));
        }
    }
    {
        auto t = small_table();
        auto manager = sub::make_page_manager(t.pages, t.dependencies, budget(400));
        CHECK(manager);
        if (manager) {
            CHECK(manager->request(2));
            auto planned = manager->plan_request(3);
            CHECK(planned);
            if (planned) {
                CHECK(planned->installs.size() == 1 && planned->installs[0] == 3);
                CHECK(planned->evictions.size() == 1 && planned->evictions[0] == 2);
                auto applied = manager->apply(std::move(*planned));
                CHECK(applied);
                CHECK(manager->resident(3) && !manager->resident(2));
            }
        }
    }
    {
        auto t = small_table();
        auto layout = sub::page_arena_layout::create(t.pages, 64);
        CHECK(layout);
        if (layout) {
            CHECK(layout->bytes() == 868);
            CHECK(layout->page_count() == t.pages.size());
            CHECK(layout->page_offset(0) == 0);
            CHECK(layout->page_offset(1) == 128);
            CHECK(layout->page_offset(2) == 256);
            CHECK(!layout->page_offset(7));
        }
        CHECK(!sub::page_arena_layout::create(t.pages, 3));
        auto empty = sub::page_arena_layout::create({});
        CHECK(empty && empty->bytes() == 0 && empty->page_count() == 0);
    }
    {
        auto t = small_table();
        auto manager = sub::make_page_manager(t.pages, t.dependencies, budget(400));
        auto layout = sub::page_arena_layout::create(t.pages, 64);
        CHECK(manager && layout);
        if (manager && layout) {
            auto table = sub::make_gpu_page_table(t.pages, *layout, *manager);
            CHECK(table);
            if (table) {
                CHECK(table->size() == t.pages.size());
                CHECK((*table)[0].state == static_cast<std::uint32_t>(sub::gpu_page_state::resident));
                CHECK((*table)[1].state == static_cast<std::uint32_t>(sub::gpu_page_state::absent));
                CHECK((*table)[1].offset == layout->page_offset(1));
            }
            auto promoted = manager->request(2);
            CHECK(promoted);
            auto resident_table = sub::make_gpu_page_table(t.pages, *layout, *manager);
            CHECK(resident_table && (*resident_table)[1].state == static_cast<std::uint32_t>(sub::gpu_page_state::resident));
        }
    }
    {
        std::array<sub::page_entry, 2> pages{};
        pages[0] = {4, 0, 2, 8};
        pages[1] = {4, 2, 1, 4};
        std::array<std::uint32_t, 3> references{11, 22, 33};
        std::array<sub::page_section_view, 6> sections{};
        sections[3] = {std::as_bytes(std::span(references)), 4};
        auto packed = sub::pack_page_arena(pages, sections, 64);
        CHECK(packed);
        if (packed) {
            CHECK(packed->layout.bytes() == 68);
            CHECK(packed->layout.page_offset(0) == 0 && packed->layout.page_offset(1) == 64);
            CHECK(packed->bytes.size() == 68);
            std::uint32_t first = 0, second = 0, third = 0;
            std::memcpy(&first, packed->bytes.data(), 4);
            std::memcpy(&second, packed->bytes.data() + 4, 4);
            std::memcpy(&third, packed->bytes.data() + 64, 4);
            CHECK(first == 11 && second == 22 && third == 33);
            CHECK(packed->bytes[8] == std::byte{0} && packed->bytes[63] == std::byte{0});
        }
        auto truncated = pages;
        truncated[0].bytes = 12;
        CHECK(!sub::pack_page_arena(truncated, sections, 64));
        auto outside = pages;
        outside[1].count = 2;
        outside[1].bytes = 8;
        CHECK(!sub::pack_page_arena(outside, sections, 64));
        CHECK(!sub::pack_page_arena(pages, std::span(sections).first(5), 64));
        auto bad_section = pages;
        bad_section[0].section = 7;
        CHECK(!sub::pack_page_arena(bad_section, sections, 64));
        auto empty = sub::pack_page_arena({}, sections, 64);
        CHECK(empty && empty->layout.bytes() == 0 && empty->bytes.empty());
    }
    {
        sub::page_request_buffer requests(2);
        CHECK(requests.capacity() == 2);
        CHECK(requests.header().count == 0 && requests.header().capacity == 2);
        CHECK(requests.push({.page = 4, .priority = 2, .item = 7, .frame = 9}));
        CHECK(requests.push({.page = 5, .priority = 1, .item = 8, .frame = 9}));
        CHECK(requests.header().count == 2 && requests.header().overflow == 0);
        CHECK(requests.request(0)->page == 4 && requests.request(1)->page == 5);
        CHECK(!requests.request(2));
        CHECK(!requests.push({.page = 6}));
        CHECK(requests.header().overflow == 1);
        requests.clear(11);
        CHECK(requests.header().count == 0 && requests.header().overflow == 0 && requests.header().frame == 11);
    }
    {
        auto t = small_table();
        auto manager = sub::make_page_manager(t.pages, t.dependencies, budget(400));
        CHECK(manager);
        if (!manager) {
            return 1;
        }
        sub::page_streamer streamer(std::move(*manager), 2);
        auto first = streamer.request(2, 3, 7, 9);
        CHECK(first);
        CHECK(streamer.pending(2));
        auto started = streamer.begin();
        CHECK(started && started->has_value());
        if (started && started->has_value()) {
            CHECK((*started)->page == 2 && (*started)->bytes == 100 && (*started)->id == *first);
        }
        CHECK(!streamer.manager().resident(2));
        auto completed = streamer.complete();
        CHECK(completed);
        CHECK(streamer.manager().resident(2));
        CHECK(streamer.stats().starts == 1 && streamer.stats().completions == 1);

        // Coalescing happens before the queue bound; an active page is already pending.
        auto coalesced = streamer.request(3);
        auto duplicate = streamer.request(3);
        CHECK(coalesced && duplicate && *coalesced == *duplicate);
        CHECK(streamer.stats().coalesced == 1);
        auto began = streamer.begin();
        CHECK(began && began->has_value());
        CHECK(streamer.complete());
        CHECK(streamer.manager().resident(3) && !streamer.manager().resident(2));
    }
    {
        auto t = small_table();
        auto manager = sub::make_page_manager(t.pages, t.dependencies, budget(400));
        CHECK(manager);
        if (manager) {
            sub::page_streamer streamer(std::move(*manager), 1);
            CHECK(streamer.request(2));
            auto full = streamer.request(3);
            CHECK(!full && full.error() == sub::residency_error::capacity);
            CHECK(!streamer.complete());
        }
    }
    return 0;
}
