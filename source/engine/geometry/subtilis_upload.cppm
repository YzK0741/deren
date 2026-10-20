module;
#include <expected>
#include <memory>
#include <utility>
export module deren.engine.subtilis.upload;
import deren.engine.subtilis.builder;
import deren.promise.rhi;
import deren.engine.render_resource.instances;

export namespace deren::engine::subtilis {
    namespace upload_rhi = deren::promise::rhi;
    class uploaded_geometry;
    [[nodiscard]] std::expected<std::shared_ptr<uploaded_geometry const>, upload_rhi::error>
    upload_geometry(upload_rhi::shared_object<upload_rhi::api_core> const& device,
                    std::shared_ptr<built_geometry const> source,
                    upload_rhi::buffer_flags flags = upload_rhi::to_bits(upload_rhi::buffer_flag::device_address));
    /// One immutable CPU hierarchy and its complete GPU payload. Borrowed buffers
    /// are valid while this generation (or a view's lease of it) is retained.
    class uploaded_geometry {
        std::shared_ptr<void const> resources_;
        std::shared_ptr<built_geometry const> source_;
        std::shared_ptr<render_resource::instances::generation const> vertices_, indices_;
        friend std::expected<std::shared_ptr<uploaded_geometry const>, upload_rhi::error>
        upload_geometry(upload_rhi::shared_object<upload_rhi::api_core> const&, std::shared_ptr<built_geometry const>, upload_rhi::buffer_flags);
        uploaded_geometry(std::shared_ptr<void const> resources, std::shared_ptr<built_geometry const> source,
                          std::shared_ptr<render_resource::instances::generation const> vertices,
                          std::shared_ptr<render_resource::instances::generation const> indices)
            : resources_(std::move(resources))
            , source_(std::move(source))
            , vertices_(std::move(vertices))
            , indices_(std::move(indices)) {
        }

    public:
        uploaded_geometry(uploaded_geometry const&) = delete;
        uploaded_geometry& operator=(uploaded_geometry const&) = delete;
        [[nodiscard]] built_geometry const& source() const noexcept {
            return *source_;
        }
        [[nodiscard]] upload_rhi::buffer* vertices() const noexcept {
            return vertices_->objects[0].buffer.get();
        }
        [[nodiscard]] upload_rhi::buffer* indices() const noexcept {
            return indices_->objects[0].buffer.get();
        }
    };
} // namespace deren::engine::subtilis
