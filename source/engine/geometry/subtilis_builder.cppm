module;
#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>
export module deren.engine.subtilis.builder;
import deren.engine.meshlet;
import deren.engine.subtilis.asset;
import deren.utility.shared;

export namespace deren::engine::subtilis {
    struct build_input {
        meshlet_build_input geometry;
        std::uint32_t normal_offset = 12, uv_offset = 24;
    };
    struct build_options {
        std::uint32_t group_size = 8, max_depth = 16;
        float target_ratio = .5f, max_error = .01f, normal_weight = .1f, uv_weight = .01f;
        std::uint64_t max_output_bytes = 256ull * 1024 * 1024;
    };
    enum class build_error { invalid_input,
                             invalid_options,
                             capacity,
                             invalid_geometry,
    };
    struct build_statistics {
        std::uint32_t nodes = 0, roots = 0, clusters = 0, depth = 0;
        std::uint64_t lod0_triangles = 0, coarse_triangles = 0, payload_bytes = 0, source_hash = 0;
        std::uint32_t rejected_topology = 0, rejected_reduction = 0, rejected_budget = 0;
        std::uint32_t schema_version = 1;
        std::uint32_t normal_offset = 12, uv_offset = 24;
        deren::utility::vessel::vector<std::uint64_t> level_triangles;
    };
    inline constexpr std::string_view builder_dependency_revision = "9e1f07b159d3cb777f1c67ed31fc11fd117986f4";
    /// One immutable source/payload/hierarchy generation. Not a disk format or GPU ABI.
    class built_geometry {
    public:
        built_geometry(built_geometry const&) = delete;
        built_geometry& operator=(built_geometry const&) = delete;
        built_geometry(built_geometry&& other) noexcept
            : vertices_(std::move(other.vertices_))
            , indices_(std::move(other.indices_))
            , hierarchy_(std::move(other.hierarchy_))
            , options_(other.options_)
            , statistics_(std::exchange(other.statistics_, {}))
            , stride_(std::exchange(other.stride_, 0))
            , vertex_count_(std::exchange(other.vertex_count_, 0))
            , lod0_count_(std::exchange(other.lod0_count_, 0)) {
        }
        built_geometry& operator=(built_geometry&& other) noexcept {
            if (this != &other) {
                vertices_ = std::move(other.vertices_);
                indices_ = std::move(other.indices_);
                hierarchy_ = std::move(other.hierarchy_);
                options_ = other.options_;
                statistics_ = std::exchange(other.statistics_, {});
                stride_ = std::exchange(other.stride_, 0);
                vertex_count_ = std::exchange(other.vertex_count_, 0);
                lod0_count_ = std::exchange(other.lod0_count_, 0);
            }
            return *this;
        }
        [[nodiscard]] std::span<std::uint8_t const> vertices() const noexcept {
            return vertices_;
        }
        [[nodiscard]] std::span<std::uint32_t const> indices() const noexcept {
            return indices_;
        }
        [[nodiscard]] asset const& hierarchy() const noexcept {
            return hierarchy_;
        }
        [[nodiscard]] build_options const& options() const noexcept {
            return options_;
        }
        [[nodiscard]] build_statistics const& statistics() const noexcept {
            return statistics_;
        }
        [[nodiscard]] std::uint32_t vertex_stride() const noexcept {
            return stride_;
        }
        [[nodiscard]] std::uint32_t vertex_count() const noexcept {
            return vertex_count_;
        }
        [[nodiscard]] std::uint32_t lod0_index_count() const noexcept {
            return lod0_count_;
        }

    private:
        friend std::expected<built_geometry, build_error> build(build_input const&, build_options const&);
        built_geometry(deren::utility::vessel::vector<std::uint8_t>&& vertices,
                       deren::utility::vessel::vector<std::uint32_t>&& indices, asset&& hierarchy,
                       build_options const& options, build_statistics&& statistics,
                       std::uint32_t stride, std::uint32_t vertex_count, std::uint32_t lod0_count)
            : vertices_(std::move(vertices))
            , indices_(std::move(indices))
            , hierarchy_(std::move(hierarchy))
            , options_(options)
            , statistics_(std::move(statistics))
            , stride_(stride)
            , vertex_count_(vertex_count)
            , lod0_count_(lod0_count) {
        }
        deren::utility::vessel::vector<std::uint8_t> vertices_;
        deren::utility::vessel::vector<std::uint32_t> indices_;
        asset hierarchy_;
        build_options options_;
        build_statistics statistics_;
        std::uint32_t stride_, vertex_count_, lod0_count_;
    };
    /// @brief build a hierarchical cluster LOD from LOD0 geometry and options.
    /// @details The feature overview, the measured numbers and the document index are in @ref md_docs_2subtilis.
    [[nodiscard]] std::expected<built_geometry, build_error> build(build_input const& input, build_options const& options = {});
} // namespace deren::engine::subtilis

#include "subtilis_builder_internal.hpp"
