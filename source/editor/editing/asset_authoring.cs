using deren.scripting;

namespace deren.editor {
    internal readonly unsafe struct asset_authoring {
        private readonly authoring_table* native;

        #pragma warning disable CS0649 // The native table owns these fields.
        private struct authoring_table {
            internal delegate* unmanaged<utf8_view*, script_status> root;
            internal delegate* unmanaged<utf8_view*, script_status> choose;
            internal delegate* unmanaged<script_status> refresh;
        }

        private struct authoring_table_extended {
            internal delegate* unmanaged<utf8_view*, script_status> root;
            internal delegate* unmanaged<utf8_view*, script_status> choose;
            internal delegate* unmanaged<script_status> refresh;
            internal delegate* unmanaged<utf8_view*, script_status> choose_environment;
            internal delegate* unmanaged<utf8_view, utf8_view*, script_status> directory;
            internal delegate* unmanaged<utf8_view, uint, utf8_view*, script_status> metadata;
        }

        #pragma warning restore CS0649
        private readonly delegate* unmanaged<utf8_view*, script_status> choose_environment;

        private readonly delegate* unmanaged<utf8_view, utf8_view*, script_status> directory;
        private readonly delegate* unmanaged<utf8_view, uint, utf8_view*, script_status> metadata;

        private asset_authoring(authoring_table* address, delegate* unmanaged<utf8_view*, script_status> environment_picker,
                                delegate* unmanaged<utf8_view, utf8_view*, script_status> directory_query,
                                delegate* unmanaged<utf8_view, uint, utf8_view*, script_status> metadata_query) {
            native = address;
            choose_environment = environment_picker;
            directory = directory_query;
            metadata = metadata_query;
        }

        /// <summary>
        /// Resolves the catalogue table, preferring the extended module (id 8) so the environment picker is
        /// available, and falling back to module 7 for a host that predates it. The two tables share their
        /// first three entries, which is what makes the fallback a plain read of the shorter layout.
        /// </summary>
        internal static bool try_open(out asset_authoring authoring) {
            authoring = default;
            var core = managed_bootstrap.core;
            if (core.get_module == null) { return false; }
            void* address;
            uint size;
            var status = core.get_module((uint)native_module.asset_authoring_extended, &address, &size);
            if (status == script_status.success && address != null && size >= 4 * sizeof(nint)) {
                var extended = (authoring_table_extended*)address;
                if (extended->root != null && extended->choose != null && extended->refresh != null) {
                    authoring = new asset_authoring((authoring_table*)address, extended->choose_environment,
                                                    size >= 5 * sizeof(nint) ? extended->directory : null,
                                                    size >= sizeof(authoring_table_extended) ? extended->metadata : null);
                    return true;
                }
            }
            if (core.get_module((uint)native_module.asset_authoring, &address, &size) != script_status.success ||
                address == null || size < sizeof(authoring_table)) {
                return false;
            }
            var resolved = (authoring_table*)address;
            if (resolved->root == null || resolved->choose == null || resolved->refresh == null) {
                return false;
            }
            authoring = new asset_authoring(resolved, null, null, null);
            return true;
        }

        /// <summary>Whether the host carries the environment-map picker (module 8).</summary>
        internal bool has_environment_picker => choose_environment != null;

        internal string? library_root() => read_path(native->root);
        internal string? pick_model() => read_path(native->choose);
        internal string? pick_environment() => choose_environment != null ? read_path(choose_environment) : null;
        internal script_status refresh() => native->refresh();

        internal string? asset_directory(string id) {
            if (directory == null) { return null; }
            using var identifier = native_string.create(id);
            utf8_view path;
            return directory(identifier.view, &path) == script_status.success && path.data != null ? path.decode() : null;
        }

        internal string? display_name(string id) => query_metadata(id, 0);
        internal string? motion_path(string id) => query_metadata(id, 1);

        private string? query_metadata(string id, uint field) {
            if (metadata == null) { return null; }
            using var identifier = native_string.create(id);
            utf8_view value;
            return metadata(identifier.view, field, &value) == script_status.success && value.data != null
                ? value.decode() : null;
        }

        private static string? read_path(delegate* unmanaged<utf8_view*, script_status> call) {
            utf8_view path;
            return call(&path) == script_status.success && path.data != null ? path.decode() : null;
        }
    }
}
