module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
module deren.engine.subtilis.upload;
import deren.engine.pass.chain;
import deren.engine.pass.buffer_storage;
import deren.engine.render_resource;
import deren.engine.buffer_transfer;

namespace deren::engine::subtilis {
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

    std::expected<std::shared_ptr<uploaded_geometry const>, upload_rhi::error>
    upload_geometry(upload_rhi::shared_object<upload_rhi::api_core> const& device,
                    std::shared_ptr<built_geometry const> source, upload_rhi::buffer_flags flags, geometry_upload_mode mode) {
        if (mode != geometry_upload_mode::automatic && mode != geometry_upload_mode::synchronous && mode != geometry_upload_mode::batched) {
            return std::unexpected(upload_rhi::error::invalid_argument);
        }
        if (!device || !source || source->vertices().empty() || source->indices().empty() || !source->lod0_index_count()) {
            return std::unexpected(upload_rhi::error::invalid_argument);
        }
        auto* scopes = upload_rhi::query_extension<upload_rhi::scoped_synchronization>(*device);
        bool const available = device.owns_lifetime() && scopes && upload_rhi::query_extension<upload_rhi::command_submission>(*device);
        if (mode == geometry_upload_mode::batched && !available) {
            return std::unexpected(upload_rhi::error::unsupported);
        }
        bool const batch_upload = mode != geometry_upload_mode::synchronous && available;
        namespace managed = render_resource::instances;
        auto resources = std::allocate_shared<pass::pass_chain>(deren::utility::memory::allocator<pass::pass_chain>{});
        auto& provider = resources->emplace<pass::resource_storage_pass>();
        std::array<managed::request, 2> requests;
        std::array<std::span<std::byte const>, 2> bytes{std::as_bytes(source->vertices()), std::as_bytes(source->indices())};
        for (std::size_t i = 0; i < requests.size(); ++i) {
            auto& request = requests[i];
            request.id = i == 0 ? render_resource::resource_id::scene_geometry_vertices : render_resource::resource_id::scene_geometry_indices;
            request.type = managed::kind::buffer;
            request.life = managed::lifetime::scene;
            request.buffer = {.size = bytes[i].size(), .usage = i == 0 ? upload_rhi::buffer_usage::vertex : upload_rhi::buffer_usage::index, .flags = flags};
            if (!batch_upload) {
                request.initial_data = std::shared_ptr<std::byte const>{source, bytes[i].data()};
                request.initial_data_size = bytes[i].size();
                request.initialization_key = source;
            }
            request.dependencies.push_back(source);
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
        auto vertices = snapshot.find(requests[0].id);
        auto indices = snapshot.find(requests[1].id);
        if (batch_upload) {
            namespace sync = upload_rhi::synchronization;
            // Immutable geometry is read by vertex input and the available shader stages.
            // The AS build verb separately owns its transfer-write -> build-read pre-barrier.
            auto const readers = sync::stage::vertex_input | sync::stage::vertex_shader | sync::stage::fragment_shader |
                                 sync::stage::compute_shader | sync::stage::task_shader | sync::stage::mesh_shader | sync::stage::ray_tracing;
            auto const supported = static_cast<std::uint32_t>(scopes->supported_stages());
            sync::scope const published{static_cast<sync::stage>(supported & static_cast<std::uint32_t>(readers)), sync::access::read};
            buffer_transfer_service transfers(device);
            auto batch = transfers.make_batch();
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                auto generation = i == 0 ? vertices : indices;
                auto* buffer = generation->objects[0].buffer.get();
                auto lease = resource_lease<upload_rhi::buffer>::from_shared(device, upload_rhi::shared_object<upload_rhi::buffer>{generation, buffer});
                // clang-tidy 23 mistakes the required separator after {} for an initializer trailing comma.
                if (auto result = batch.upload(std::move(lease), 0, bytes[i], {}, published); result != upload_rhi::error::ok) { // NOLINT(readability-trailing-comma)
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
                                                       std::move(resources), std::move(source), std::move(vertices), std::move(indices), std::move(edges));
    }
} // namespace deren::engine::subtilis
