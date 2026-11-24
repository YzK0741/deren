using System.Runtime.InteropServices;

namespace deren.scripting {
    /// <summary>
    /// ABI 45: the host's collision world table.
    /// </summary>
    /// <remarks>
    /// Managed code owns the policy - which boxes exist, how the capsule is tuned, substepping,
    /// step-up and floor snapping - and the host owns only the box set plus the kernel calls. A world
    /// belongs to the active scene: after the scene changes the managed side rebuilds it.
    /// </remarks>
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct collision_api_layout {
        internal delegate* unmanaged<script_status> world_reset;
        internal delegate* unmanaged<collision_box_desc*, script_status> add_box;
        internal delegate* unmanaged<uint, script_status> remove_box;
        internal delegate* unmanaged<collision_capsule_desc*, float*, collision_hit_desc*, script_status> sweep_capsule;
        internal delegate* unmanaged<collision_capsule_desc*, float*, float, collision_move_desc*, script_status> move_capsule;
        internal delegate* unmanaged<collision_capsule_desc*, float, float, collision_floor_desc*, script_status> find_floor;

        internal readonly bool is_valid() => world_reset != null && add_box != null && remove_box != null &&
            sweep_capsule != null && move_capsule != null && find_floor != null;
    }

    internal static unsafe class collision_api {
        internal static collision_api_layout table;

        internal static bool is_ready() {
            if (!host_call_guard.host_calls_allowed) {
                host_call_guard.note_refusal();
                return false;
            }
            return table.is_valid();
        }

        internal static void shutdown() {
            table = default;
        }

        internal static script_status initialize(native_api* api) {
            shutdown();
            if (api == null || api->get_module == null) {
                return script_status.module_unavailable;
            }
            void* value = null;
            uint size = 0;
            var status = api->get_module((uint)native_module.collision, &value, &size);
            if (status != script_status.success) {
                return status;
            }
            if (value == null || size != (uint)sizeof(collision_api_layout)) {
                return script_status.module_invalid;
            }
            var resolved = *(collision_api_layout*)value;
            if (!resolved.is_valid()) {
                return script_status.module_invalid;
            }
            table = resolved;
            return script_status.success;
        }
    }
}
