using System.Runtime.InteropServices;

namespace deren.scripting {
    internal static unsafe class scene_api {
        [StructLayout(LayoutKind.Sequential)]
        private struct scene_api_layout {
            public delegate* unmanaged[Cdecl]<node_handle, transform_value*, script_status> get_transform;
            public delegate* unmanaged[Cdecl]<node_handle, transform_value*, script_status> set_transform;
            public delegate* unmanaged[Cdecl]<ulong, utf8_view, transform_value*, node_handle*, script_status> try_spawn;
            public delegate* unmanaged[Cdecl]<ulong, node_handle, script_status> try_despawn;
            public delegate* unmanaged[Cdecl]<ulong*, script_status> create_scene_tree;
            public delegate* unmanaged[Cdecl]<ulong, script_status> activate_scene;
            public delegate* unmanaged[Cdecl]<ulong*, script_status> get_active_scene;
            public delegate* unmanaged[Cdecl]<ulong, script_status> deactivate_scene;
            public delegate* unmanaged[Cdecl]<ulong, script_status> destroy_scene_tree;
            public delegate* unmanaged[Cdecl]<ulong, node_handle, utf8_view, transform_value*, node_handle*, script_status> create_node;
            public delegate* unmanaged[Cdecl]<ulong, node_handle, script_status> destroy_node;
            public delegate* unmanaged[Cdecl]<ulong, node_handle, transform_value*, script_status> get_node_transform;
            public delegate* unmanaged[Cdecl]<ulong, node_handle, transform_value*, script_status> set_node_transform;
            public delegate* unmanaged[Cdecl]<ulong, node_handle, byte, script_status> set_node_visible;
            public delegate* unmanaged[Cdecl]<uint, utf8_view*, script_status> list_asset;
            public delegate* unmanaged[Cdecl]<utf8_view, ulong*, script_status> load_asset;
            public delegate* unmanaged[Cdecl]<ulong, script_status> release_asset;
            public delegate* unmanaged[Cdecl]<ulong, node_handle, ulong, transform_value*, node_handle*, script_status> bind_asset;

            public readonly bool is_valid() {
                return get_transform != null && set_transform != null && try_spawn != null && try_despawn != null &&
                       create_scene_tree != null && activate_scene != null && get_active_scene != null &&
                       deactivate_scene != null && destroy_scene_tree != null && create_node != null && destroy_node != null &&
                       get_node_transform != null && set_node_transform != null && set_node_visible != null &&
                       list_asset != null && load_asset != null && release_asset != null && bind_asset != null;
            }
        }

        private static delegate* unmanaged[Cdecl]<node_handle, transform_value*, script_status> get_transform;
        private static delegate* unmanaged[Cdecl]<node_handle, transform_value*, script_status> set_transform;
        private static delegate* unmanaged[Cdecl]<ulong, utf8_view, transform_value*, node_handle*, script_status> try_spawn;
        private static delegate* unmanaged[Cdecl]<ulong, node_handle, script_status> try_despawn;
        private static delegate* unmanaged[Cdecl]<ulong*, script_status> create_scene_tree;
        private static delegate* unmanaged[Cdecl]<ulong, script_status> activate_scene;
        private static delegate* unmanaged[Cdecl]<ulong*, script_status> get_active_scene;
        private static delegate* unmanaged[Cdecl]<ulong, script_status> deactivate_scene;
        private static delegate* unmanaged[Cdecl]<ulong, script_status> destroy_scene_tree;
        private static delegate* unmanaged[Cdecl]<ulong, node_handle, utf8_view, transform_value*, node_handle*, script_status> create_node;
        private static delegate* unmanaged[Cdecl]<ulong, node_handle, script_status> destroy_node;
        private static delegate* unmanaged[Cdecl]<ulong, node_handle, transform_value*, script_status> get_node_transform;
        private static delegate* unmanaged[Cdecl]<ulong, node_handle, transform_value*, script_status> set_node_transform;
        private static delegate* unmanaged[Cdecl]<ulong, node_handle, byte, script_status> set_node_visible;
        private static delegate* unmanaged[Cdecl]<uint, utf8_view*, script_status> list_asset;
        private static delegate* unmanaged[Cdecl]<utf8_view, ulong*, script_status> load_asset;
        private static delegate* unmanaged[Cdecl]<ulong, script_status> release_asset;
        private static delegate* unmanaged[Cdecl]<ulong, node_handle, ulong, transform_value*, node_handle*, script_status> bind_asset;

        internal static bool is_ready() {
            return get_transform != null && set_transform != null && try_spawn != null && try_despawn != null &&
                   create_scene_tree != null && activate_scene != null && get_active_scene != null &&
                   deactivate_scene != null && destroy_scene_tree != null && create_node != null && destroy_node != null &&
                   get_node_transform != null && set_node_transform != null && set_node_visible != null &&
                   list_asset != null && load_asset != null && release_asset != null && bind_asset != null;
        }

        internal static script_status initialize(native_api* api) {
            get_transform = null;
            set_transform = null;
            try_spawn = null;
            try_despawn = null;
            create_scene_tree = null;
            activate_scene = null;
            get_active_scene = null;
            deactivate_scene = null;
            destroy_scene_tree = null;
            create_node = null;
            destroy_node = null;
            get_node_transform = null;
            set_node_transform = null;
            set_node_visible = null;
            list_asset = null;
            load_asset = null;
            release_asset = null;
            bind_asset = null;

            void* table = null;
            var status = api->get_module((uint)native_module.scene, &table);
            if (status != script_status.success) {
                return status;
            }
            if (table == null) {
                return script_status.module_invalid;
            }
            var resolved = *(scene_api_layout*)table;
            if (!resolved.is_valid()) {
                return script_status.module_invalid;
            }
            get_transform = resolved.get_transform;
            set_transform = resolved.set_transform;
            try_spawn = resolved.try_spawn;
            try_despawn = resolved.try_despawn;
            create_scene_tree = resolved.create_scene_tree;
            activate_scene = resolved.activate_scene;
            get_active_scene = resolved.get_active_scene;
            deactivate_scene = resolved.deactivate_scene;
            destroy_scene_tree = resolved.destroy_scene_tree;
            create_node = resolved.create_node;
            destroy_node = resolved.destroy_node;
            get_node_transform = resolved.get_node_transform;
            set_node_transform = resolved.set_node_transform;
            set_node_visible = resolved.set_node_visible;
            list_asset = resolved.list_asset;
            load_asset = resolved.load_asset;
            release_asset = resolved.release_asset;
            bind_asset = resolved.bind_asset;
            return script_status.success;
        }

        internal static script_status query_transform(node_handle node, transform_value* value) {
            return is_ready() ? get_transform(node, value) : script_status.module_unavailable;
        }

        internal static script_status assign_transform(node_handle node, transform_value* value) {
            return is_ready() ? set_transform(node, value) : script_status.module_unavailable;
        }

        internal static script_status spawn(ulong scene, utf8_view asset, transform_value* value, node_handle* node) {
            return is_ready() ? try_spawn(scene, asset, value, node) : script_status.module_unavailable;
        }

        internal static script_status despawn(ulong scene, node_handle node) {
            return is_ready() ? try_despawn(scene, node) : script_status.module_unavailable;
        }

        internal static script_status create_tree(ulong* tree) {
            return is_ready() ? create_scene_tree(tree) : script_status.module_unavailable;
        }

        internal static script_status activate(ulong tree) {
            return is_ready() ? activate_scene(tree) : script_status.module_unavailable;
        }

        internal static script_status active_tree(ulong* tree) {
            return is_ready() ? get_active_scene(tree) : script_status.module_unavailable;
        }

        internal static script_status deactivate(ulong tree) {
            return is_ready() ? deactivate_scene(tree) : script_status.module_unavailable;
        }

        internal static script_status destroy_tree(ulong tree) {
            return is_ready() ? destroy_scene_tree(tree) : script_status.module_unavailable;
        }

        internal static script_status create_tree_node(ulong tree, node_handle parent, utf8_view name,
                                                       transform_value* local, node_handle* node) {
            return is_ready() ? create_node(tree, parent, name, local, node) : script_status.module_unavailable;
        }

        internal static script_status destroy_tree_node(ulong tree, node_handle node) {
            return is_ready() ? destroy_node(tree, node) : script_status.module_unavailable;
        }

        internal static script_status query_tree_transform(ulong tree, node_handle node, transform_value* value) {
            return is_ready() ? get_node_transform(tree, node, value) : script_status.module_unavailable;
        }

        internal static script_status assign_tree_transform(ulong tree, node_handle node, transform_value* value) {
            return is_ready() ? set_node_transform(tree, node, value) : script_status.module_unavailable;
        }

        internal static script_status assign_tree_visible(ulong tree, node_handle node, bool visible) {
            return is_ready() ? set_node_visible(tree, node, visible ? (byte)1 : (byte)0) : script_status.module_unavailable;
        }

        internal static script_status list_scene_asset(uint index, utf8_view* id) {
            return is_ready() ? list_asset(index, id) : script_status.module_unavailable;
        }

        internal static script_status load_scene_asset(utf8_view id, ulong* asset) {
            return is_ready() ? load_asset(id, asset) : script_status.module_unavailable;
        }

        internal static script_status release_scene_asset(ulong asset) {
            return is_ready() ? release_asset(asset) : script_status.module_unavailable;
        }

        internal static script_status bind_tree_asset(ulong tree, node_handle parent, ulong asset,
                                                       transform_value* offset, node_handle* root) {
            return is_ready() ? bind_asset(tree, parent, asset, offset, root) : script_status.module_unavailable;
        }
    }
}
