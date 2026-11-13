using System.Text;
using deren.scripting;

namespace deren {
    public readonly struct scene_access {
        private readonly script_base? owner;

        internal scene_access(script_base owner) {
            this.owner = owner;
        }

        /// <summary>The scene node this script_base is attached to.</summary>
        public node_id self => owner is null ? default : new node_id(owner.self_handle, owner);

        public unsafe bool try_spawn(string asset_id, transform_value pose, out node_id target) {
            target = default;
            if (owner == null || string.IsNullOrEmpty(asset_id)) {
                return false;
            }
            owner.ensure_bound();
            var asset = Encoding.UTF8.GetBytes(asset_id);
            fixed (byte* data = asset) {
                node_handle handle;
                if (scene_api.spawn(owner.scene_id, new utf8_view { data = data, length = (uint)asset.Length }, &pose, &handle)
                    != script_status.success) {
                    return false;
                }
                target = new node_id(handle, owner);
                return true;
            }
        }

        public unsafe bool try_despawn(node_id target) {
            if (owner == null || !target.is_valid) {
                return false;
            }
            owner.ensure_bound();
            return scene_api.despawn(owner.scene_id, target.handle) == script_status.success;
        }
    }
}
