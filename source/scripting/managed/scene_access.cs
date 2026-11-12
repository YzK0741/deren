using System.Text;
using deren.scripting;

namespace deren {
    public readonly struct entity_ref {
        internal readonly entity_handle handle;

        internal entity_ref(entity_handle handle) {
            this.handle = handle;
        }
    }

    public readonly struct scene_access {
        private readonly behaviour? owner;

        internal scene_access(behaviour owner) {
            this.owner = owner;
        }

        public unsafe bool try_spawn(string asset_id, transform_value pose, out entity_ref entity) {
            entity = default;
            if (owner == null || string.IsNullOrEmpty(asset_id)) {
                return false;
            }
            var api = owner.bound_api();
            var asset = Encoding.UTF8.GetBytes(asset_id);
            fixed (byte* data = asset) {
                entity_handle handle;
                if (api.try_spawn(api.context, new utf8_view { data = data, length = (uint)asset.Length }, &pose, &handle)
                    != script_status.success) {
                    return false;
                }
                entity = new entity_ref(handle);
                return true;
            }
        }

        public unsafe bool try_despawn(entity_ref entity) {
            if (owner == null) {
                return false;
            }
            var api = owner.bound_api();
            return api.try_despawn(api.context, entity.handle) == script_status.success;
        }
    }
}
