using System;
using System.Text;
using deren.scripting;

namespace deren {
    /// <summary>
    /// The global image-based lighting every rendered scene shares.
    /// </summary>
    public static unsafe class global_environment {
        /// <summary>
        /// Replaces the global IBL with the environment file at <paramref name="path"/>: a baked
        /// .ibl or an equirectangular .hdr, resolved against the executable directory when
        /// relative. An empty path returns to the procedural sky.
        ///
        /// The host applies the change between frames, so a successful call only means the path
        /// was accepted; if the later decode fails the current environment stays. Returns false
        /// when the host exposes no environment entry, the scene is gone, or the file is missing.
        /// </summary>
        public static bool try_set_hdr(string? path) {
            path ??= string.Empty;
            try {
                var bytes = Encoding.UTF8.GetBytes(path);
                fixed (byte* data = bytes) {
                    var view = new utf8_view { data = data, length = (uint)bytes.Length };
                    var status = scene_api.set_global_environment(view);
                    if (status == script_status.success) {
                        return true;
                    }
                    managed_bootstrap.core.write_log(Encoding.UTF8.GetBytes(
                        $"Game: cannot set the global environment '{path}' ({status})"));
                    return false;
                }
            } catch (Exception error) {
                managed_bootstrap.core.write_log(Encoding.UTF8.GetBytes(
                    $"Game: global environment request failed: {error.GetBaseException().Message}"));
                return false;
            }
        }
    }
}
