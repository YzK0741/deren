using System;
using System.Collections.Generic;
using System.Text;
using deren.scripting;

namespace deren {
    /// <summary>
    /// Loads assets from the configured asset library. A handle owns one shared source model;
    /// bind it to any number of managed trees before releasing it.
    /// </summary>
    public static class asset_manager {
        /// <summary>
        /// Returns the loadable asset ids in sorted order. The native ids are borrowed for the
        /// duration of the call, so each one is copied into managed memory.
        /// </summary>
        public static unsafe string[] list() {
            if (!scene_api.is_ready()) {
                return Array.Empty<string>();
            }
            var ids = new List<string>();
            for (uint index = 0;; ++index) {
                utf8_view view;
                var status = scene_api.list_scene_asset(index, &view);
                if (status == script_status.asset_not_found) {
                    return ids.ToArray();
                }
                if (status != script_status.success || view.data == null) {
                    return Array.Empty<string>();
                }
                ids.Add(Encoding.UTF8.GetString(view.data, checked((int)view.length)));
            }
        }

        /// <summary>
        /// Loads one asset by library id. Returns false when the asset is missing, the library is
        /// not configured, or the source file cannot be read.
        /// </summary>
        public static unsafe bool try_load(string id, out asset_handle asset) {
            asset = default;
            if (string.IsNullOrEmpty(id)) {
                return false;
            }
            var id_bytes = Encoding.UTF8.GetBytes(id);
            fixed (byte* id_data = id_bytes) {
                var view = new utf8_view {
                    data = id_data,
                    length = (uint)id_bytes.Length
                };
                ulong value = 0;
                if (scene_api.load_scene_asset(view, &value) != script_status.success || value == 0) {
                    return false;
                }
                asset = new asset_handle(value);
                return true;
            }
        }

        /// <summary>
        /// Releases one load reference. Instances already bound to a tree keep their copied node
        /// data and remain valid for the tree's lifetime.
        /// </summary>
        public static bool release(asset_handle asset) {
            return asset.is_valid && scene_api.release_scene_asset(asset.value) == script_status.success;
        }
    }
}
