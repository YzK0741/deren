module;
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
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
            std::uint32_t first_index = 0, index_count = 0, producer = 0, consumer = 0;
            float center_x = 0, center_y = 0, center_z = 0, radius = 0;
        };
        static_assert(sizeof(wire_page_cluster) == 32u);
        struct wire_page_group {
            std::uint32_t fine_first = 0, fine_count = 0, coarse_first = 0, coarse_count = 0;
            float lod_x = 0, lod_y = 0, lod_z = 0, lod_radius = 0;
            float error = 0;
            std::array<std::byte, 12> padding{};
        };
        static_assert(sizeof(wire_page_group) == 48u);
        static_assert(offsetof(wire_page_group, error) == 32u);

        std::uint32_t page_slot_bytes(std::span<page_entry const> pages) {
            std::uint32_t bytes = 64;
            for (auto const& page : pages) {
                bytes = std::max(bytes, (page.bytes + 63u) & ~63u);
            }
            return bytes;
        }

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
            std::uint32_t const slot_bytes = page_slot_bytes(pages);
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
            auto const capacity = std::min<std::uint32_t>(std::max<std::uint32_t>(static_cast<std::uint32_t>(pages.size()), 1u), 256u);
            return std::make_shared<page_payload_block>(std::move(*packed), std::move(*table), capacity, std::move(*manager), slots, slot_bytes);
        }
    } // namespace

    double maximum_triangle_edge(meshlet const& record, std::span<std::uint8_t const> vertices,
                                 std::span<std::uint32_t const> indices, std::uint32_t stride) noexcept {
        double const invalid = std::numeric_limits<double>::infinity();
        if (stride < 12u || vertices.size() % stride != 0u || record.index_count == 0u || record.index_count % 3u != 0u ||
            static_cast<std::uint64_t>(record.first_index) + record.index_count > indices.size()) {
            return invalid;
        }
        double maximum_squared = 0.0;
        for (std::uint32_t t = 0u; t < record.index_count; t += 3u) {
            std::array<std::array<float, 3>, 3> p{};
            for (uint32_t c = 0u; c < 3u; ++c) {
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
            for (uint32_t c = 0u; c < 3u; ++c) {
                double squared = 0.0;
                for (uint32_t axis = 0u; axis < 3u; ++axis) {
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
        std::shared_ptr<page_payload_block> payload;
        upload_rhi::shared_object<upload_rhi::api_core> device;
        std::shared_ptr<render_resource::instances::generation const> arena;
        std::unique_ptr<buffer_transfer_service> transfers;
        deren::utility::vessel::vector<std::byte> initial_bytes;
        deren::utility::vessel::vector<std::uint32_t> slot_of;
        deren::utility::vessel::vector<std::uint32_t> free_slots;
    };

    std::expected<std::shared_ptr<uploaded_geometry const>, upload_rhi::error>
    upload_geometry(upload_rhi::shared_object<upload_rhi::api_core> const& device,
                    std::shared_ptr<built_geometry const> source, upload_rhi::buffer_flags flags, geometry_upload_mode mode,
                    std::uint64_t resident_page_bytes) {
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
            std::uint32_t pinned = 0;
            for (auto const& page : pages) {
                pinned += (page.flags & static_cast<std::uint16_t>(deren::asset::subtilis::page_flags::always_resident)) != 0u ? 1u : 0u;
            }
            auto const requested_slots = std::min<std::uint64_t>(
                std::max<std::uint64_t>(resident_page_bytes / page_slot_bytes(pages), 1u), pages.size());
            auto const slots = static_cast<std::uint32_t>(std::max<std::uint64_t>(requested_slots, pinned));
            auto made = make_page_payload(*source, slots);
            if (!made) {
                return std::unexpected(upload_rhi::error::invalid_argument);
            }
            page_payload = std::move(*made);
            page_pages = std::make_shared<page_state>();
            page_pages->payload = page_payload;
            page_pages->device = device;
            page_pages->initial_bytes.assign(static_cast<std::size_t>(slots) * page_payload->slot_bytes, std::byte{0});
            page_pages->slot_of.assign(pages.size(), UINT32_MAX);
            std::uint32_t next_slot = 0;
            for (std::uint32_t i = 0; i < pages.size(); ++i) {
                auto& entry = page_payload->table[i];
                if (!page_payload->manager.resident(i)) {
                    entry.offset = 0;
                    continue;
                }
                if (next_slot >= slots) {
                    return std::unexpected(upload_rhi::error::unsupported);
                }
                page_pages->slot_of[i] = next_slot;
                entry.offset = next_slot * page_payload->slot_bytes;
                auto const source_offset = page_payload->arena.layout.page_offset(i);
                if (!source_offset || entry.bytes > page_payload->slot_bytes) {
                    return std::unexpected(upload_rhi::error::invalid_argument);
                }
                std::memcpy(page_pages->initial_bytes.data() + static_cast<std::size_t>(next_slot) * page_payload->slot_bytes,
                            page_payload->arena.bytes.data() + *source_offset, entry.bytes);
                ++next_slot;
            }
            for (std::uint32_t slot = next_slot; slot < slots; ++slot) {
                page_pages->free_slots.push_back(slot);
            }
        }
        auto const request_count = std::size_t{2} + (cut_payload ? 3u : 0u) + (page_payload ? 3u : 0u);
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
            auto const storage_flags = upload_rhi::to_bits(upload_rhi::buffer_flag::device_address) |
                                       upload_rhi::to_bits(upload_rhi::buffer_flag::storage) |
                                       upload_rhi::to_bits(upload_rhi::buffer_flag::transfer_destination);
            append(render_resource::resource_id::scene_cut_groups, std::as_bytes(std::span(cut_payload->groups)), upload_rhi::buffer_usage::storage_coherent, storage_flags, cut_payload);
            append(render_resource::resource_id::scene_cut_references, std::as_bytes(std::span(cut_payload->references)), upload_rhi::buffer_usage::storage_coherent, storage_flags, cut_payload);
            append(render_resource::resource_id::scene_cut_clusters, std::as_bytes(std::span(cut_payload->clusters)), upload_rhi::buffer_usage::storage_coherent, storage_flags, cut_payload);
        }
        if (page_payload) {
            auto const storage_flags = upload_rhi::to_bits(upload_rhi::buffer_flag::device_address) |
                                       upload_rhi::to_bits(upload_rhi::buffer_flag::storage) |
                                       upload_rhi::to_bits(upload_rhi::buffer_flag::transfer_destination);
            append(render_resource::resource_id::scene_page_arena, std::as_bytes(std::span(page_pages->initial_bytes)), upload_rhi::buffer_usage::storage_coherent, storage_flags, page_payload);
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
            page_arena = snapshot.find(render_resource::resource_id::scene_page_arena);
            page_table = snapshot.find(render_resource::resource_id::scene_page_table);
            page_requests = snapshot.find(render_resource::resource_id::scene_page_requests);
            page_pages->arena = page_arena;
        }
        if (batch_upload) {
            namespace sync = upload_rhi::synchronization;
            // Immutable geometry is read by vertex input and the available shader stages.
            // The AS build verb records its own transfer-write -> build-read pre-barrier.
            auto const readers = sync::stage::vertex_input | sync::stage::vertex_shader | sync::stage::fragment_shader |
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
            if (auto result = submitted->wait(UINT64_MAX); result != upload_rhi::error::ok) {
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
            auto append = [&](meshlet const& record) {
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
                    append(c.record);
                }
            } else {
                edges.reserve(source->hierarchy().clusters().size());
                for (auto const& c : source->hierarchy().clusters()) {
                    append(c);
                }
            }
            std::sort(edges.begin(), edges.end(), [](auto const& a, auto const& b) {
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

    page_install_stats uploaded_geometry::install_requested_pages(std::uint64_t budget_bytes, std::uint32_t frame) const noexcept {
        page_install_stats stats;
        auto* state = page_state_.get();
        auto* request_buffer = this->page_requests();
        auto* table_buffer = this->page_table();
        if (!state || !state->payload || !state->arena || !state->device || !request_buffer || !table_buffer || page_count_ == 0u ||
            page_request_capacity_ == 0u || state->payload->table.size() != page_count_) {
            return stats;
        }
        // The request queue and page table are shared by all frame slots. Waiting
        // only for the paced slot does not finish the other slot's cut dispatch.
        // Drain before reading OR resetting the queue, including empty/budget-limited
        // calls. Keep this synchronous until requests have per-submission storage.
        state->device->wait_idle();
        auto request_bytes = request_buffer->mapped();
        auto table_bytes = table_buffer->mapped();
        constexpr std::size_t header_size = sizeof(gpu_page_request_header);
        constexpr std::size_t request_size = sizeof(gpu_page_request);
        constexpr std::size_t entry_size = sizeof(gpu_page_table_entry);
        auto const slot_bytes = state->payload->slot_bytes;
        if (request_bytes.size() < header_size + static_cast<std::size_t>(page_request_capacity_) * request_size ||
            table_bytes.size() < static_cast<std::size_t>(page_count_) * entry_size) {
            return stats;
        }

        auto read_entry = [&](std::uint32_t page) {
            gpu_page_table_entry entry{};
            std::memcpy(&entry, table_bytes.data() + static_cast<std::size_t>(page) * entry_size, entry_size);
            return entry;
        };
        auto write_entry = [&](std::uint32_t page, gpu_page_table_entry const& entry) {
            std::memcpy(table_bytes.data() + static_cast<std::size_t>(page) * entry_size, &entry, entry_size);
        };
        auto rebuild_free = [&] {
            state->free_slots.clear();
            for (std::uint32_t slot = 0; slot < state->payload->slots; ++slot) {
                bool used = false;
                for (std::uint32_t page = 0; page < page_count_; ++page) {
                    if (state->slot_of[page] == slot) {
                        used = true;
                        break;
                    }
                }
                if (!used) {
                    state->free_slots.push_back(slot);
                }
            }
        };

        gpu_page_request_header header{};
        std::memcpy(&header, request_bytes.data(), header_size);
        stats.requests = header.count;
        stats.overflow = header.overflow;
        auto const available = std::min(header.count, page_request_capacity_);
        if (header.count > page_request_capacity_) {
            stats.overflow = 1u;
        }

        namespace sync = upload_rhi::synchronization;
        sync::scope const published{
            sync::stage::compute_shader | sync::stage::task_shader | sync::stage::mesh_shader | sync::stage::fragment_shader |
                sync::stage::vertex_shader,
            sync::access::read,
        };
        auto* entries = request_bytes.data() + header_size;
        std::uint32_t pending = 0;
        for (std::uint32_t i = 0; i < available; ++i) {
            gpu_page_request request{};
            std::memcpy(&request, entries + static_cast<std::size_t>(i) * request_size, request_size);
            if (request.page >= page_count_) {
                continue;
            }
            auto entry = read_entry(request.page);
            if (entry.state == static_cast<std::uint32_t>(gpu_page_state::resident)) {
                continue;
            }

            bool installed = false;
            auto planned = state->payload->manager.plan_request(request.page);
            if (planned && planned->installed_bytes <= budget_bytes - stats.bytes) {
                auto plan = std::move(*planned);
                auto const installs = plan.installs;
                auto const installed_bytes = plan.installed_bytes;
                struct evicted_entry {
                    std::uint32_t page = 0, slot = 0;
                    gpu_page_table_entry entry{};
                };
                deren::utility::vessel::vector<evicted_entry> evicted;
                deren::utility::vessel::vector<std::uint32_t> assigned;
                for (auto page : plan.evictions) {
                    auto victim = read_entry(page);
                    if (state->slot_of[page] != UINT32_MAX) {
                        evicted.push_back({page, state->slot_of[page], victim});
                        state->free_slots.push_back(state->slot_of[page]);
                        state->slot_of[page] = UINT32_MAX;
                    }
                    victim.state = static_cast<std::uint32_t>(gpu_page_state::absent);
                    victim.offset = 0;
                    write_entry(page, victim);
                }
                auto rollback = [&] {
                    for (auto const& victim : evicted) {
                        write_entry(victim.page, victim.entry);
                        state->slot_of[victim.page] = victim.slot;
                    }
                    rebuild_free();
                };
                if (state->free_slots.size() >= installs.size() && (state->transfers || !installs.empty())) {
                    if (!state->transfers && !installs.empty()) {
                        state->transfers = std::make_unique<buffer_transfer_service>(state->device);
                    }
                    for (std::size_t k = 0; k < installs.size(); ++k) {
                        assigned.push_back(state->free_slots.back());
                        state->free_slots.pop_back();
                    }
                    if (!installs.empty()) {
                        auto batch = state->transfers->make_batch();
                        auto const arena_access = resource_handle<upload_rhi::buffer>::from_shared(
                            state->device, upload_rhi::shared_object<upload_rhi::buffer>{state->arena, state->arena->objects[0].buffer.get()});
                        bool copied = true;
                        for (std::size_t k = 0; k < installs.size(); ++k) {
                            auto const page = installs[k];
                            auto const page_entry = read_entry(page);
                            auto const source_offset = state->payload->arena.layout.page_offset(page);
                            if (!source_offset || page_entry.bytes > slot_bytes) {
                                copied = false;
                                break;
                            }
                            auto const source_bytes =
                                std::span<std::byte const>{state->payload->arena.bytes.data() + *source_offset, page_entry.bytes};
                            sync::scope no_before{};
                            auto const result =
                                batch.upload(arena_access, static_cast<std::uint64_t>(assigned[k]) * slot_bytes, source_bytes, no_before, published);
                            if (result != upload_rhi::error::ok) {
                                copied = false;
                                break;
                            }
                        }
                        if (copied) {
                            auto submitted = state->transfers->submit(std::move(batch));
                            copied = submitted && submitted->wait(UINT64_MAX) == upload_rhi::error::ok;
                        }
                        if (copied) {
                            auto applied = state->payload->manager.apply(std::move(plan));
                            if (applied) {
                                for (std::size_t k = 0; k < installs.size(); ++k) {
                                    auto page_entry = read_entry(installs[k]);
                                    page_entry.state = static_cast<std::uint32_t>(gpu_page_state::resident);
                                    page_entry.offset = assigned[k] * slot_bytes;
                                    state->slot_of[installs[k]] = assigned[k];
                                    write_entry(installs[k], page_entry);
                                }
                                stats.bytes += installed_bytes;
                                stats.installed += static_cast<std::uint32_t>(installs.size());
                                installed = true;
                            }
                        }
                        if (!installed) {
                            state->device->wait_idle();
                        }
                    }
                }
                if (!installed) {
                    rollback();
                }
            }
            if (installed) {
                continue;
            }

            entry.state = static_cast<std::uint32_t>(gpu_page_state::requested);
            write_entry(request.page, entry);
            bool duplicate = false;
            for (std::uint32_t j = 0; j < pending; ++j) {
                gpu_page_request queued{};
                std::memcpy(&queued, entries + static_cast<std::size_t>(j) * request_size, request_size);
                if (queued.page == request.page) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                std::memcpy(entries + static_cast<std::size_t>(pending) * request_size, &request, request_size);
                ++pending;
            }
        }

        header.count = pending;
        header.capacity = page_request_capacity_;
        header.overflow = 0u;
        header.frame = frame;
        std::memcpy(request_bytes.data(), &header, header_size);
        stats.pending = pending;
        return stats;
    }
} // namespace deren::engine::subtilis
