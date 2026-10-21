#pragma once
// Internal publication seam shared by the package writers and the converter test.
//
// Production entry points (`write_package`, `write_bytes_staged`, the CLI) always
// pass `nullptr` for `sink_ops`: there is no environment variable, config key or
// runtime switch that can inject a fault. The test target reaches the `*_impl`
// functions directly and supplies a failing byte sink / rename operation to prove
// that short writes, close failures and final publication failures clean up the
// staging directory and leave source and prior-target bytes untouched.
#include "document.hpp"

namespace deren::tools::subtilis::package_detail {
    /// One staged file opened through the publication seam.
    struct byte_sink {
        virtual ~byte_sink() = default;
        /// Write the whole buffer or return an io error. A short write must be reported.
        virtual result<void> write(std::span<std::byte const> bytes) = 0;
        /// Flush and close; a failed close must return an io error.
        virtual result<void> close() = 0;
    };
    /// Test-only fault injection points. `nullptr` members mean the production implementation.
    struct sink_ops {
        result<std::unique_ptr<byte_sink>> (*open)(path const& file, void* context) = nullptr;
        result<void> (*rename)(path const& from, path const& to, void* context) = nullptr;
        void* context = nullptr;
    };
    /// Production: staged single-file (GLB) write. Never injects.
    result<void> write_bytes_staged_impl(path const& target, std::span<std::byte const> bytes, bool verify_glb, bool overwrite, sink_ops const* ops);
    /// Production: staged glTF directory-package write. Never injects.
    result<void> write_gltf_package_impl(document const&, path const& package_dir, bool overwrite, sink_ops const* ops);
    /// `package_dir / "<package_dir leaf>.gltf"`: the entry JSON of a directory package.
    path package_json_path(path const& package_dir);
    /// CLI view of `--format gltf -o out.gltf`: the package directory is `out`.
    path package_dir_for_cli_output(path const& output);
    /// Canonical containment check used to refuse replacing a package that holds the source
    /// (or a report file). Conservative: an uncanonicalizable path is treated as inside.
    bool path_inside(path const& candidate, path const& directory);
    /// The platform's move-a-single-filesystem-entry primitive, used by the injected rename
    /// test to delegate calls that must succeed. `to` must not exist.
    result<void> move_path(path const& from, path const& to);
} // namespace deren::tools::subtilis::package_detail
