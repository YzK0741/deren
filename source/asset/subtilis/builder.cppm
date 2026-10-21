export module deren.asset.subtilis.builder;
export import deren.asset.subtilis.dag;
#include "builder_internal.hpp"
export namespace deren::asset::subtilis {
    struct level_statistics {
        uint64_t triangles = 0, clusters = 0, groups = 0, roots = 0;
    };
    // One structural profiling row. cpu_ms is accumulated worker-task elapsed time (thread time,
    // not CPU cycles): parallel tasks can add up to more than the wall clock, which is why it is
    // reported separately and never used as a speed threshold. `inclusive` rows overlap their
    // child rows and are excluded from build_report::accumulated_ms.
    struct stage_profile {
        string name;
        double cpu_ms = 0;
        bool inclusive = false;
        uint64_t count = 0;
    };
    // Execution metadata only: thread count, timing, workspace and stage statistics never enter the
    // wire payload, the build metadata string or the source/build identities.
    struct build_report {
        vector<level_statistics> levels;
        array<uint64_t, 6> rejected{};
        uint32_t rounds = 0, cross_group_repartitions = 0;
        bool lod0_only = false;
        uint64_t known_work_peak_bytes = 0;
        uint64_t known_work_limit_bytes = 0;
        uint32_t threads = 1;
        uint32_t planned_concurrency = 1;
        vector<stage_profile> stages;
        double accumulated_ms = 0;
        double wall_ms = 0;
    };
    struct build_result {
        validated_asset asset;
        build_report report;
    };
    result<build_result> build_serial(source_geometry, build_options const&, build_limits const&);
    result<build_result> build(source_geometry, build_options const&, execution_options const&);
} // namespace deren::asset::subtilis
