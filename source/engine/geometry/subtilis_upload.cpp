module;

#include <cstddef>

module deren.engine.subtilis.upload;
import deren.vstd;
import deren.engine.pass.chain;
import deren.engine.pass.buffer_storage;
import deren.engine.render_resource;
import deren.engine.buffer_transfer;
import deren.engine.subtilis.page_streaming;
import deren.asset.subtilis.types;

namespace deren::engine::subtilis {
    namespace {
        // The page arena is an image of the wire payload, not of the engine DAG. A cluster is 32
        // encoded bytes and a group is 48 (36 used, 12 required zero padding); the engine structs are
        // 56 and 36 bytes respectively, so copying them would shift every page boundary.
        struct wire_page_cluster {
            [[maybe_unused]] std::uint32_t first_index = 0;
            [[maybe_unused]] std::uint32_t index_count = 0;
            [[maybe_unused]] std::uint32_t producer = 0;
            [[maybe_unused]] std::uint32_t consumer = 0;
            [[maybe_unused]] float center_x = 0;
            [[maybe_unused]] float center_y = 0;
            [[maybe_unused]] float center_z = 0;
            [[maybe_unused]] float radius = 0;
        };
        static_assert(sizeof(wire_page_cluster) == 32u);
        struct wire_page_group {
            [[maybe_unused]] std::uint32_t fine_first = 0;
            [[maybe_unused]] std::uint32_t fine_count = 0;
            [[maybe_unused]] std::uint32_t coarse_first = 0;
            [[maybe_unused]] std::uint32_t coarse_count = 0;
            [[maybe_unused]] float lod_x = 0;
            [[maybe_unused]] float lod_y = 0;
            [[maybe_unused]] float lod_z = 0;
            [[maybe_unused]] float lod_radius = 0;
            float error = 0;
            [[maybe_unused]] std::array<std::byte, 12> padding{};
        };
        static_assert(sizeof(wire_page_group) == 48u);
        static_assert(offsetof(wire_page_group, error) == 32u);

        struct page_payload_block {
            page_arena_payload arena;
            deren::utility::vessel::vector<gpu_page_table_entry> table;
            page_request_buffer requests;
            page_manager manager;
            std::uint32_t slots = 0;
            std::uint32_t slot_bytes = 64;
            page_payload_block(page_arena_payload&& arena_bytes, deren::utility::vessel::vector<gpu_page_table_entry>&& table_entries,
                               std::uint32_t request_capacity, page_manager&& residency, std::uint32_t slot_count, std::uint32_t slot_size)
                : arena(std::move(arena_bytes))
                , table(std::move(table_entries))
                , requests(request_capacity)
                , manager(std::move(residency))
                , slots(slot_count)
                , slot_bytes(slot_size) {
            }
        };

        std::expected<std::shared_ptr<page_payload_block>, residency_error>
        make_page_payload(built_geometry const& source, std::uint32_t slots) {
            auto const pages = source.pages();
            auto const* dag = source.hierarchy_dag();
            if (pages.empty() || !dag) {
                return std::unexpected(residency_error::invalid_page_table);
            }
            deren::utility::vessel::vector<wire_page_cluster> clusters;
            clusters.reserve(dag->clusters().size());
            for (auto const& cluster : dag->clusters()) {
                auto const& record = cluster.record;
                clusters.push_back({
                    record.first_index,
                    record.index_count,
                    cluster.producer,
                    cluster.consumer,
                    record.center_x,
                    record.center_y,
                    record.center_z,
                    record.radius,
                });
            }
            deren::utility::vessel::vector<wire_page_group> groups;
            groups.reserve(dag->groups().size());
            for (auto const& group : dag->groups()) {
                groups.push_back({
                    group.fine.first,
                    group.fine.count,
                    group.coarse.first,
                    group.coarse.count,
                    group.lod.x,
                    group.lod.y,
                    group.lod.z,
                    group.lod.radius,
                    group.error,
                });
            }
            std::array<page_section_view, 6> sections{};
            sections[0] = {std::as_bytes(source.indices()), 4};
            sections[1] = {std::as_bytes(std::span(clusters)), 32};
            sections[2] = {std::as_bytes(std::span(groups)), 48};
            sections[3] = {std::as_bytes(dag->references()), 4};
            sections[4] = {std::as_bytes(dag->leaves()), 4};
            sections[5] = {std::as_bytes(dag->roots()), 4};
            auto packed = pack_page_arena(pages, sections);
            if (!packed) {
                return std::unexpected(packed.error());
            }
            std::uint32_t const slot_bytes = page_arena_pool::slot_bytes;
            residency_limits limits{.max_resident_bytes = static_cast<std::uint64_t>(slots) * slot_bytes, .max_resident_pages = slots};
            auto manager = make_page_manager(pages, source.page_dependencies(), limits);
            if (!manager) {
                return std::unexpected(manager.error());
            }
            // Only pinned pages start resident. Index pages become resident when the cut requests them
            // and install_requested_pages() admits them under the per-frame byte budget.
            auto table = make_gpu_page_table(pages, packed->layout, *manager);
            if (!table) {
                return std::unexpected(table.error());
            }
            // The shader claims each page in the page table before appending, so a page appears at
            // most once per frame. Capacity therefore follows the page count rather than a duplicate
            // estimate; one 16-byte request per page is small next to the 32-byte table row.
            auto const capacity = std::max<std::uint32_t>(static_cast<std::uint32_t>(pages.size()), 1u);
            return std::make_shared<page_payload_block>(std::move(*packed), std::move(*table), capacity, std::move(*manager), slots, slot_bytes);
        }
    } // namespace

    struct page_arena_pool::state {
        upload_rhi::shared_object<upload_rhi::api_core> device;
        std::shared_ptr<render_resource::instances::generation const> arena;
        upload_rhi::shared_object<upload_rhi::buffer> buffer;
        deren::utility::vessel::vector<std::uint32_t> free_slots;
        deren::utility::vessel::vector<std::uint32_t> references;
        deren::utility::vessel::vector<std::uint8_t> pending_free;
        // Mirrors free_slots membership. It makes release() idempotent: a slot that was already
        // returned cannot be appended twice and later handed to two owners.
        deren::utility::vessel::vector<std::uint8_t> free_mark;
        deren::utility::vessel::vector<void*> owner_context;
        deren::utility::vessel::vector<page_arena_pool::slot_evict_fn> owner_evict;
        std::mutex lock;
    };

    page_arena_pool::page_arena_pool(std::shared_ptr<state> state) noexcept
        : state_(std::move(state)) {
    }

    page_arena_pool::~page_arena_pool() = default;

    std::expected<std::shared_ptr<page_arena_pool>, upload_rhi::error>
    page_arena_pool::create(upload_rhi::shared_object<upload_rhi::api_core> const& device, std::uint64_t bytes) {
        if (!device || bytes < slot_bytes) {
            return std::unexpected(upload_rhi::error::invalid_argument);
        }
        auto const count = bytes / slot_bytes;
        if (count == 0 || count > max_slots) {
            return std::unexpected(upload_rhi::error::invalid_argument);
        }
        namespace managed = render_resource::instances;
        auto generation = std::make_shared<managed::generation>();
        generation->device = device;
        generation->description.type = managed::kind::buffer;
        generation->description.life = managed::lifetime::device;
        constexpr auto flags = upload_rhi::to_bits(upload_rhi::buffer_flag::device_address) |
                               upload_rhi::to_bits(upload_rhi::buffer_flag::storage) |
                               upload_rhi::to_bits(upload_rhi::buffer_flag::transfer_destination);
        generation->description.buffer = {.size = count * slot_bytes, .usage = upload_rhi::buffer_usage::storage_coherent, .flags = flags};
        generation->objects.resize(1);
        generation->objects[0].buffer = device->resources->create_buffer(generation->description.buffer);
        if (!generation->objects[0].buffer) {
            return std::unexpected(upload_rhi::error::operation_failed);
        }
        auto pool_state = std::make_shared<state>();
        pool_state->device = device;
        pool_state->arena = std::move(generation);
        pool_state->buffer = upload_rhi::shared_object<upload_rhi::buffer>{pool_state->arena, pool_state->arena->objects[0].buffer.get()};
        pool_state->free_slots.reserve(static_cast<std::size_t>(count));
        pool_state->references.assign(static_cast<std::size_t>(count), 0u);
        pool_state->pending_free.assign(static_cast<std::size_t>(count), 0u);
        pool_state->free_mark.assign(static_cast<std::size_t>(count), 1u);
        pool_state->owner_context.assign(static_cast<std::size_t>(count), nullptr);
        pool_state->owner_evict.assign(static_cast<std::size_t>(count), nullptr);
        for (std::uint32_t slot = 0; slot < count; ++slot) {
            pool_state->free_slots.push_back(slot);
        }
        return std::shared_ptr<page_arena_pool>{new page_arena_pool(std::move(pool_state))};
    }

    std::uint32_t page_arena_pool::capacity_slots() const noexcept {
        return static_cast<std::uint32_t>(state_->references.size());
    }

    std::shared_ptr<render_resource::instances::generation const> const& page_arena_pool::arena() const noexcept {
        return state_->arena;
    }

    bool page_arena_pool::acquire(std::uint32_t count, deren::utility::vessel::vector<std::uint32_t>& out, void const* skip_owner, std::uint32_t* evicted) {
        out.clear();
        if (evicted) {
            *evicted = 0;
        }
        if (count == 0) {
            return true;
        }
        std::uint32_t evicted_count = 0;
        struct victim {
            void* context;
            slot_evict_fn evict;
            std::uint32_t slot;
        };
        for (;;) {
            deren::utility::vessel::vector<victim> victims;
            std::size_t needed = 0;
            {
                std::scoped_lock guard{state_->lock};
                std::size_t available = 0;
                for (auto const slot : state_->free_slots) {
                    if (state_->references[slot] == 0 && state_->free_mark[slot] != 0) {
                        ++available;
                    }
                }
                if (available >= count) {
                    out.reserve(count);
                    for (auto it = state_->free_slots.rbegin(); it != state_->free_slots.rend() && out.size() < count;) {
                        auto const slot = *it;
                        if (state_->references[slot] != 0 || state_->free_mark[slot] == 0) {
                            ++it;
                            continue;
                        }
                        out.push_back(slot);
                        state_->free_mark[slot] = 0;
                        // A reverse_iterator's base() points one past the element it dereferences,
                        // so the element to erase is prev(base()). Erasing next.base() - 1 removed
                        // the neighbouring slot and left this one on the free list: with one shared
                        // arena that handed the same physical slot to several sources.
                        auto const erased = state_->free_slots.erase(std::prev(it.base()));
                        it = std::make_reverse_iterator(erased);
                    }
                    if (evicted) {
                        *evicted = evicted_count;
                    }
                    return true;
                }
                needed = count - available;
                for (std::uint32_t slot = 0; slot < state_->references.size(); ++slot) {
                    if (state_->owner_evict[slot] != nullptr && state_->owner_context[slot] != skip_owner && state_->references[slot] == 0) {
                        victims.push_back({state_->owner_context[slot], state_->owner_evict[slot], slot});
                    }
                }
            }
            if (victims.empty()) {
                return false;
            }
            bool progress = false;
            for (auto const& candidate : victims) {
                if (candidate.evict(candidate.context, candidate.slot)) {
                    ++evicted_count;
                    progress = true;
                    if (--needed == 0) {
                        break;
                    }
                }
            }
            if (!progress) {
                return false;
            }
        }
    }

    void page_arena_pool::set_owner(std::uint32_t slot, void* context, slot_evict_fn evict) {
        std::scoped_lock guard{state_->lock};
        if (slot >= state_->references.size()) {
            return;
        }
        state_->owner_context[slot] = context;
        state_->owner_evict[slot] = evict;
    }

    void page_arena_pool::clear_owner(std::uint32_t slot) {
        std::scoped_lock guard{state_->lock};
        if (slot >= state_->references.size()) {
            return;
        }
        state_->owner_context[slot] = nullptr;
        state_->owner_evict[slot] = nullptr;
    }

    void page_arena_pool::release(std::uint32_t slot) {
        std::scoped_lock guard{state_->lock};
        if (slot >= state_->references.size()) {
            return;
        }
        if (state_->references[slot] != 0) {
            state_->pending_free[slot] = 1;
        } else if (state_->free_mark[slot] == 0) {
            state_->free_mark[slot] = 1;
            state_->free_slots.push_back(slot);
        }
    }

    void page_arena_pool::release(std::span<std::uint32_t const> slots) {
        for (auto const slot : slots) {
            this->release(slot);
        }
    }

    void page_arena_pool::retain(std::uint32_t slot) {
        std::scoped_lock guard{state_->lock};
        if (slot < state_->references.size()) {
            ++state_->references[slot];
        }
    }

    void page_arena_pool::drop(std::uint32_t slot) {
        std::scoped_lock guard{state_->lock};
        if (slot >= state_->references.size() || state_->references[slot] == 0) {
            return;
        }
        --state_->references[slot];
        if (state_->references[slot] == 0 && state_->pending_free[slot] != 0) {
            state_->pending_free[slot] = 0;
            if (state_->free_mark[slot] == 0) {
                state_->free_mark[slot] = 1;
                state_->free_slots.push_back(slot);
            }
        }
    }

    bool page_arena_pool::write(std::uint32_t slot, std::span<std::byte const> bytes) {
        if (slot >= this->capacity_slots() || bytes.size() > slot_bytes) {
            return false;
        }
        auto mapped = state_->buffer->mapped();
        if (mapped.size() < static_cast<std::size_t>(capacity_slots()) * slot_bytes) {
            return false;
        }
        std::memcpy(mapped.data() + static_cast<std::size_t>(slot) * slot_bytes, bytes.data(), bytes.size());
        return true;
    }

    double maximum_triangle_edge(meshlet const& record, std::span<std::uint8_t const> vertices,
                                 std::span<std::uint32_t const> indices, std::uint32_t stride) noexcept {
        constexpr double invalid = std::numeric_limits<double>::infinity();
        if (stride < 12u || vertices.size() % stride != 0u || record.index_count == 0u || record.index_count % 3u != 0u ||
            static_cast<std::uint64_t>(record.first_index) + record.index_count > indices.size()) {
            return invalid;
        }
        double maximum_squared = 0.0;
        for (std::uint32_t t = 0u; t < record.index_count; t += 3u) {
            std::array<std::array<float, 3>, 3> p{};
            for (std::uint32_t c = 0u; c < 3u; ++c) {
                auto const index = static_cast<std::int64_t>(indices[static_cast<std::size_t>(record.first_index) + t + c]) + record.base_vertex;
                if (index < 0 || static_cast<std::uint64_t>(index) >= vertices.size() / stride) {
                    return invalid;
                }
                std::memcpy(p[c].data(), vertices.data() + static_cast<std::size_t>(index) * stride, 12u);
                for (float v : p[c]) {
                    if (!std::isfinite(v)) {
                        return invalid;
                    }
                }
            }
            for (std::uint32_t c = 0u; c < 3u; ++c) {
                double squared = 0.0;
                for (std::uint32_t axis = 0u; axis < 3u; ++axis) {
                    double const delta = static_cast<double>(p[c][axis]) - p[(c + 1u) % 3u][axis];
                    squared += delta * delta;
                }
                maximum_squared = std::max(maximum_squared, squared);
            }
        }
        return std::nextafter(std::sqrt(maximum_squared), invalid);
    }
    double uploaded_geometry::triangle_edge(meshlet const& record) const noexcept {
        auto const key = std::tuple(record.first_index, record.index_count, record.base_vertex);
        auto const found = std::lower_bound(this->triangle_edges_.begin(), this->triangle_edges_.end(), key,
                                            [](triangle_edge_record const& a, auto const& b) { return std::tuple(a.first_index, a.index_count, a.base_vertex) < b; });
        if (found == this->triangle_edges_.end() || std::tuple(found->first_index, found->index_count, found->base_vertex) != key) {
            return std::numeric_limits<double>::infinity();
        }
        return found->maximum_edge;
    }

    struct page_state {
        struct upload {
            buffer_transfer_ticket ticket;
            residency_plan plan;
            deren::utility::vessel::vector<std::uint32_t> assigned;
            std::uint32_t failed_polls = 0;
        };
        std::optional<upload> uploading;
        struct frame {
            upload_rhi::shared_object<upload_rhi::buffer> table, requests;
            deren::utility::vessel::vector<std::uint32_t> references;
            bool published = false;
            std::uint32_t collected = 0, overflow = 0, page_misses = 0;
        };
        std::shared_ptr<page_payload_block> payload;
        upload_rhi::shared_object<upload_rhi::api_core> device;
        std::shared_ptr<page_arena_pool> pool;
        std::shared_ptr<render_resource::instances::generation const> arena;
        std::unique_ptr<buffer_transfer_service> transfers;
        deren::utility::vessel::vector<std::uint32_t> slot_of;
        deren::utility::vessel::vector<std::uint32_t> owned_slots;
        deren::utility::vessel::vector<frame> frames;
        deren::utility::vessel::vector<std::uint32_t> pending;
        deren::utility::vessel::vector<std::uint32_t> pending_priority;
        deren::utility::vessel::vector<std::uint8_t> reserved;
        std::span<std::uint32_t const> installing_pages{};

        void drop_frame_references(frame& value) const {
            if (pool) {
                for (auto const physical : value.references) {
                    pool->drop(physical);
                }
            }
            value.references.clear();
            value.published = false;
        }

        ~page_state() {
            transfers.reset(); // Drain accepted copies before returning their destination slots.
            // Published snapshots retain arena slots. Drop every frame's references before the
            // owned slots are released, or a destroyed source leaves slots permanently referenced
            // and the shared pool can never reclaim them.
            for (auto& frame : frames) {
                drop_frame_references(frame);
            }
            if (pool) {
                for (auto const slot : owned_slots) {
                    pool->clear_owner(slot);
                    pool->release(slot);
                }
            }
        }

        void release_owned(std::uint32_t slot) {
            if (pool) {
                pool->clear_owner(slot);
                pool->release(slot);
            }
            std::erase(owned_slots, slot);
        }

        // Called by the shared pool when another source needs a slot. The page must be unpinned
        // and dependency-free; referenced slots are never offered by the pool.
        static bool evict_slot(void* context, std::uint32_t slot) {
            auto* state = static_cast<page_state*>(context);
            if (state->uploading) {
                return false;
            }
            auto const found = std::ranges::find(state->slot_of, slot);
            if (found == state->slot_of.end()) {
                return false;
            }
            auto const page = static_cast<std::uint32_t>(std::distance(state->slot_of.begin(), found));
            if (std::ranges::find(state->installing_pages, page) != state->installing_pages.end()) {
                return false;
            }
            if (!state->payload->manager.evict(page)) {
                return false;
            }
            auto& entry = state->payload->table[page];
            entry.state = static_cast<std::uint32_t>(gpu_page_state::absent);
            entry.offset = 0;
            state->slot_of[page] = std::numeric_limits<std::uint32_t>::max();
            state->pending_priority[page] = 0u;
            state->reserved[slot] = 0;
            state->release_owned(slot);
            return true;
        }
    };

    std::expected<std::shared_ptr<uploaded_geometry const>, upload_rhi::error>
    upload_geometry(upload_rhi::shared_object<upload_rhi::api_core> const& device,
                    std::shared_ptr<built_geometry const> source, upload_rhi::buffer_flags flags, geometry_upload_mode mode,
                    std::uint64_t resident_page_bytes, std::shared_ptr<page_arena_pool> arena_pool) {
        if (mode != geometry_upload_mode::automatic && mode != geometry_upload_mode::synchronous && mode != geometry_upload_mode::batched) {
            return std::unexpected(upload_rhi::error::invalid_argument);
        }
        if (!device || !source || source->vertices().empty() || source->indices().empty() || !source->lod0_index_count() ||
            resident_page_bytes < deren::asset::subtilis::page_bytes) {
            return std::unexpected(upload_rhi::error::invalid_argument);
        }
        auto* scopes = upload_rhi::query_extension<upload_rhi::scoped_synchronization>(*device);
        bool const available = device.is_live() && scopes && upload_rhi::query_extension<upload_rhi::command_submission>(*device);
        if (mode == geometry_upload_mode::batched && !available) {
            return std::unexpected(upload_rhi::error::unsupported);
        }
        bool const batch_upload = mode != geometry_upload_mode::synchronous && available;
        namespace managed = render_resource::instances;
        struct cut_payload_block {
            deren::utility::vessel::vector<cut_group_record> groups;
            deren::utility::vessel::vector<std::uint32_t> references;
            deren::utility::vessel::vector<cut_cluster_record> clusters;
        };
        std::shared_ptr<cut_payload_block const> cut_payload;
        if (auto const* dag = source->hierarchy_dag(); dag && !dag->groups().empty()) {
            auto payload = std::make_shared<cut_payload_block>();
            payload->groups.reserve(dag->groups().size());
            for (auto const& group : dag->groups()) {
                payload->groups.push_back({
                    group.fine.first,
                    group.fine.count,
                    group.coarse.first,
                    group.coarse.count,
                    group.lod.x,
                    group.lod.y,
                    group.lod.z,
                    group.lod.radius,
                    group.error,
                });
            }
            payload->references.assign(dag->references().begin(), dag->references().end());
            payload->clusters.reserve(dag->clusters().size());
            for (auto const& cluster : dag->clusters()) {
                payload->clusters.push_back({cluster.record, cluster.producer, cluster.consumer});
            }
            cut_payload = std::move(payload);
        }
        auto resources = std::allocate_shared<pass::pass_chain>(deren::utility::memory::allocator<pass::pass_chain>{});
        auto& provider = resources->emplace<pass::resource_storage_pass>();
        std::shared_ptr<page_payload_block> page_payload;
        std::shared_ptr<page_state> page_pages;
        if (!source->pages().empty()) {
            auto const pages = source->pages();
            std::shared_ptr<page_arena_pool> pool = std::move(arena_pool);
            if (!pool) {
                auto created = page_arena_pool::create(device, resident_page_bytes);
                if (!created) {
                    return std::unexpected(created.error());
                }
                pool = std::move(*created);
            }
            std::uint32_t pinned = 0;
            for (auto const& page : pages) {
                pinned += (page.flags & static_cast<std::uint16_t>(deren::asset::subtilis::page_flags::always_resident)) != 0u ? 1u : 0u;
            }
            page_pages = std::make_shared<page_state>();
            page_pages->pool = pool;
            if (pinned > pool->capacity_slots() || !pool->acquire(pinned, page_pages->owned_slots)) {
                // The full index buffer also serves the cut, so a tight budget can disable paging for this geometry.
                page_pages.reset();
            }
            if (page_pages) {
                auto const slot_limit = std::min<std::uint32_t>(static_cast<std::uint32_t>(pages.size()), pool->capacity_slots());
                auto made = make_page_payload(*source, slot_limit);
                if (!made) {
                    return std::unexpected(upload_rhi::error::invalid_argument);
                }
                page_payload = std::move(*made);
                page_payload->slot_bytes = page_arena_pool::slot_bytes;
                page_pages->payload = page_payload;
                page_pages->device = device;
                page_pages->arena = pool->arena();
                page_pages->slot_of.assign(pages.size(), std::numeric_limits<std::uint32_t>::max());
                page_pages->pending_priority.assign(pages.size(), 0u);
                page_pages->reserved.assign(pool->capacity_slots(), 0u);
                std::size_t next_slot = 0;
                for (std::uint32_t i = 0; i < pages.size(); ++i) {
                    auto& entry = page_payload->table[i];
                    if (!page_payload->manager.resident(i)) {
                        entry.offset = 0;
                        continue;
                    }
                    auto const slot = page_pages->owned_slots[next_slot++];
                    page_pages->slot_of[i] = slot;
                    entry.offset = static_cast<std::uint64_t>(slot) * page_payload->slot_bytes;
                    auto const source_offset = page_payload->arena.layout.page_offset(i);
                    if (!source_offset || entry.bytes > page_payload->slot_bytes) {
                        return std::unexpected(upload_rhi::error::invalid_argument);
                    }
                    if (!pool->write(slot, std::span<std::byte const>{page_payload->arena.bytes.data() + *source_offset, entry.bytes})) {
                        return std::unexpected(upload_rhi::error::operation_failed);
                    }
                    pool->set_owner(slot, page_pages.get(), &page_state::evict_slot);
                }
            }
        }
        auto const request_count = std::size_t{2} + (cut_payload ? 3u : 0u) + (page_payload ? 2u : 0u);
        deren::utility::vessel::vector<managed::request> requests;
        requests.reserve(request_count);
        deren::utility::vessel::vector<std::span<std::byte const>> bytes;
        bytes.reserve(request_count);
        auto append = [&](render_resource::resource_id id, std::span<std::byte const> data, upload_rhi::buffer_usage usage,
                          upload_rhi::buffer_flags buffer_flags, std::shared_ptr<void const> const& storage_key) {
            requests.emplace_back();
            auto& request = requests.back();
            request.id = id;
            request.type = managed::kind::buffer;
            request.life = managed::lifetime::scene;
            request.buffer = {.size = data.size(), .usage = usage, .flags = buffer_flags};
            auto alias = std::shared_ptr<std::byte const>{storage_key, data.data()};
            request.dependencies.push_back(storage_key);
            if (!batch_upload) {
                request.initial_data = std::move(alias);
                request.initial_data_size = data.size();
                request.initialization_key = storage_key;
            }
            bytes.push_back(data);
        };
        append(render_resource::resource_id::scene_geometry_vertices, std::as_bytes(source->vertices()), upload_rhi::buffer_usage::vertex, flags, source);
        append(render_resource::resource_id::scene_geometry_indices, std::as_bytes(source->indices()), upload_rhi::buffer_usage::index, flags, source);
        if (cut_payload) {
            // Small immutable payload read only by the future cut compute stage. storage_coherent
            // (rather than storage_gpu_only) is required by the one-shot initial-data path.
            constexpr auto storage_flags = upload_rhi::to_bits(upload_rhi::buffer_flag::device_address) |
                                           upload_rhi::to_bits(upload_rhi::buffer_flag::storage) |
                                           upload_rhi::to_bits(upload_rhi::buffer_flag::transfer_destination);
            append(render_resource::resource_id::scene_cut_groups, std::as_bytes(std::span(cut_payload->groups)), upload_rhi::buffer_usage::storage_coherent, storage_flags, cut_payload);
            append(render_resource::resource_id::scene_cut_references, std::as_bytes(std::span(cut_payload->references)), upload_rhi::buffer_usage::storage_coherent, storage_flags, cut_payload);
            append(render_resource::resource_id::scene_cut_clusters, std::as_bytes(std::span(cut_payload->clusters)), upload_rhi::buffer_usage::storage_coherent, storage_flags, cut_payload);
        }
        if (page_payload) {
            constexpr auto storage_flags = upload_rhi::to_bits(upload_rhi::buffer_flag::device_address) |
                                           upload_rhi::to_bits(upload_rhi::buffer_flag::storage) |
                                           upload_rhi::to_bits(upload_rhi::buffer_flag::transfer_destination);
            append(render_resource::resource_id::scene_page_table, std::as_bytes(std::span(page_payload->table)), upload_rhi::buffer_usage::storage_coherent, storage_flags, page_payload);
            append(render_resource::resource_id::scene_page_requests, page_payload->requests.bytes(), upload_rhi::buffer_usage::storage_coherent, storage_flags, page_payload);
        }
        provider.configure(requests);
        auto admit = [&](managed::resource_snapshot const& snapshot) {
            for (auto const& request : requests) {
                auto value = snapshot.find(request.id);
                auto* buffer = value && value->objects.size() == 1 ? value->objects[0].buffer.get() : nullptr;
                if (!buffer || buffer->size() < request.buffer.size) {
                    return upload_rhi::error::unsupported;
                }
            }
            return upload_rhi::error::ok;
        };
        managed::resource_context context;
        if (auto status = resources->prepare_resources(device, context, admit); status != upload_rhi::error::ok) {
            return std::unexpected(status);
        }
        auto snapshot = resources->resources().snapshot();
        auto vertices = snapshot.find(render_resource::resource_id::scene_geometry_vertices);
        auto indices = snapshot.find(render_resource::resource_id::scene_geometry_indices);
        std::shared_ptr<managed::generation const> cut_groups;
        std::shared_ptr<managed::generation const> cut_references;
        std::shared_ptr<managed::generation const> cut_clusters;
        if (cut_payload) {
            cut_groups = snapshot.find(render_resource::resource_id::scene_cut_groups);
            cut_references = snapshot.find(render_resource::resource_id::scene_cut_references);
            cut_clusters = snapshot.find(render_resource::resource_id::scene_cut_clusters);
        }
        std::shared_ptr<managed::generation const> page_arena;
        std::shared_ptr<managed::generation const> page_table;
        std::shared_ptr<managed::generation const> page_requests;
        if (page_payload) {
            page_arena = page_pages->arena;
            page_table = snapshot.find(render_resource::resource_id::scene_page_table);
            page_requests = snapshot.find(render_resource::resource_id::scene_page_requests);
        }
        if (batch_upload) {
            namespace sync = upload_rhi::synchronization;
            // Immutable geometry is read by vertex input and the available shader stages.
            // The AS build verb records its own transfer-write -> build-read pre-barrier.
            constexpr auto readers = sync::stage::vertex_input | sync::stage::vertex_shader | sync::stage::fragment_shader |
                                     sync::stage::compute_shader | sync::stage::task_shader | sync::stage::mesh_shader | sync::stage::ray_tracing;
            auto const supported = static_cast<std::uint32_t>(scopes->supported_stages());
            sync::scope const published{static_cast<sync::stage>(supported & static_cast<std::uint32_t>(readers)), sync::access::read};
            buffer_transfer_service transfers(device);
            auto batch = transfers.make_batch();
            deren::utility::vessel::vector<std::shared_ptr<managed::generation const>> generations;
            generations.reserve(requests.size());
            for (auto const& request : requests) {
                generations.push_back(snapshot.find(request.id));
            }
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                auto const& generation = generations[i];
                if (!generation) {
                    return std::unexpected(upload_rhi::error::unsupported);
                }
                auto* buffer = generation->objects[0].buffer.get();
                auto access = resource_handle<upload_rhi::buffer>::from_shared(device, upload_rhi::shared_object<upload_rhi::buffer>{generation, buffer});
                // clang-tidy 23 mistakes the required separator after {} for an initializer trailing comma.
                if (auto result = batch.upload(std::move(access), 0, bytes[i], {}, published); result != upload_rhi::error::ok) { // NOLINT(readability-trailing-comma)
                    return std::unexpected(result);
                }
            }
            auto submitted = transfers.submit(std::move(batch));
            if (!submitted) {
                return std::unexpected(submitted.error());
            }
            if (auto result = submitted->wait(std::numeric_limits<std::uint64_t>::max()); result != upload_rhi::error::ok) {
                return std::unexpected(result);
            }
        }
        for (auto& request : requests) {
            request.initial_data.reset();
        }
        provider.configure(requests);
        deren::utility::vessel::vector<triangle_edge_record> edges;
        auto const* edge_mode = std::getenv("DEREN_SOFTWARE_RASTER_EDGE");
        auto const* gpu_mode = std::getenv("DEREN_SOFTWARE_RASTER_GPU");
        if ((edge_mode && std::string_view(edge_mode) == "1") || (gpu_mode && std::string_view(gpu_mode) == "1")) {
            auto append_edge = [&](meshlet const& record) {
                edges.push_back({
                    record.first_index,
                    record.index_count,
                    record.base_vertex,
                    maximum_triangle_edge(record, source->vertices(), source->indices(), source->vertex_stride()),
                });
            };
            if (auto const* dag = source->hierarchy_dag()) {
                edges.reserve(dag->clusters().size());
                for (auto const& c : dag->clusters()) {
                    append_edge(c.record);
                }
            } else {
                edges.reserve(source->hierarchy().clusters().size());
                for (auto const& c : source->hierarchy().clusters()) {
                    append_edge(c);
                }
            }
            std::ranges::sort(edges, [](auto const& a, auto const& b) {
                return std::tuple(a.first_index, a.index_count, a.base_vertex) < std::tuple(b.first_index, b.index_count, b.base_vertex);
            });
        }
        return std::allocate_shared<uploaded_geometry>(deren::utility::memory::allocator<uploaded_geometry>{}, uploaded_geometry::construction_key{},
                                                       std::move(resources), std::move(source), std::move(vertices), std::move(indices), std::move(cut_groups),
                                                       std::move(cut_references), std::move(cut_clusters),
                                                       cut_payload ? static_cast<std::uint32_t>(cut_payload->groups.size()) : 0u,
                                                       cut_payload ? static_cast<std::uint32_t>(cut_payload->references.size()) : 0u,
                                                       cut_payload ? static_cast<std::uint32_t>(cut_payload->clusters.size()) : 0u,
                                                       std::move(page_arena), std::move(page_table), std::move(page_requests),
                                                       page_payload ? static_cast<std::uint32_t>(page_payload->table.size()) : 0u,
                                                       page_payload ? page_payload->requests.capacity() : 0u, std::move(edges), std::move(page_pages));
    }

    upload_rhi::error uploaded_geometry::configure_page_frames(std::uint32_t count) const {
        auto* state = page_state_.get();
        if (!state) {
            return upload_rhi::error::ok;
        }
        if (count == 0) {
            return upload_rhi::error::invalid_argument;
        }
        if (state->frames.size() == count) {
            return upload_rhi::error::ok;
        }
        if (std::ranges::any_of(state->frames, [](auto const& f) { return f.published; })) {
            return upload_rhi::error::not_ready;
        }
        // Allocate transactionally: a failed allocation must not replace a live buffer.
        auto frames = state->frames;
        frames.resize(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (frames[i].table) {
                continue;
            }
            if (i == 0) {
                frames[i].table = {page_table_, this->page_table()};
                frames[i].requests = {page_requests_, this->page_requests()};
                continue;
            }
            constexpr auto flags = upload_rhi::to_bits(upload_rhi::buffer_flag::device_address) |
                                   upload_rhi::to_bits(upload_rhi::buffer_flag::storage);
            frames[i].table = state->device->resources->create_buffer({.size = this->page_table()->size(), .usage = upload_rhi::buffer_usage::storage_coherent, .flags = flags, .initial_bytes = std::as_bytes(std::span(state->payload->table))});
            frames[i].requests = state->device->resources->create_buffer({.size = this->page_requests()->size(), .usage = upload_rhi::buffer_usage::storage_coherent, .flags = flags});
            if (!frames[i].table || !frames[i].requests) {
                return upload_rhi::error::operation_failed;
            }
            auto bytes = frames[i].requests->mapped();
            if (bytes.size() != frames[i].requests->size()) {
                return upload_rhi::error::operation_failed;
            }
            std::memset(bytes.data(), 0, bytes.size());
            gpu_page_request_header const header{0, page_request_capacity_, 0, 0};
            std::memcpy(bytes.data(), &header, sizeof(header));
        }
        state->frames = std::move(frames);
        return upload_rhi::error::ok;
    }

    upload_rhi::buffer* uploaded_geometry::page_table(std::uint32_t slot) const noexcept {
        if (!page_state_ || page_state_->frames.empty()) {
            return slot == 0 ? this->page_table() : nullptr;
        }
        return slot < page_state_->frames.size() ? page_state_->frames[slot].table.get() : nullptr;
    }
    upload_rhi::buffer* uploaded_geometry::page_requests(std::uint32_t slot) const noexcept {
        if (!page_state_ || page_state_->frames.empty()) {
            return slot == 0 ? this->page_requests() : nullptr;
        }
        return slot < page_state_->frames.size() ? page_state_->frames[slot].requests.get() : nullptr;
    }

    upload_rhi::error uploaded_geometry::complete_page_frame(std::uint32_t slot) const {
        auto* state = page_state_.get();
        if (!state) {
            return upload_rhi::error::ok;
        }
        if (state->frames.empty()) {
            if (auto result = configure_page_frames(1); result != upload_rhi::error::ok) {
                return result;
            }
        }
        if (slot >= state->frames.size()) {
            return upload_rhi::error::invalid_argument;
        }
        auto& frame = state->frames[slot];
        auto bytes = frame.requests->mapped();
        if (bytes.size() != frame.requests->size()) {
            // The snapshot is not being consumed, so its retained slots must not outlive it.
            state->drop_frame_references(frame);
            return upload_rhi::error::operation_failed;
        }
        gpu_page_request_header header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        frame.collected += header.count;
        frame.page_misses += header.frame;
        frame.overflow |= header.overflow | (header.count > page_request_capacity_ ? 1u : 0u);
        for (std::uint32_t i = 0; i < std::min(header.count, page_request_capacity_); ++i) {
            gpu_page_request request{};
            std::memcpy(&request, bytes.data() + sizeof(header) + static_cast<std::size_t>(i) * sizeof(request), sizeof(request));
            if (request.page < page_count_ && !state->payload->manager.resident(request.page)) {
                auto const found = std::ranges::find(state->pending, request.page);
                if (found == state->pending.end()) {
                    state->pending.push_back(request.page);
                    state->pending_priority[request.page] = request.priority;
                } else if (request.priority > state->pending_priority[request.page]) {
                    state->pending_priority[request.page] = request.priority;
                }
            }
        }
        header = {0, page_request_capacity_, 0, 0};
        std::memcpy(bytes.data(), &header, sizeof(header));
        for (auto physical : frame.references) {
            state->pool->drop(physical);
        }
        frame.references.clear();
        frame.published = false;
        return upload_rhi::error::ok;
    }

    upload_rhi::error uploaded_geometry::publish_page_frame(std::uint32_t slot) const {
        auto* state = page_state_.get();
        if (!state) {
            return upload_rhi::error::ok;
        }
        if (slot >= state->frames.size()) {
            return upload_rhi::error::invalid_argument;
        }
        auto& frame = state->frames[slot];
        if (frame.published) {
            return upload_rhi::error::not_ready;
        }
        auto bytes = frame.table->mapped();
        auto source = std::as_bytes(std::span(state->payload->table));
        if (bytes.size() != source.size()) {
            return upload_rhi::error::operation_failed;
        }
        std::memcpy(bytes.data(), source.data(), source.size());
        for (auto physical : state->slot_of) {
            if (physical != std::numeric_limits<std::uint32_t>::max()) {
                frame.references.push_back(physical);
                state->pool->retain(physical);
            }
        }
        frame.published = true;
        return upload_rhi::error::ok;
    }

    bool uploaded_geometry::has_pending_page_upload() const noexcept {
        return page_state_ && page_state_->uploading.has_value();
    }

    page_install_stats uploaded_geometry::collect_page_uploads() const {
        page_install_stats stats;
        auto* state = page_state_.get();
        if (!state) {
            return stats;
        }
        if (state->uploading) {
            auto& upload = *state->uploading;
            stats.in_flight = static_cast<std::uint32_t>(upload.plan.installs.size());
            stats.in_flight_bytes = upload.plan.installed_bytes;
            // A completed copy whose residency plan cannot be published may release its slots.
            auto abandon = [&](auto const& installs, auto const& assigned) {
                for (auto physical : assigned) {
                    state->reserved[physical] = 0;
                    state->release_owned(physical);
                }
                for (auto page : installs) {
                    state->slot_of[page] = std::numeric_limits<std::uint32_t>::max();
                    state->pending_priority[page] = 0;
                    auto& entry = state->payload->table[page];
                    entry.state = static_cast<std::uint32_t>(gpu_page_state::absent);
                    entry.offset = 0;
                }
                std::erase_if(state->pending, [&](auto page) {
                    return std::ranges::find(installs, page) != installs.end();
                });
                stats.abandoned = static_cast<std::uint32_t>(installs.size());
                stats.in_flight = 0;
                stats.in_flight_bytes = 0;
                state->uploading.reset(); // Invalidates the installs/assigned spans; publish stats first.
            };
            constexpr std::uint32_t poll_failure_limit = 3;
            auto const result = upload.ticket.poll();
            if (result == upload_rhi::error::device_lost) {
                stats.result = result;
            } else if (result == upload_rhi::error::ok) {
                auto const installs = upload.plan.installs;
                if (!state->payload->manager.apply(std::move(upload.plan))) {
                    abandon(installs, upload.assigned);
                    return stats;
                }
                for (std::size_t k = 0; k < installs.size(); ++k) {
                    auto const id = installs[k];
                    auto const physical = upload.assigned[k];
                    auto& entry = state->payload->table[id];
                    entry.state = static_cast<std::uint32_t>(gpu_page_state::resident);
                    entry.offset = physical * state->payload->slot_bytes;
                    state->slot_of[id] = physical;
                    state->pending_priority[id] = 0u;
                    state->reserved[physical] = 0;
                    state->pool->set_owner(physical, state, &page_state::evict_slot);
                }
                stats.installed = static_cast<std::uint32_t>(installs.size());
                stats.in_flight = 0;
                stats.in_flight_bytes = 0;
                state->uploading.reset();
                stats.result = state->transfers->collect();
            } else if (result != upload_rhi::error::not_ready) {
                // A retryable poll failure does not invalidate the copy. Count it so a
                // permanently unobservable ticket eventually returns its slots instead of
                // pinning arena capacity forever; the frame keeps rendering with LOD0.
                ++upload.failed_polls;
                if (upload.failed_polls >= poll_failure_limit) {
                    abandon(upload.plan.installs, upload.assigned);
                }
            }
        }
        std::erase_if(state->pending, [&](auto page) {
            if (!state->payload->manager.resident(page)) {
                return false;
            }
            state->pending_priority[page] = 0u;
            return true;
        });
        stats.pending = static_cast<std::uint32_t>(state->pending.size());
        return stats;
    }

    page_install_stats uploaded_geometry::install_requested_pages(std::uint64_t budget_bytes, std::uint32_t sequence,
                                                                  std::uint32_t slot, std::uint32_t max_install_pages) const noexcept {
        (void)sequence; // The request header's fourth word carries page misses, not the frame sequence.
        page_install_stats stats;
        auto* state = page_state_.get();
        if (!state) {
            return stats;
        }
        if (state->frames.empty()) {
            stats.result = configure_page_frames(1);
        }
        if (stats.result != upload_rhi::error::ok) {
            return stats;
        }
        if (slot >= state->frames.size()) {
            stats.result = upload_rhi::error::invalid_argument;
            return stats;
        }
        auto& frame = state->frames[slot];
        if (frame.published) {
            stats.result = upload_rhi::error::not_ready;
            return stats;
        }
        stats = collect_page_uploads();
        if (stats.result != upload_rhi::error::ok) {
            return stats;
        }
        stats.result = complete_page_frame(slot);
        if (stats.result != upload_rhi::error::ok) {
            return stats;
        }
        stats.requests = std::exchange(frame.collected, 0u);
        stats.overflow = std::exchange(frame.overflow, 0u);
        stats.page_misses = std::exchange(frame.page_misses, 0u);
        auto& table = state->payload->table;
        // One transaction can contain many requests. While it is in flight the
        // manager is unchanged, so its dependency-closed plan cannot become stale.
        if (!state->uploading && budget_bytes != 0 && max_install_pages != 0) {
            std::ranges::sort(state->pending, [state](std::uint32_t a, std::uint32_t b) {
                auto const priority_a = state->pending_priority[a];
                auto const priority_b = state->pending_priority[b];
                return priority_a != priority_b ? priority_a > priority_b : a < b;
            });
            deren::utility::vessel::vector<std::uint32_t> targets;
            std::optional<residency_plan> plan;
            for (auto page : state->pending) {
                if (targets.size() >= max_install_pages) {
                    break;
                }
                if (state->payload->manager.resident(page)) {
                    continue;
                }
                targets.push_back(page);
                auto trial = state->payload->manager.plan_requests(targets);
                if (!trial || trial->installed_bytes > budget_bytes) {
                    targets.pop_back();
                    continue;
                }
                plan = std::move(*trial);
            }
            if (plan) {
                // Logical eviction precedes physical reuse. Older snapshots still
                // protect their slots, so a full arena defers without any waiting.
                stats.evicted = static_cast<std::uint32_t>(plan->evictions.size());
                for (auto victim : plan->evictions) {
                    if (!state->payload->manager.evict(victim)) {
                        stats.result = upload_rhi::error::operation_failed;
                        return stats;
                    }
                    auto const slot = state->slot_of[victim];
                    state->slot_of[victim] = std::numeric_limits<std::uint32_t>::max();
                    state->pending_priority[victim] = 0u;
                    table[victim].state = static_cast<std::uint32_t>(gpu_page_state::absent);
                    table[victim].offset = 0;
                    if (slot != std::numeric_limits<std::uint32_t>::max()) {
                        state->reserved[slot] = 0;
                        state->release_owned(slot);
                    }
                }
                auto revised = state->payload->manager.plan_requests(targets);
                if (!revised) {
                    stats.result = upload_rhi::error::operation_failed;
                    return stats;
                }
                if (revised->installed_bytes > budget_bytes) {
                    // Evicting the trial victims can change the dependency closure. If the revised
                    // plan no longer fits the install budget, leave the targets absent and pending
                    // for a later frame instead of starting an over-budget transaction.
                    plan.reset();
                } else {
                    plan = std::move(*revised);
                }
                if (plan) {
                    deren::utility::vessel::vector<std::uint32_t> assigned;
                    bool acquired_slots = false;
                    std::uint32_t cross_evicted = 0;
                    if (!plan->installs.empty()) {
                        // Exclude this source from victim selection. Its resident dependencies are
                        // not listed in installing_pages, so reclaiming any of its pages could
                        // invalidate the dependency-closed plan before apply.
                        state->installing_pages = std::span<std::uint32_t const>{plan->installs};
                        acquired_slots = state->pool->acquire(static_cast<std::uint32_t>(plan->installs.size()), assigned, state, &cross_evicted);
                        state->installing_pages = {};
                    }
                    stats.evicted += cross_evicted;
                    if (acquired_slots) {
                        for (auto physical : assigned) {
                            state->reserved[physical] = 1;
                            state->owned_slots.push_back(physical);
                        }
                        if (!state->transfers) {
                            state->transfers = std::make_unique<buffer_transfer_service>(state->device);
                        }
                        namespace sync = upload_rhi::synchronization;
                        constexpr sync::scope published{
                            sync::stage::compute_shader | sync::stage::task_shader | sync::stage::mesh_shader |
                                sync::stage::fragment_shader | sync::stage::vertex_shader,
                            sync::access::read,
                        };
                        auto batch = state->transfers->make_batch();
                        auto arena = resource_handle<upload_rhi::buffer>::from_shared(
                            state->device, upload_rhi::shared_object<upload_rhi::buffer>{state->arena, state->arena->objects[0].buffer.get()});
                        for (std::size_t k = 0; k < plan->installs.size(); ++k) {
                            auto const install = plan->installs[k];
                            auto const physical = assigned[k];
                            auto const source_offset = state->payload->arena.layout.page_offset(install);
                            if (!source_offset || table[install].bytes > state->payload->slot_bytes) {
                                stats.result = upload_rhi::error::invalid_argument;
                                break;
                            }
                            auto bytes = std::span<std::byte const>{state->payload->arena.bytes.data() + *source_offset, table[install].bytes};
                            constexpr sync::scope no_before{};
                            stats.result = batch.upload(arena, static_cast<std::uint64_t>(physical) * state->payload->slot_bytes, bytes, no_before, published);
                            if (stats.result != upload_rhi::error::ok) {
                                break;
                            }
                        }
                        if (stats.result != upload_rhi::error::ok) {
                            for (auto physical : assigned) {
                                state->reserved[physical] = 0;
                                state->release_owned(physical);
                            }
                            return stats;
                        }
                        // Store bookkeeping before native submit can accept work.
                        state->uploading.emplace(page_state::upload{{}, std::move(*plan), std::move(assigned)});
                        auto ticket = state->transfers->submit(std::move(batch));
                        if (!ticket) {
                            stats.result = ticket.error();
                            for (auto physical : state->uploading->assigned) {
                                state->reserved[physical] = 0;
                                state->release_owned(physical);
                            }
                            state->uploading.reset();
                            return stats;
                        }
                        auto& upload = *state->uploading;
                        upload.ticket = std::move(*ticket);
                        stats.submitted = static_cast<std::uint32_t>(upload.plan.installs.size());
                        stats.in_flight = stats.submitted;
                        stats.bytes = upload.plan.installed_bytes;
                        stats.in_flight_bytes = stats.bytes;
                    }
                }
            }
        }
        for (auto page : state->pending) {
            if (!state->payload->manager.resident(page)) {
                table[page].state = static_cast<std::uint32_t>(gpu_page_state::requested);
            }
        }
        stats.pending = static_cast<std::uint32_t>(state->pending.size());
        auto bytes = frame.table->mapped();
        std::memcpy(bytes.data(), table.data(), table.size() * sizeof(gpu_page_table_entry));
        gpu_page_request_header const header{0, page_request_capacity_, 0, 0};
        std::memcpy(frame.requests->mapped().data(), &header, sizeof(header));
        return stats;
    }
} // namespace deren::engine::subtilis
