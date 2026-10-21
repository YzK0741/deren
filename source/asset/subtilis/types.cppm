export module deren.asset.subtilis.types;
export import deren.utility.shared;

export namespace deren::asset::subtilis {
    using std::byte;
    using std::uint32_t;
    using std::uint64_t;
    using utility::vessel::array;
    using utility::vessel::string;
    using utility::vessel::vector;
    using digest = array<byte, 32>;
    inline constexpr uint32_t none = 0xffffffffu;
    enum class error_code { invalid_input,
                            invalid_payload,
                            unsupported_version,
                            capacity,
                            invalid_geometry,
                            invalid_options,
                            invalid_dag,
                            io };
    struct error {
        error_code code;
        string context;
        std::optional<uint32_t> primitive;
        std::optional<uint32_t> group;
        bool operator==(error const&) const = default;
    };
    template <typename T>
    using result = std::expected<T, error>;
    struct sphere {
        float x = 0, y = 0, z = 0, radius = 0;
        bool operator==(sphere const&) const = default;
    };
    struct cluster_record {
        uint32_t first_index = 0, index_count = 0, producer = none, consumer = none;
        sphere geometry;
        bool operator==(cluster_record const&) const = default;
    };
    struct group_record {
        uint32_t fine_first = 0, fine_count = 0, coarse_first = 0, coarse_count = 0;
        sphere lod;
        float error = 0;
        bool operator==(group_record const&) const = default;
    };
    struct source_geometry {
        vector<array<float, 3>> positions, normals;
        vector<array<float, 2>> uv0;
        vector<uint32_t> indices;
    };
    struct payload {
        vector<uint32_t> indices;
        vector<cluster_record> clusters;
        vector<group_record> groups;
        vector<uint32_t> references, leaves, roots;
        digest source_hash{}, build_hash{};
        string build_meta;
    };
    struct limits {
        uint64_t max_input_bytes = 4ULL << 30;
        uint64_t max_payload_bytes = 1ULL << 30;
        uint64_t max_records = 16ULL << 20;
    };
    struct build_limits {
        uint64_t max_output_bytes = 1ULL << 30;
        uint64_t max_known_work_bytes = 2ULL << 30;
    };
    struct build_options {
        uint32_t triangle_limit = 85;
        uint32_t vertex_limit = 256;
        uint32_t target_group_size = 8;
        uint32_t max_rounds = 16;
        float target_ratio = .5f;
        float relative_error = .01f;
        float normal_weight = .1f;
        float uv_weight = .01f;
    };
    struct execution_options {
        uint32_t threads = 1;
        build_limits limits;
    };
} // namespace deren::asset::subtilis
