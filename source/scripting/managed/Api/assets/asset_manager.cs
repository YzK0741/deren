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
                return [];
            }
            var ids = new List<string>();
            for (uint index = 0;; ++index) {
                utf8_view view;
                var status = scene_api.list_scene_asset(index, &view);
                if (status == script_status.asset_not_found) {
                    return [.. ids];
                }
                if (status != script_status.success || view.data == null) {
                    return [];
                }
                ids.Add(Encoding.UTF8.GetString(view.data, checked((int)view.length)));
            }
        }

        /// <summary>
        /// Loads one asset by library id. Returns false when the asset is missing, the library is
        /// not configured, or the source file cannot be read. Use the overload with an
        /// <c>out script_status</c> to tell those apart.
        /// </summary>
        public static bool try_load(string id, out asset_handle asset) {
            return try_load(id, out asset, out _);
        }

        /// <summary>
        /// Same call, reporting why it failed: <see cref="script_status.invalid_argument"/> for an
        /// empty id, <see cref="script_status.asset_not_found"/> when the library has no such asset
        /// or produced no handle, and otherwise whatever the host answered.
        /// </summary>
        public static unsafe bool try_load(string id, out asset_handle asset, out script_status status) {
            asset = default;
            if (string.IsNullOrEmpty(id)) {
                status = script_status.invalid_argument;
                return false;
            }
            using var text = native_string.create(id);
            ulong value = 0;
            status = scene_api.load_scene_asset(text.view, &value);
            if (status != script_status.success) {
                return false;
            }
            if (value == 0) {
                status = script_status.asset_not_found;
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
            return release(asset, out _);
        }

        public static bool release(asset_handle asset, out script_status status) {
            if (!asset.is_valid) {
                status = script_status.invalid_argument;
                return false;
            }
            status = scene_api.release_scene_asset(asset.value);
            return status == script_status.success;
        }

        /// <summary>
        /// The renderer pipelines this asset's manifest supports, in manifest order. Empty when the
        /// asset has no named pipeline (a gaussian splat) or the handle is invalid.
        /// </summary>
        public static unsafe string[] supported_pipelines(asset_handle asset) {
            if (!asset.is_valid) {
                return [];
            }
            uint count = 0;
            if (scene_api.query_asset_pipeline_count(asset.value, &count) != script_status.success) {
                return [];
            }
            var pipelines = new List<string>((int)count);
            for (uint index = 0; index < count; ++index) {
                utf8_view view;
                if (scene_api.query_asset_pipeline(asset.value, index, &view) != script_status.success || view.data == null) {
                    return [];
                }
                pipelines.Add(Encoding.UTF8.GetString(view.data, checked((int)view.length)));
            }
            return [.. pipelines];
        }

        /// <summary>
        /// The pipeline a bind starts on. Returns false when the asset declares none or the handle
        /// is invalid; the name is copied out of native storage.
        /// </summary>
        public static bool try_get_default_pipeline(asset_handle asset, out string? pipeline) {
            return try_get_default_pipeline(asset, out pipeline, out _);
        }

        public static unsafe bool try_get_default_pipeline(asset_handle asset, out string? pipeline,
                                                           out script_status status) {
            pipeline = null;
            if (!asset.is_valid) {
                status = script_status.invalid_argument;
                return false;
            }
            utf8_view view;
            status = scene_api.query_asset_default_pipeline(asset.value, &view);
            if (status != script_status.success) {
                return false;
            }
            if (view.data == null || view.length == 0) {
                status = script_status.asset_not_found;
                return false;
            }
            pipeline = Encoding.UTF8.GetString(view.data, checked((int)view.length));
            return true;
        }
    }
}
