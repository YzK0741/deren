export module deren.asset.subtilis.dag;
export import deren.asset.subtilis.codec;

export namespace deren::asset::subtilis {
    class validated_asset;
    result<validated_asset> make_asset(source_geometry, payload, limits const&);
    result<void> validate_structure(payload const&, limits const&);
    class validated_asset {
        source_geometry geometry_;
        payload data_;
        validated_asset(source_geometry geometry, payload data)
            : geometry_(std::move(geometry))
            , data_(std::move(data)) {
        }
        friend result<validated_asset> make_asset(source_geometry, payload, limits const&);

    public:
        validated_asset(validated_asset&&) noexcept = default;
        validated_asset& operator=(validated_asset&&) noexcept = default;
        validated_asset(validated_asset const&) = delete;
        validated_asset& operator=(validated_asset const&) = delete;
        source_geometry const& geometry() const noexcept {
            return geometry_;
        }
        payload const& data() const noexcept {
            return data_;
        }
    };
    enum class cut_mode { model_error,
                          screen_error };
    struct cut_parameters {
        cut_mode mode = cut_mode::model_error;
        float threshold = 0;
        array<float, 16> model{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        array<float, 16> view{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        array<float, 16> projection{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        array<uint32_t, 2> extent{0, 0};
        float near_plane = .1f;
        uint32_t max_clusters = none;
    };
    result<vector<uint32_t>> select_cut(validated_asset const&, cut_parameters const&);
} // namespace deren::asset::subtilis
