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
            using var text = native_string.create(id);
            ulong value = 0;
            if (scene_api.load_scene_asset(text.view, &value) != script_status.success || value == 0) {
                return false;
            }
            asset = new asset_handle(value);
            return true;
        }

        /// <summary>
        /// Releases one load reference. Instances already bound to a tree keep their copied node
        /// data and remain valid for the tree's lifetime.
        /// </summary>
        public static bool release(asset_handle asset) {
            return asset.is_valid && scene_api.release_scene_asset(asset.value) == script_status.success;
        }

        /// <summary>
        /// The renderer pipelines this asset's manifest supports, in manifest order. Empty when the
        /// asset has no named pipeline (a gaussian splat) or the handle is invalid.
        /// </summary>
        public static unsafe string[] supported_pipelines(asset_handle asset) {
            if (!asset.is_valid) {
                return Array.Empty<string>();
            }
            uint count = 0;
            if (scene_api.query_asset_pipeline_count(asset.value, &count) != script_status.success) {
                return Array.Empty<string>();
            }
            var pipelines = new List<string>((int)count);
            for (uint index = 0; index < count; ++index) {
                utf8_view view;
                if (scene_api.query_asset_pipeline(asset.value, index, &view) != script_status.success || view.data == null) {
                    return Array.Empty<string>();
                }
                pipelines.Add(Encoding.UTF8.GetString(view.data, checked((int)view.length)));
            }
            return pipelines.ToArray();
        }

        /// <summary>
        /// The pipeline a bind starts on. Returns false when the asset declares none or the handle
        /// is invalid; the name is copied out of native storage.
        /// </summary>
        public static unsafe bool try_get_default_pipeline(asset_handle asset, out string? pipeline) {
            pipeline = null;
            if (!asset.is_valid) {
                return false;
            }
            utf8_view view;
            if (scene_api.query_asset_default_pipeline(asset.value, &view) != script_status.success ||
                view.data == null || view.length == 0) {
                return false;
            }
            pipeline = Encoding.UTF8.GetString(view.data, checked((int)view.length));
            return true;
        }
    }
}
