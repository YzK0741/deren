module;
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>
module deren.engine.subtilis.upload;
import deren.engine.pass.chain;
import deren.engine.pass.buffer_storage;
import deren.engine.render_resource;

namespace deren::engine::subtilis {
    std::expected<std::shared_ptr<uploaded_geometry const>, upload_rhi::error>
    upload_geometry(upload_rhi::shared_object<upload_rhi::api_core> const& device,
                    std::shared_ptr<built_geometry const> source, upload_rhi::buffer_flags flags) {
        if (!device || !source || source->vertices().empty() || source->indices().empty() || !source->lod0_index_count()) {
            return std::unexpected(upload_rhi::error::invalid_argument);
        }
        namespace managed = render_resource::instances;
        auto resources = std::make_shared<pass::pass_chain>();
        auto& provider = resources->emplace<pass::resource_storage_pass>();
        std::array<managed::request, 2> requests;
        std::array<std::span<std::byte const>, 2> bytes{std::as_bytes(source->vertices()), std::as_bytes(source->indices())};
        for (std::size_t i = 0; i < requests.size(); ++i) {
            auto& request = requests[i];
            request.id = i == 0 ? render_resource::resource_id::scene_geometry_vertices : render_resource::resource_id::scene_geometry_indices;
            request.type = managed::kind::buffer;
            request.life = managed::lifetime::scene;
            request.buffer = {.size = bytes[i].size(), .usage = i == 0 ? upload_rhi::buffer_usage::vertex : upload_rhi::buffer_usage::index, .flags = flags};
            request.initial_data = std::shared_ptr<std::byte const>{source, bytes[i].data()};
            request.initial_data_size = bytes[i].size();
            request.initialization_key = source;
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
        if (auto status = resources->prepare_resources(device, {}, admit); status != upload_rhi::error::ok) {
            return std::unexpected(status);
        }
        auto snapshot = resources->resources().snapshot();
        auto vertices = snapshot.find(requests[0].id);
        auto indices = snapshot.find(requests[1].id);
        for (auto& request : requests) {
            request.initial_data.reset();
        }
        provider.configure(requests);
        return std::shared_ptr<uploaded_geometry const>{new uploaded_geometry(std::move(resources), std::move(source), std::move(vertices), std::move(indices))};
    }
} // namespace deren::engine::subtilis
