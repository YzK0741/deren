// module version: 0.1.0
// Shared wire types, independent of .NET hosting and renderer implementation.
export module deren.scripting.api_types;
export import deren.vstd;
export namespace deren::scripting {
    struct utf8_view {
        char const* data;
        std::uint32_t length;
    };

    enum class script_status : std::int32_t {
        success = 0,
        invalid_argument = 1,
        invalid_node = 2,
        scene_exists = 3,
        scene_not_found = 4,
        script_failure = 5,
        spawn_unavailable = 6,
        module_unavailable = 7,
        module_invalid = 8,
        asset_not_found = 9,
        asset_invalid = 10
    };

} // namespace deren::scripting
