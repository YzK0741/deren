module;
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <utility>
export module deren.engine.subtilis.upload;
import deren.engine.subtilis.builder;
import deren.engine.meshlet;
import deren.utility.shared;
import deren.promise.rhi;
import deren.engine.render_resource.instances;

export namespace deren::engine::subtilis {
    namespace upload_rhi = deren::promise::rhi;
    /// Conservative object-space maximum edge; malformed/nonfinite geometry returns infinity.
    [[nodiscard]] double maximum_triangle_edge(meshlet const&, std::span<std::uint8_t const> vertices,
                                               std::span<std::uint32_t const> indices, std::uint32_t stride) noexcept;
    struct triangle_edge_record {
        std::uint32_t first_index, index_count;
        std::int32_t base_vertex;
        double maximum_edge;
    };
    enum class geometry_upload_mode { automatic,
                                      synchronous,
                                      batched,
    };
    class uploaded_geometry;
    /// The source must remain immutable through every alias after publication;
    /// do not move-assign or modify it through a separately retained mutable owner.
    /// Returns only after both GPU buffers are ready. Automatic selects a single transfer batch
    /// when owning core, submission and scoped synchronization are available; otherwise it uses
    /// synchronous creation. Forced batched mode refuses missing capabilities; failures are propagated.
    /// Separate batched uploads do not share the backend's initial-content cache: reuse the returned owner.
    /// @details The feature overview, the measured numbers and the document index are in @ref md_docs_2subtilis.
    [[nodiscard]] std::expected<std::shared_ptr<uploaded_geometry const>, upload_rhi::error>
    upload_geometry(upload_rhi::shared_object<upload_rhi::api_core> const& device,
                    std::shared_ptr<built_geometry const> source,
                    upload_rhi::buffer_flags flags = upload_rhi::to_bits(upload_rhi::buffer_flag::device_address),
                    geometry_upload_mode mode = geometry_upload_mode::automatic);
    /// One immutable CPU hierarchy and its complete GPU payload. Borrowed buffers
    /// are valid while this generation (or a view's lease of it) is retained.
    class uploaded_geometry {
        struct construction_key {
            explicit construction_key() = default;
        };
        std::shared_ptr<void const> resources_;
        std::shared_ptr<built_geometry const> source_;
        deren::utility::vessel::vector<triangle_edge_record> triangle_edges_;
        std::shared_ptr<render_resource::instances::generation const> vertices_, indices_;
        friend std::expected<std::shared_ptr<uploaded_geometry const>, upload_rhi::error>
        upload_geometry(upload_rhi::shared_object<upload_rhi::api_core> const&, std::shared_ptr<built_geometry const>, upload_rhi::buffer_flags, geometry_upload_mode);

    public:
        uploaded_geometry(construction_key /*unused*/, std::shared_ptr<void const> resources, std::shared_ptr<built_geometry const> source,
                          std::shared_ptr<render_resource::instances::generation const> vertices,
                          std::shared_ptr<render_resource::instances::generation const> indices,
                          deren::utility::vessel::vector<triangle_edge_record> edges)
            : resources_(std::move(resources))
            , source_(std::move(source))
            , triangle_edges_(std::move(edges))
            , vertices_(std::move(vertices))
            , indices_(std::move(indices)) {
        }

        uploaded_geometry(uploaded_geometry const&) = delete;
        uploaded_geometry& operator=(uploaded_geometry const&) = delete;
        [[nodiscard]] double triangle_edge(meshlet const& record) const noexcept;
        [[nodiscard]] built_geometry const& source() const noexcept {
            return *source_;
        }
        [[nodiscard]] std::shared_ptr<built_geometry const> const& source_owner() const noexcept {
            return source_;
        }
        [[nodiscard]] upload_rhi::buffer* vertices() const noexcept {
            return vertices_->objects[0].buffer.get();
        }
        [[nodiscard]] upload_rhi::buffer* indices() const noexcept {
            return indices_->objects[0].buffer.get();
        }
    };
} // namespace deren::engine::subtilis
