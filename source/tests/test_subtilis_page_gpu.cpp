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
import deren.engine.subtilis.residency;
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
    // Real buffers/copies with deterministic completion observation. Holding back
    // poll must never make the production pager call wait or publish a mapping.
    struct controlled_submission : rhi::command_submission {
        std::string_view get_tag() const noexcept override {
            return "test.page_completion";
        }
        rhi::command_submission* native = nullptr;
        bool ready = false, reject = false, fail_poll = false;
        unsigned submits = 0, waits = 0;
        std::expected<rhi::submission_token, rhi::error> submit(rhi::command_buffer& commands) override {
            ++submits;
            return reject ? std::unexpected(rhi::error::operation_failed) : native->submit(commands);
        }
        std::expected<rhi::submission_token, rhi::error> submit(std::span<rhi::shared_object<rhi::mergeable_command_buffer> const> commands) override {
            ++submits;
            return reject ? std::unexpected(rhi::error::operation_failed) : native->submit(commands);
        }
        rhi::error poll(rhi::submission_token token) noexcept override {
            return fail_poll ? rhi::error::operation_failed : ready ? native->poll(token)
                                                                    : rhi::error::not_ready;
        }
        rhi::error wait(rhi::submission_token token, std::uint64_t timeout) noexcept override {
            ++waits;
            return native->wait(token, timeout);
        }
    };
    struct controlled_core : rhi::api_core {
        std::string_view get_tag() const noexcept override {
            return "test.page_core";
        }
        rhi::shared_object<rhi::api_core> native;
        controlled_submission completion;
        unsigned idles = 0;
        explicit controlled_core(rhi::shared_object<rhi::api_core> core)
            : native(std::move(core)) {
            bind_views(*native->resources.get(), *native->commands.get(), *native->frames.get());
            completion.native = rhi::query_extension<rhi::command_submission>(*native);
        }
        void release() noexcept override {
            delete this;
        }
        rhi::ability_bits abilities() const noexcept override {
            return native->abilities();
        }
        rhi::extension* query_extension(rhi::extension_kind kind) noexcept override {
            return kind == rhi::extension_kind::command_submission ? &completion : native->query_extension(kind);
        }
        void wait_idle() override {
            ++idles;
            native->wait_idle();
        }
        std::uint32_t api_version() const noexcept override {
            return native->api_version();
        }
        rhi::stage_time_map get_time_profiling() const override {
            return native->get_time_profiling();
        }
        void record_stage_time(std::string_view name, double time) override {
            native->record_stage_time(name, time);
        }
        void reset_time_profiling_window() override {
            native->reset_time_profiling_window();
        }
    };
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

    std::shared_ptr<sub::built_geometry const> make_paged_geometry(bool padded = false, bool pin_first_index_page = false) {
        auto fixture = subtilis_fixture::cross_dag();
        if (padded) {
            fixture.data.indices.insert(fixture.data.indices.end(), 20000, 0u);
        }
        if (!wire::compute_pages(fixture.data, {})) {
            return nullptr;
        }
        if (pin_first_index_page) {
            fixture.data.pages.front().flags = static_cast<std::uint16_t>(wire::page_flags::always_resident);
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
    std::uint32_t resident_pages = 0;
    for (std::uint32_t i = 0; i < image.page_count(); ++i) {
        auto const& row = table[i];
        auto const& page = geometry->pages()[i];
        CHECK(row.page == i);
        CHECK(row.section == page.section && row.first == page.first && row.count == page.count && row.bytes == page.bytes);
        bool const pinned = (page.flags & static_cast<std::uint16_t>(wire::page_flags::always_resident)) != 0;
        CHECK(row.state == static_cast<std::uint32_t>(pinned ? sub::gpu_page_state::resident : sub::gpu_page_state::absent));
        CHECK(row.flags == page.flags);
        if (row.state != static_cast<std::uint32_t>(sub::gpu_page_state::resident)) {
            CHECK(row.offset == 0u);
            continue;
        }
        ++resident_pages;
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
    CHECK(resident_pages == 0 || arena_end > 0);
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
            CHECK(header->count == 0 && header->frame == 0);
            CHECK(reinterpret_cast<sub::gpu_page_table_entry const*>(table_bytes.data())[target].state ==
                  static_cast<std::uint32_t>(sub::gpu_page_state::requested));

            auto const installed = image.install_requested_pages(1ull << 30, 8);
            CHECK(installed.requests == 0 && installed.installed == 0 && installed.submitted >= 1);
            core->wait_idle();
            CHECK(image.collect_page_uploads().installed >= 1);
            CHECK(image.install_requested_pages(0, 8).pending == 0);
            CHECK(header->count == 0);
            auto const& installed_row = reinterpret_cast<sub::gpu_page_table_entry const*>(table_bytes.data())[target];
            CHECK(installed_row.state == static_cast<std::uint32_t>(sub::gpu_page_state::resident));
            CHECK(installed_row.offset % 64u == 0u);
            CHECK(std::uint64_t(installed_row.offset) + installed_row.bytes <= arena_bytes.size());
            auto const& installed_page = geometry->pages()[target];
            if (installed_page.section == 1) {
                CHECK(matches<std::uint32_t>(arena_bytes.data(), geometry->indices(), installed_page.first, installed_page.count,
                                             installed_row.offset));
            }
        }
    }
    CHECK(image.configure_page_frames(3) == rhi::error::ok);
    CHECK(image.page_requests(0) != image.page_requests(1));
    CHECK(image.page_requests(1) != image.page_requests(2));
    CHECK(image.page_table(0) != image.page_table(2));
    CHECK(image.page_table(3) == nullptr);
    CHECK(image.complete_page_frame(3) == rhi::error::invalid_argument);
    CHECK(image.publish_page_frame(0) == rhi::error::ok);
    CHECK(image.configure_page_frames(4) == rhi::error::not_ready);
    CHECK(image.install_requested_pages(0, 9, 3).result == rhi::error::invalid_argument);
    auto snapshot = image.page_table(0)->mapped();
    std::vector<std::byte> preserved(snapshot.begin(), snapshot.end());
    CHECK(image.publish_page_frame(0) == rhi::error::not_ready);
    CHECK(image.install_requested_pages(0, 9, 0).result == rhi::error::not_ready);
    CHECK(image.install_requested_pages(0, 9, 1).result == rhi::error::ok);
    CHECK(std::memcmp(snapshot.data(), preserved.data(), snapshot.size()) == 0);
    CHECK(image.complete_page_frame(0) == rhi::error::ok);
    auto large = make_paged_geometry(true);
    CHECK(large);
    if (large) {
        auto manager = sub::make_page_manager(large->pages(), large->page_dependencies());
        CHECK(manager);
        if (manager) {
            std::vector<std::uint32_t> candidates;
            for (std::uint32_t i = 0; i < large->pages().size(); ++i) {
                auto plan = manager->plan_request(i);
                if (plan && plan->installs.size() == 1) {
                    candidates.push_back(i);
                }
            }
            CHECK(candidates.size() >= 2);
            auto bounded = sub::upload_geometry(core, large, rhi::to_bits(rhi::buffer_flag::device_address),
                                                sub::geometry_upload_mode::synchronous,
                                                (manager->stats().resident_pages + 1ull) * wire::page_bytes);
            CHECK(bounded);
            if (bounded && candidates.size() >= 2) {
                auto const& pages = **bounded;
                CHECK(pages.configure_page_frames(3) == rhi::error::ok);
                auto queue = [&](std::uint32_t slot, std::uint32_t page) {
                    auto bytes = pages.page_requests(slot)->mapped();
                    sub::gpu_page_request_header header{1, pages.page_request_capacity(), 0, 0};
                    sub::gpu_page_request request{page, 0, 0, 0};
                    std::memcpy(bytes.data(), &header, sizeof(header));
                    std::memcpy(bytes.data() + sizeof(header), &request, sizeof(request));
                };
                auto row = [&](std::uint32_t slot, std::uint32_t page) {
                    sub::gpu_page_table_entry out{};
                    auto bytes = pages.page_table(slot)->mapped();
                    std::memcpy(&out, bytes.data() + page * sizeof(out), sizeof(out));
                    return out;
                };
                queue(0, candidates[0]);
                CHECK(pages.install_requested_pages(1ull << 20, 1, 0).submitted == 1);
                core->wait_idle();
                CHECK(pages.collect_page_uploads().installed == 1);
                CHECK(pages.install_requested_pages(0, 1, 0).pending == 0);
                CHECK(pages.publish_page_frame(0) == rhi::error::ok);
                CHECK(pages.publish_page_frame(2) == rhi::error::ok);
                auto old_row = row(0, candidates[0]);
                auto arena = pages.page_arena()->mapped();
                std::vector<std::byte> old_bytes(arena.begin() + old_row.offset, arena.begin() + old_row.offset + old_row.bytes);
                queue(1, candidates[1]);
                auto deferred = pages.install_requested_pages(1ull << 20, 2, 1);
                CHECK(deferred.result == rhi::error::ok && deferred.installed == 0 && deferred.pending == 1);
                CHECK(row(0, candidates[0]).state == static_cast<std::uint32_t>(sub::gpu_page_state::resident));
                CHECK(row(1, candidates[0]).state == static_cast<std::uint32_t>(sub::gpu_page_state::absent));
                CHECK(std::memcmp(old_bytes.data(), arena.data() + old_row.offset, old_bytes.size()) == 0);
                CHECK(pages.publish_page_frame(1) == rhi::error::ok);
                CHECK(pages.complete_page_frame(0) == rhi::error::ok);
                auto still_referenced = pages.install_requested_pages(1ull << 20, 3, 0);
                CHECK(still_referenced.installed == 0 && still_referenced.pending == 1);
                CHECK(std::memcmp(old_bytes.data(), arena.data() + old_row.offset, old_bytes.size()) == 0);
                CHECK(pages.complete_page_frame(2) == rhi::error::ok);
                auto admitted = pages.install_requested_pages(1ull << 20, 3, 0);
                CHECK(admitted.result == rhi::error::ok && admitted.submitted == 1 && admitted.pending == 1);
                core->wait_idle();
                CHECK(pages.collect_page_uploads().installed == 1);
                CHECK(pages.install_requested_pages(0, 3, 0).pending == 0);
                CHECK(row(0, candidates[1]).offset == old_row.offset);
                CHECK(pages.complete_page_frame(1) == rhi::error::ok);
                // Completed GPU queues can contain duplicates, invalid pages or
                // an overflowing count. CPU ingestion remains bounded and deduplicated.
                auto input = pages.page_requests(2)->mapped();
                sub::gpu_page_request_header overflow{pages.page_request_capacity() + 1, pages.page_request_capacity(), 1, 4};
                std::memset(input.data(), 0xff, input.size());
                std::memcpy(input.data(), &overflow, sizeof(overflow));
                sub::gpu_page_request duplicate{candidates[0], 0, 0, 4};
                std::memcpy(input.data() + sizeof(overflow), &duplicate, sizeof(duplicate));
                std::memcpy(input.data() + sizeof(overflow) + sizeof(duplicate), &duplicate, sizeof(duplicate));
                auto queued = pages.install_requested_pages(0, 4, 2);
                CHECK(queued.result == rhi::error::ok && queued.pending == 1 && queued.overflow == 1 && queued.page_misses == 4);
                CHECK(pages.complete_page_frame(2) == rhi::error::ok);
                CHECK(pages.install_requested_pages(1ull << 20, 5, 2).submitted == 1);
                core->wait_idle();
                CHECK(pages.collect_page_uploads().installed == 1);
            }
        }
    }
    CHECK(image.publish_page_frame(0) == rhi::error::ok);
    CHECK(image.complete_page_frame(0) == rhi::error::ok);
    deren::vk_test::write_line("page image: {} pages, {} arena bytes, table {} bytes", image.page_count(), arena_bytes.size(), table_bytes.size());
    {
        auto pool = sub::page_arena_pool::create(core, 3ull * sub::page_arena_pool::slot_bytes);
        CHECK(pool);
        if (pool) {
            deren::utility::vessel::vector<std::uint32_t> occupied, replacement;
            CHECK((*pool)->acquire(3, occupied));
            for (auto slot : occupied) {
                (*pool)->set_owner(slot, pool->get(), [](void* context, std::uint32_t victim) {
                    auto& arena = *static_cast<sub::page_arena_pool*>(context);
                    arena.clear_owner(victim);
                    arena.release(victim);
                    return true;
                });
            }
            std::uint32_t evicted = 0;
            std::uint32_t skipped_evicted = 0;
            CHECK(!(*pool)->acquire(1, replacement, pool->get(), &skipped_evicted));
            CHECK(skipped_evicted == 0);
            CHECK((*pool)->acquire(1, replacement, nullptr, &evicted));
            CHECK(evicted == 1);
        }
    }
    {
        auto pool = sub::page_arena_pool::create(core, 2ull * sub::page_arena_pool::slot_bytes);
        CHECK(pool);
        if (pool) {
            deren::utility::vessel::vector<std::uint32_t> occupied, reused, extra;
            CHECK((*pool)->acquire(2, occupied));
            (*pool)->release(occupied);
            // Returning a slot twice must not append it twice. A duplicate free-list entry would
            // later hand the same physical arena slot to two sources.
            (*pool)->release(occupied.front());
            CHECK((*pool)->acquire(2, reused));
            CHECK(reused.size() == 2 && reused[0] != reused[1]);
            CHECK(!(*pool)->acquire(1, extra));
            (*pool)->release(reused);
        }
    }
    {
        auto pool = sub::page_arena_pool::create(core, sub::page_arena_pool::slot_bytes);
        CHECK(pool);
        if (pool) {
            deren::utility::vessel::vector<std::uint32_t> slot, reused, extra;
            CHECK((*pool)->acquire(1, slot));
            (*pool)->retain(slot.front());
            (*pool)->release(slot.front()); // Deferred until the published frame drops it.
            (*pool)->drop(slot.front());
            (*pool)->release(slot.front()); // Must be a no-op: the slot is already free.
            CHECK((*pool)->acquire(1, reused));
            CHECK(reused.size() == 1 && reused.front() == slot.front());
            CHECK(!(*pool)->acquire(1, extra));
            (*pool)->release(reused);
        }
    }
    {
        auto manager = sub::make_page_manager(geometry->pages(), geometry->page_dependencies());
        CHECK(manager);
        if (manager) {
            std::uint32_t single = UINT32_MAX;
            std::uint32_t pair = UINT32_MAX;
            for (std::uint32_t page = 0; page < geometry->pages().size(); ++page) {
                auto plan = manager->plan_request(page);
                if (plan && plan->installs.size() == 1) {
                    single = page;
                } else if (plan && plan->installs.size() == 2) {
                    pair = page;
                }
            }
            CHECK(single != UINT32_MAX && pair != UINT32_MAX);
            if (single != UINT32_MAX && pair != UINT32_MAX) {
                auto pool = sub::page_arena_pool::create(core, 2u * sub::page_arena_pool::slot_bytes);
                CHECK(pool);
                if (pool) {
                    auto upload = [&] {
                        return sub::upload_geometry(core, geometry, rhi::to_bits(rhi::buffer_flag::device_address),
                                                    sub::geometry_upload_mode::synchronous, 2u * sub::page_arena_pool::slot_bytes, *pool);
                    };
                    auto first = upload();
                    auto second = upload();
                    CHECK(first && second);
                    if (first && second) {
                        auto queue = [](sub::uploaded_geometry const& destination, std::uint32_t page) {
                            auto bytes = destination.page_requests(0)->mapped();
                            sub::gpu_page_request_header header{1, destination.page_request_capacity(), 0, 0};
                            std::memcpy(bytes.data(), &header, sizeof(header));
                            sub::gpu_page_request request{page, 0, 0, 0};
                            std::memcpy(bytes.data() + sizeof(header), &request, sizeof(request));
                        };
                        CHECK((*first)->configure_page_frames(1) == rhi::error::ok);
                        queue(**first, pair);
                        auto installed_first = (*first)->install_requested_pages(1ull << 20, 1, 0);
                        CHECK(installed_first.result == rhi::error::ok && installed_first.submitted == 2);
                        core->wait_idle();
                        CHECK((*first)->collect_page_uploads().installed == 2);
                        CHECK((*second)->configure_page_frames(1) == rhi::error::ok);
                        queue(**second, single);
                        // The pool holds both slots of the first source. Admitting the second source
                        // must reclaim an unreferenced page instead of failing or double-allocating.
                        auto installed_second = (*second)->install_requested_pages(1ull << 20, 2, 0);
                        CHECK(installed_second.result == rhi::error::ok && installed_second.submitted == 1);
                        CHECK(installed_second.evicted >= 1);
                        core->wait_idle();
                        CHECK((*second)->collect_page_uploads().installed == 1);
                        CHECK((*first)->publish_page_frame(0) == rhi::error::ok);
                        CHECK((*second)->publish_page_frame(0) == rhi::error::ok);
                        auto const* first_table = reinterpret_cast<sub::gpu_page_table_entry const*>((*first)->page_table(0)->mapped().data());
                        auto const* second_table = reinterpret_cast<sub::gpu_page_table_entry const*>((*second)->page_table(0)->mapped().data());
                        CHECK(first_table[pair].state == static_cast<std::uint32_t>(sub::gpu_page_state::absent));
                        CHECK(second_table[single].state == static_cast<std::uint32_t>(sub::gpu_page_state::resident));
                        CHECK((*first)->complete_page_frame(0) == rhi::error::ok);
                        CHECK((*second)->complete_page_frame(0) == rhi::error::ok);
                    }
                }
            }
        }
    }
    {
        auto pinned = make_paged_geometry(false, true);
        CHECK(pinned);
        if (pinned) {
            auto pinned_manager = sub::make_page_manager(pinned->pages(), pinned->page_dependencies());
            std::uint64_t pinned_bytes = 0;
            for (auto const& page : pinned->pages()) {
                if ((page.flags & static_cast<std::uint16_t>(wire::page_flags::always_resident)) != 0) {
                    pinned_bytes += page.bytes;
                }
            }
            CHECK(pinned_manager);
            CHECK(pinned_bytes > 0);
            if (pinned_manager) {
                CHECK(pinned_manager->stats().resident_pages == 1);
                CHECK(pinned_manager->stats().resident_bytes == pinned_bytes);
            }
            auto pool = sub::page_arena_pool::create(core, sub::page_arena_pool::slot_bytes);
            CHECK(pool);
            if (pool) {
                auto upload = [&] {
                    return sub::upload_geometry(core, pinned, rhi::to_bits(rhi::buffer_flag::device_address),
                                                sub::geometry_upload_mode::synchronous, sub::page_arena_pool::slot_bytes, *pool);
                };
                auto first = upload();
                CHECK(first && (*first)->page_table() != nullptr);
                if (first && (*first)->page_table()) {
                    auto const* table = reinterpret_cast<sub::gpu_page_table_entry const*>((*first)->page_table()->mapped().data());
                    CHECK((*first)->page_arena()->size() == sub::page_arena_pool::slot_bytes);
                    CHECK(table[0].state == static_cast<std::uint32_t>(sub::gpu_page_state::resident));
                    CHECK(table[0].bytes == pinned->pages().front().bytes);
                    CHECK(table[0].offset == 0);
                }
                auto second = upload();
                CHECK(second && (*second)->page_table() == nullptr && (*second)->cut_groups() != nullptr);
            }
        }
    }
    {
        auto oversized = sub::page_arena_pool::create(core, sub::page_arena_pool::max_bytes + sub::page_arena_pool::slot_bytes);
        CHECK(!oversized);
        auto empty = sub::page_arena_pool::create(core, 0);
        CHECK(!empty);
    }
    {
        auto adapter = std::make_shared<controlled_core>(core);
        rhi::shared_object<rhi::api_core> wrapped{adapter, adapter.get()};
        auto const pool_bytes = geometry->pages().size() * std::uint64_t{sub::page_arena_pool::slot_bytes};
        auto pool = sub::page_arena_pool::create(wrapped, pool_bytes);
        CHECK(pool);
        if (!pool) {
            return deren::vk_test::finish("test_subtilis_page_gpu");
        }
        auto async = sub::upload_geometry(wrapped, geometry, rhi::to_bits(rhi::buffer_flag::device_address),
                                          sub::geometry_upload_mode::synchronous, pool_bytes, *pool);
        CHECK(async);
        if (async) {
            auto const& pages = **async;
            auto const startup_idles = adapter->idles;
            CHECK(pages.configure_page_frames(2) == rhi::error::ok);
            auto manager = sub::make_page_manager(geometry->pages(), geometry->page_dependencies());
            std::vector<std::uint32_t> targets;
            for (std::uint32_t i = 0; i < geometry->pages().size(); ++i) {
                if (manager && !manager->resident(i)) {
                    targets.push_back(i);
                }
            }
            CHECK(targets.size() >= 2);
            auto queue = [&](sub::uploaded_geometry const& destination, std::uint32_t slot) {
                auto bytes = destination.page_requests(slot)->mapped();
                sub::gpu_page_request_header header{static_cast<std::uint32_t>(targets.size()), destination.page_request_capacity(), 0, 0};
                std::memcpy(bytes.data(), &header, sizeof(header));
                for (std::size_t i = 0; i < targets.size(); ++i) {
                    sub::gpu_page_request request{targets[i], 0, 0, 0};
                    std::memcpy(bytes.data() + sizeof(header) + i * sizeof(request), &request, sizeof(request));
                }
            };
            queue(pages, 0);
            auto submitted = pages.install_requested_pages(1ull << 20, 1, 0);
            CHECK(submitted.result == rhi::error::ok && submitted.installed == 0 && submitted.submitted == targets.size());
            CHECK(pages.has_pending_page_upload());
            CHECK(submitted.in_flight_bytes == submitted.bytes && submitted.bytes > 0);
            CHECK(adapter->completion.waits == 0 && adapter->completion.submits == 1);
            CHECK(pages.publish_page_frame(0) == rhi::error::ok);
            auto frozen = pages.page_table(0)->mapped();
            std::vector<std::byte> before(frozen.begin(), frozen.end());
            queue(pages, 1);
            auto duplicate = pages.install_requested_pages(1ull << 20, 2, 1);
            CHECK(duplicate.submitted == 0 && duplicate.bytes == 0 && adapter->completion.submits == 1);
            CHECK(duplicate.in_flight_bytes == submitted.bytes);
            core->wait_idle(); // Test setup only: the pager must still observe pending until allowed.
            adapter->completion.fail_poll = true;
            auto const first_failure = pages.collect_page_uploads();
            CHECK(first_failure.result == rhi::error::ok);
            CHECK(first_failure.in_flight_bytes == submitted.bytes);
            CHECK(first_failure.abandoned == 0);
            CHECK(pages.has_pending_page_upload());
            auto const second_failure = pages.collect_page_uploads();
            CHECK(second_failure.result == rhi::error::ok);
            CHECK(second_failure.in_flight_bytes == submitted.bytes);
            CHECK(second_failure.abandoned == 0);
            CHECK(pages.has_pending_page_upload());
            auto const abandoned = pages.collect_page_uploads();
            CHECK(abandoned.result == rhi::error::ok);
            CHECK(abandoned.abandoned == targets.size());
            CHECK(abandoned.in_flight_bytes == 0);
            CHECK(!pages.has_pending_page_upload());
            deren::utility::vessel::vector<std::uint32_t> competing;
            bool const reused = (*pool)->acquire(1, competing);
            CHECK(reused);
            if (reused) {
                (*pool)->release(competing);
            }
            CHECK(std::memcmp(before.data(), frozen.data(), frozen.size()) == 0);
            // The abandoned pages are absent, not resident, and the same demand can be
            // submitted again without a stale ticket or reservation blocking it.
            auto const cleared = pages.install_requested_pages(0, 3, 1);
            CHECK(cleared.result == rhi::error::ok && cleared.pending == 0);
            {
                auto const rows = reinterpret_cast<sub::gpu_page_table_entry const*>(pages.page_table(1)->mapped().data());
                for (auto id : targets) {
                    CHECK(rows[id].state == static_cast<std::uint32_t>(sub::gpu_page_state::absent));
                }
            }
            adapter->completion.fail_poll = false;
            adapter->completion.ready = false;
            queue(pages, 1);
            auto const retried = pages.install_requested_pages(1ull << 20, 4, 1);
            CHECK(retried.result == rhi::error::ok && retried.submitted == targets.size());
            CHECK(pages.has_pending_page_upload());
            core->wait_idle();
            adapter->completion.ready = true;
            auto completed = pages.collect_page_uploads();
            CHECK(completed.result == rhi::error::ok && completed.installed == targets.size() && completed.bytes == 0);
            CHECK(!pages.has_pending_page_upload());
            CHECK(completed.in_flight_bytes == 0);
            CHECK(std::memcmp(before.data(), frozen.data(), frozen.size()) == 0);
            CHECK(pages.install_requested_pages(0, 3, 1).bytes == 0);
            CHECK(pages.publish_page_frame(1) == rhi::error::ok);
            for (auto id : targets) {
                auto rows = reinterpret_cast<sub::gpu_page_table_entry const*>(pages.page_table(1)->mapped().data());
                CHECK(rows[id].state == static_cast<std::uint32_t>(sub::gpu_page_state::resident));
            }
            CHECK(adapter->completion.waits == 0);
            CHECK(adapter->idles == startup_idles);
            CHECK(pages.complete_page_frame(0) == rhi::error::ok);
            CHECK(pages.complete_page_frame(1) == rhi::error::ok);
            // A rejected native submit must release its reservations and leave
            // demand retryable, without exposing partially copied pages.
            auto retry = sub::upload_geometry(wrapped, geometry, rhi::to_bits(rhi::buffer_flag::device_address), sub::geometry_upload_mode::synchronous);
            CHECK(retry);
            if (retry) {
                CHECK((*retry)->configure_page_frames(2) == rhi::error::ok);
                adapter->completion.reject = true;
                queue(**retry, 0);
                auto rejected = (*retry)->install_requested_pages(1ull << 20, 4, 0);
                CHECK(rejected.result == rhi::error::operation_failed && rejected.bytes == 0);
                CHECK(!(*retry)->has_pending_page_upload());
                adapter->completion.reject = false;
                adapter->completion.ready = false;
                auto resubmitted = (*retry)->install_requested_pages(1ull << 20, 5, 0);
                CHECK(resubmitted.result == rhi::error::ok && resubmitted.submitted == targets.size());
                CHECK((*retry)->collect_page_uploads().in_flight == targets.size());
                core->wait_idle();
                adapter->completion.ready = true;
                CHECK((*retry)->collect_page_uploads().installed == targets.size());
                CHECK(adapter->completion.waits == 0);
            }
        }
        // Destroying a paged source after publishing snapshots must drop every retained
        // slot reference, otherwise the shared arena leaks the whole source forever.
        CHECK(async);
        async->reset();
        CHECK(!*async);
        auto const capacity = (*pool)->capacity_slots();
        deren::utility::vessel::vector<std::uint32_t> all_slots;
        CHECK((*pool)->acquire(capacity, all_slots));
        CHECK(all_slots.size() == capacity);
        (*pool)->release(all_slots);
    }
    return deren::vk_test::finish("test_subtilis_page_gpu");
}
