export module deren.asset.subtilis.builder;
export import deren.asset.subtilis.dag;
#include "builder_internal.hpp"
export namespace deren::asset::subtilis {
    struct level_statistics {
        uint64_t triangles = 0, clusters = 0, groups = 0, roots = 0;
    };
    struct build_report {
        vector<level_statistics> levels;
        array<uint64_t, 6> rejected{};
        uint32_t rounds = 0, cross_group_repartitions = 0;
        bool lod0_only = false;
        uint64_t known_work_peak_bytes = 0;
        double wall_ms = 0;
    };
    struct build_result {
        validated_asset asset;
        build_report report;
    };
    result<build_result> build_serial(source_geometry, build_options const&, build_limits const&);
    result<build_result> build(source_geometry, build_options const&, execution_options const&);
} // namespace deren::asset::subtilis
