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
    // Optional structural sub-stage split of one reduce_group call, in milliseconds. The caller
    // accumulates it on the scheduling thread; parallel tasks never write shared totals.
    struct reduce_timing {
        double boundary_ms = 0;
        double simplify_ms = 0;
        double recluster_ms = 0;
    };
    primitive_scale measure_scale(source_geometry const&);
    topology analyze_topology(source_geometry const&);
    boundary_signature boundary_of(source_geometry const&, std::span<uint32_t const>);
    boundary_signature boundary_of(source_geometry const&, std::span<uint32_t const>, topology const&);
    bool preserves_region(source_geometry const&, topology const&, boundary_signature const&, std::span<uint32_t const>);
    result<frontier> cluster_lod0(source_geometry const&, build_options const&);
    result<frontier> cluster_indices(source_geometry const&, std::span<uint32_t const>, build_options const&, primitive_scale const&);
    result<vector<vector<uint32_t>>> partition_clusters(source_geometry const&, std::span<cluster_input const>, build_options const&, primitive_scale const&);
    result<reduction_candidate> reduce_group(source_geometry const&, std::span<cluster_input const>, build_options const&, primitive_scale const&, topology const* cached_topology = nullptr, reduce_timing* timing = nullptr);
    void initialize_meshoptimizer();
    result<sphere> geometry_sphere(source_geometry const&, std::span<uint32_t const>);

    // Private bounded scheduler used by the builder (per reduction round) and by the converter
    // (per coarse primitive task). Determinism contract:
    //  - every task's `prepare` may run concurrently and complete in any order, exactly once,
    //    unless a prepare error cancels the remaining (not yet started) tasks;
    //  - `prepare(index)` writes only the caller's per-index slot; `commit` runs after all
    //    prepares on the calling thread, in ascending index order, so completion order, thread
    //    count and timing never decide visible IDs;
    //  - a prepare error runs no commit and reports one of the failed tasks' typed errors;
    //  - concurrency is min(threads, count, max_known_work_bytes / max expression estimate); a
    //    single maximum task that does not fit is a typed capacity error;
    //  - worker threads are joined before return (no detached tasks, no cross-build state).
    using task_fn = std::function<result<void>(std::size_t)>;
    using commit_fn = std::function<result<void>(std::size_t)>;
    result<void> run_ordered(std::size_t count, uint32_t threads, std::span<std::size_t const> estimates, std::size_t max_known_work_bytes, task_fn prepare, commit_fn commit);
} // namespace deren::asset::subtilis::builder_detail
