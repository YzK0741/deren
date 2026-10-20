#pragma once
// Included in the primary module after imports; these are private module-owned types.
namespace deren::engine::subtilis::builder_detail {
    template<class T> using vector = deren::utility::vessel::vector<T>;
    struct source {
        vector<std::uint8_t> vertices;
        vector<std::uint32_t> indices;
        vector<std::array<float,3>> positions;
        vector<std::array<float,5>> attributes;
        double scale = 0;
    };
    struct logical_node {
        vector<std::uint32_t> children, indices, source_triangles;
        vector<meshlet> records;
        float error = 0;
        std::uint32_t depth = 0;
        std::uint32_t lod0_first = 0;
    };
    void initialize_meshoptimizer();
    std::expected<meshlet, build_error> make_record(source const&, std::span<std::uint32_t const>, std::uint32_t first);
    std::expected<void, build_error> simplify(source const&, vector<logical_node>&, vector<std::uint32_t>&,
                                              build_statistics&, build_options const&);
}
