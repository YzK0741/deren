using System.Runtime.InteropServices;

namespace deren.scripting {
    /// <summary>
    /// ABI 33: the structs both sides pass by value or by pointer. Keep this list in step with
    /// <c>scripting::abi_type</c> in managed_host.cppm; never renumber a value once shipped.
    /// </summary>
    internal enum abi_type : uint {
        native_api = 1,
        managed_api = 2,
        frame_info = 3,
        transform_value = 4,
        input_event = 5,
        point_light_desc = 6,
        material_override_desc = 7,
        startup_options = 8,
        head_basis_desc = 9,
        face_frame_desc = 10,
        animation_state = 11,
        diagnostic_counters = 12,
        toon_rig_desc = 13,
        /// <summary>ABI 42: the rml_ui table's pointer-event payload.</summary>
        rml_ui_event = 14,
        /// <summary>ABI 45: the collision module's payloads (see <c>collision_api</c>).</summary>
        collision_box_desc = 15,
        collision_capsule_desc = 16,
        collision_hit_desc = 17,
        collision_move_desc = 18,
        collision_floor_desc = 19,
    }

    /// <summary>One <c>{type, sizeof}</c> answer from the host's abi_layout entry point.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct abi_layout_entry {
        public abi_type type;
        public uint size;
    }
}
