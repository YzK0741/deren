#pragma once
// Module-visible implementation details for the converter and its focused tests.
// These names are not a stable consumer API.
export namespace deren::asset::subtilis::builder_detail {
    struct primitive_scale {
        array<double, 3> origin{};
        double diagonal = 0;
    };
    struct cluster_input {
        std::span<uint32_t const> indices;
    };
    struct frontier {
        vector<uint32_t> indices;
        vector<cluster_record> clusters;
    };
    enum class rejection_reason { none,
                                  no_progress,
                                  unsafe_topology,
                                  zero_scale,
                                  numeric_error,
                                  boundary_changed };
    struct reduction_candidate {
        frontier coarse;
        float local_error = 0;
        rejection_reason reason = rejection_reason::no_progress;
        bool accepted() const noexcept {
            return reason == rejection_reason::none;
        }
    };
    struct topology {
        vector<uint32_t> position_class, attribute_class;
        vector<unsigned char> seams, unsafe_vertices;
    };
    struct boundary_signature {
        vector<array<uint32_t, 2>> geometric, attribute, class_components;
        vector<uint32_t> boundary_classes, components;
        bool valid = false;
        bool operator==(boundary_signature const& rhs) const {
            return valid == rhs.valid && geometric == rhs.geometric && attribute == rhs.attribute && components.size() == rhs.components.size();
        }
    };
    primitive_scale measure_scale(source_geometry const&);
    topology analyze_topology(source_geometry const&);
    boundary_signature boundary_of(source_geometry const&, std::span<uint32_t const>);
    boundary_signature boundary_of(source_geometry const&, std::span<uint32_t const>, topology const&);
    bool preserves_region(source_geometry const&, topology const&, boundary_signature const&, std::span<uint32_t const>);
    result<frontier> cluster_lod0(source_geometry const&, build_options const&);
    result<frontier> cluster_indices(source_geometry const&, std::span<uint32_t const>, build_options const&, primitive_scale const&);
    result<vector<vector<uint32_t>>> partition_clusters(source_geometry const&, std::span<cluster_input const>, build_options const&, primitive_scale const&);
    result<reduction_candidate> reduce_group(source_geometry const&, std::span<cluster_input const>, build_options const&, primitive_scale const&, topology const* cached_topology = nullptr);
    void initialize_meshoptimizer();
    result<sphere> geometry_sphere(source_geometry const&, std::span<uint32_t const>);
} // namespace deren::asset::subtilis::builder_detail
