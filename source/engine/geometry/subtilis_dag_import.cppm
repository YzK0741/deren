module;
#include <cstddef>
#include <cstdint>
#include <cstring>
export module deren.engine.subtilis.dag_import;
export import deren.vstd;
export import deren.engine.subtilis.dag_asset;
import deren.asset.subtilis.dag;
import deren.engine.subtilis;
export namespace deren::engine::subtilis {
    enum class import_failure { invalid_payload,
                                profile_mismatch,
                                material_not_opaque,
                                source_mismatch,
                                invalid_dag,
                                lod0_only,
                                capacity_item,
                                capacity_table,
                                count };
    /// The payload format only bounds each section by `limits::max_records` (16M records), which a
    /// crafted payload can reach before the leaf admission budget rejects it. Every legal hierarchy's
    /// cluster, group and reference tables stay proportional to its leaves, so import caps them at
    /// eight times `visible_meshlet_capacity` and refuses anything larger as `capacity_item`.
    inline constexpr uint64_t import_record_limit = 8 * uint64_t(visible_meshlet_capacity);
    struct import_input {
        std::span<uint8_t const> payload_bytes;
        uint32_t version = 1;
        std::string_view profile = "shared-vertices";
        bool profile_attributes = true, material_opaque = true;
        std::span<uint8_t const> vertex_bytes;
        uint32_t vertex_stride = 80, vertex_count = 0;
        std::span<uint8_t const> index_bytes;
        uint32_t index_width = 4, index_count = 0;
    };
    using import_digest = std::array<std::byte, 32>;
    struct import_key {
        import_digest source_hash{}, payload_hash{};
        auto operator<=>(import_key const&) const = default;
    };
    class imported_asset;
    std::expected<imported_asset, import_failure> import_payload(import_input const&);
    class imported_asset {
        dag_asset hierarchy_;
        utility::vessel::vector<uint8_t> vertices_;
        utility::vessel::vector<uint32_t> indices_;
        // The version-1.1 page table is kept with the immutable hierarchy so a residency manager can
        // address the same element ranges the payload validated. Empty for a version-1.0 payload.
        utility::vessel::vector<deren::asset::subtilis::page_entry> pages_;
        utility::vessel::vector<uint32_t> page_dependencies_;
        uint32_t vertex_count_ = 0;
        /// the source record's byte stride: 80 after the TANGENT was appended to the engine's interleaved
        /// vertex (`gltf_loader.cpp`), carried rather than assumed so the payload's bytes and the mesh fetch's
        /// stride cannot disagree (the vertices below are the SOURCE bytes, not a re-pack).
        uint32_t vertex_stride_ = 0;
        import_key key_;
        import_digest build_hash_;
        imported_asset(dag_asset hierarchy, utility::vessel::vector<uint8_t> vertices, utility::vessel::vector<uint32_t> indices, utility::vessel::vector<deren::asset::subtilis::page_entry> pages, utility::vessel::vector<uint32_t> page_dependencies, uint32_t count, uint32_t stride, import_key key, import_digest build_hash)
            : hierarchy_(std::move(hierarchy))
            , vertices_(std::move(vertices))
            , indices_(std::move(indices))
            , pages_(std::move(pages))
            , page_dependencies_(std::move(page_dependencies))
            , vertex_count_(count)
            , vertex_stride_(stride)
            , key_(key)
            , build_hash_(build_hash) {
        }
        friend std::expected<imported_asset, import_failure> import_payload(import_input const&);

    public:
        imported_asset(imported_asset const&) = delete ("imported_asset is non-copyable: pass it by reference or construct a new instance");
        imported_asset& operator=(imported_asset const&) = delete ("imported_asset is non-copyable: modify the existing instance instead");
        imported_asset(imported_asset&&) = default;
        imported_asset& operator=(imported_asset&&) = default;
        dag_asset const& hierarchy() const noexcept {
            return hierarchy_;
        }
        std::span<uint8_t const> vertices() const noexcept {
            return vertices_;
        }
        std::span<uint32_t const> indices() const noexcept {
            return indices_;
        }
        std::span<deren::asset::subtilis::page_entry const> pages() const noexcept {
            return {pages_.data(), pages_.size()};
        }
        std::span<uint32_t const> page_dependencies() const noexcept {
            return {page_dependencies_.data(), page_dependencies_.size()};
        }
        std::span<meshlet const> lod0_clusters() const noexcept {
            return hierarchy_.lod0_clusters();
        }
        uint32_t vertex_stride() const noexcept {
            return vertex_stride_;
        }
        uint32_t vertex_count() const noexcept {
            return vertex_count_;
        }
        uint32_t lod0_index_count() const noexcept {
            return hierarchy_.lod0_index_count();
        }
        import_key const& cache_key() const noexcept {
            return key_;
        }
        import_digest const& build_hash() const noexcept {
            return build_hash_;
        }
    };
    inline std::expected<imported_asset, import_failure> import_payload(import_input const& input) {
        namespace wire = deren::asset::subtilis;
        if (!input.profile_attributes) {
            return std::unexpected(import_failure::profile_mismatch);
        }
        if (!input.material_opaque) {
            return std::unexpected(import_failure::material_not_opaque);
        }
        if (input.version != 1 || input.profile != "shared-vertices" || input.payload_bytes.empty()) {
            return std::unexpected(import_failure::invalid_payload);
        }
        wire::limits limits;
        auto bytes = std::as_bytes(input.payload_bytes);
        auto decoded = wire::decode(bytes, limits);
        if (!decoded) {
            return std::unexpected(import_failure::invalid_payload);
        }
        // Bound the decoded hierarchy tables from the leaf budget before structural validation and
        // make() build their per-cluster copies: a legal payload's tables are proportional to its
        // leaves, so a larger table is a capacity failure rather than a slow rejection.
        if (decoded->clusters.size() > import_record_limit || decoded->groups.size() > import_record_limit || decoded->references.size() > import_record_limit) {
            return std::unexpected(import_failure::capacity_item);
        }
        auto structure = wire::validate_structure(*decoded, limits);
        if (!structure) {
            return std::unexpected(import_failure::invalid_dag);
        }
        if (decoded->leaves.size() > visible_meshlet_capacity) {
            return std::unexpected(import_failure::capacity_item);
        }
        if (decoded->groups.empty()) {
            return std::unexpected(import_failure::lod0_only);
        }
        uint64_t vertex_bytes = uint64_t(input.vertex_count) * input.vertex_stride, index_bytes = uint64_t(input.index_count) * input.index_width;
        if (input.vertex_count > limits.max_records || input.index_count > limits.max_records || vertex_bytes > limits.max_input_bytes || index_bytes > limits.max_input_bytes - vertex_bytes) {
            return std::unexpected(import_failure::capacity_item);
        }
        if (input.vertex_stride != 80 || !input.vertex_count || !input.index_count || input.index_count % 3 || (input.index_width != 2 && input.index_width != 4) || vertex_bytes > input.vertex_bytes.size() || index_bytes > input.index_bytes.size()) {
            return std::unexpected(import_failure::source_mismatch);
        }
        wire::source_geometry source;
        source.positions.resize(input.vertex_count);
        source.normals.resize(input.vertex_count);
        source.uv0.resize(input.vertex_count);
        source.indices.resize(input.index_count);
        for (uint32_t id = 0; id < input.vertex_count; ++id) {
            auto* vertex = input.vertex_bytes.data() + size_t(id) * input.vertex_stride;
            std::memcpy(source.positions[id].data(), vertex, 12);
            std::memcpy(source.normals[id].data(), vertex + 12, 12);
            std::memcpy(source.uv0[id].data(), vertex + 24, 8);
        }
        for (uint32_t id = 0; id < input.index_count; ++id) {
            auto* index = input.index_bytes.data() + size_t(id) * input.index_width;
            if (input.index_width == 2) {
                uint16_t value = 0;
                std::memcpy(&value, index, 2);
                source.indices[id] = value;
            } else {
                std::memcpy(&source.indices[id], index, 4);
            }
        }
        auto source_hash = wire::source_digest(source);
        if (!source_hash || *source_hash != decoded->source_hash) {
            return std::unexpected(import_failure::source_mismatch);
        }
        for (auto index : decoded->indices) {
            if (index >= input.vertex_count) {
                return std::unexpected(import_failure::source_mismatch);
            }
        }
        uint64_t finest_indices = 0;
        for (auto id : decoded->leaves) {
            finest_indices += decoded->clusters[id].index_count;
        }
        if (finest_indices != input.index_count) {
            return std::unexpected(import_failure::source_mismatch);
        }
        utility::vessel::vector<dag_cluster> clusters;
        utility::vessel::vector<dag_group> groups;
        clusters.reserve(decoded->clusters.size());
        groups.reserve(decoded->groups.size());
        for (auto const& c : decoded->clusters) {
            meshlet record{};
            record.first_index = c.first_index;
            record.index_count = c.index_count;
            record.center_x = c.geometry.x;
            record.center_y = c.geometry.y;
            record.center_z = c.geometry.z;
            record.radius = c.geometry.radius;
            record.cone_cos = 0; // no normal-cone claim; all other cone/padding fields are zero
            clusters.push_back({record, c.producer, c.consumer});
        }
        for (auto const& g : decoded->groups) {
            groups.push_back({{g.fine_first, g.fine_count}, {g.coarse_first, g.coarse_count}, {g.lod.x, g.lod.y, g.lod.z, g.lod.radius}, g.error});
        }
        auto hierarchy = dag_asset::make(clusters, groups, decoded->references, decoded->leaves, decoded->roots, input.index_count, static_cast<uint32_t>(decoded->indices.size()));
        if (!hierarchy) {
            return std::unexpected(hierarchy.error() == dag_error::capacity ? import_failure::capacity_item : import_failure::invalid_dag);
        }
        import_key key{*source_hash, wire::sha256(bytes)};
        return imported_asset(std::move(*hierarchy), utility::vessel::vector<uint8_t>(input.vertex_bytes.begin(), input.vertex_bytes.begin() + static_cast<size_t>(vertex_bytes)), std::move(decoded->indices), std::move(decoded->pages), std::move(decoded->page_dependencies), input.vertex_count, input.vertex_stride, key, decoded->build_hash);
    }
    [[nodiscard]] inline std::expected<void, import_failure> admit_table(imported_asset const& asset, uint64_t used, uint32_t capacity) noexcept {
        if (used > capacity || asset.lod0_clusters().size() > capacity - used) {
            return std::unexpected(import_failure::capacity_table);
        }
        return {};
    }
} // namespace deren::engine::subtilis
