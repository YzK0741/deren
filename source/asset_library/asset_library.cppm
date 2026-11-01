// ============================================================================
// module: asset_library
// module version: 1.0.0
//
// A folder-backed asset catalogue: each asset is one directory with an
// `asset.toml` manifest, and the library root may carry a `library.toml` that
// names the default asset, limits the scan depth and lists folders to ignore.
// Pure CPU, no Vulkan dependency.
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - independently of the rest of the project.
// ============================================================================
module;

#include <cstdint>

export module deren.asset_library;
export import deren.utility.shared;
export import deren.vstd;

/**
 * @file asset_library.cppm
 * @defgroup asset_library Asset Library
 * @brief discover renderable assets from a directory tree and expose their
 *        manifests as plain data. Pure CPU, no Vulkan dependency.
 *
 * Layout:
 * @code
 * assets/
 *   library.toml             # optional: version, default, scan_depth, ignore
 *   damaged-helmet/
 *     asset.toml             # required for an asset
 *     DamagedHelmet.gltf
 *   character/
 *     asset.toml
 *     character.glb
 *     character.glb.toon.tsv
 * @endcode
 *
 * `library.toml`:
 * @code
 * version = 1
 * default = "damaged-helmet"
 * scan_depth = 1       # 1 = direct child folders, 2 = one nested level, ...
 * ignore = ["drafts", "tmp"]
 * @endcode
 *
 * `asset.toml`:
 * @code
 * id = "damaged-helmet"        # required and unique; defaults to the folder name
 * name = "Damaged Helmet"
 * type = "pbr"                  # pbr | toon | unlit | gaussian
 * model = "DamagedHelmet.gltf"  # required, resolved relative to this folder
 * sidecar = "DamagedHelmet.gltf.toon.tsv"
 * license = "LICENSE.md"
 * thumbnail = "thumbnail.png"
 * tags = ["sample", "helmet"]
 *
 * [toon]
 * profile = "default"
 * game_toon = true              # explicit opt-in; `type` alone never enables it
 * tone_mapping = 1
 *
 * [environment]
 * hdr = "studio.hdr"
 *
 * [camera]
 * pose = [35.0, -12.0, 4.2, 0.0, 1.6, 0.0]  # yaw, pitch, distance, target xyz
 * @endcode
 *
 * Every path in a manifest is relative to the asset directory. Absolute paths,
 * `..` components and missing model files make the manifest invalid; an invalid
 * manifest is skipped with a warning rather than silently becoming an asset.
 * The library never guesses that `type = "toon"` means the experimental
 * game-toon renderer: that stage is enabled only by `[toon] game_toon = true`,
 * which mirrors the configuration's separate opt-in.
 */
namespace deren::asset_library {
    /**
     * @ingroup asset_library
     * @brief how an asset is shaded by default; metadata only, the renderer's
     *        own switches still decide which pipeline runs
     */
    export enum class asset_type {
        pbr,
        toon,
        unlit,
        gaussian,
    };

    /// the manifest spelling of @p type, for logs and diagnostics
    export std::string_view asset_type_name(asset_type type) noexcept;

    /// parse a case-insensitive manifest spelling; nullopt for an unknown type
    export std::optional<asset_type> parse_asset_type(std::string_view text) noexcept;

    /**
     * @ingroup asset_library
     * @brief one validated asset manifest. Paths are absolute and lexically
     *        normalized, so a caller never has to combine them with the asset
     *        directory again.
     */
    export struct asset_entry {
        std::string id;
        std::string name;
        asset_type type = asset_type::pbr;
        std::filesystem::path directory;
        std::filesystem::path model;
        std::filesystem::path sidecar;   // empty when the manifest has none
        std::filesystem::path license;   // empty when absent or unreadable
        std::filesystem::path thumbnail; // empty when absent or unreadable
        std::vector<std::string> tags;
        // The game-toon opt-in is deliberately separate from `type`: a toon
        // asset may be shaded by the ordinary toon chain, and only this flag
        // asks for the captured game-toon path.
        bool game_toon = false;
        std::string toon_profile;
        std::optional<int32_t> tone_mapping; // 0..2, same meaning as [render] tone_mapping
        std::filesystem::path environment_hdr; // empty when absent or unreadable
        std::optional<std::array<float, 6>> camera_pose; // yaw, pitch, distance, target xyz
    };

    /**
     * @ingroup asset_library
     * @brief the result of scanning a library root.
     *
     * `valid()` is false when the ROOT itself cannot be read or contains no
     * usable asset; individual bad manifests are reported through `warnings()`
     * and skipped, because one broken asset must not hide the rest.
     */
    export class asset_library {
    public:
        asset_library() = default;

        /// scan @p root, reading its optional library.toml and every asset.toml
        /// within the configured depth
        [[nodiscard]] static asset_library scan(std::filesystem::path root);

        /// true when the root was scanned and at least one asset was loaded
        [[nodiscard]] bool valid() const noexcept { return error_.empty(); }

        /// why the root could not be used; empty when valid()
        [[nodiscard]] std::string const& error() const noexcept { return error_; }

        /// the absolute root that was scanned
        [[nodiscard]] std::filesystem::path const& root() const noexcept { return root_; }

        /// assets sorted by id, so iteration order never depends on the filesystem
        [[nodiscard]] std::span<asset_entry const> entries() const noexcept { return entries_; }

        /// the asset named by library.toml's `default`, or the first asset when
        /// that key is absent; nullptr when no asset was loaded
        [[nodiscard]] asset_entry const* default_asset() const noexcept;

        /// find @p id exactly; nullptr when absent
        [[nodiscard]] asset_entry const* find(std::string_view id) const noexcept;

        /// non-fatal problems found while scanning (bad manifests, duplicate ids,
        /// missing optional files), one line each
        [[nodiscard]] std::vector<std::string> const& warnings() const noexcept { return warnings_; }

    private:
        std::filesystem::path root_;
        std::vector<asset_entry> entries_;
        std::string default_id_;
        std::string error_;
        std::vector<std::string> warnings_;
    };
} // namespace deren::asset_library
