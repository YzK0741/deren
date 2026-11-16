
namespace deren.scripting {
    /// <summary>
    /// ABI 33: checks the host's struct sizes against this side's mirrors before anything is bound.
    /// The version check only catches an ABI both sides know they disagree about; these sizes also
    /// catch a field added, removed, or reordered without a version bump, which used to surface
    /// later as a call through a wrong signature instead of a startup failure.
    /// </summary>
    internal static unsafe class abi_manifest {
        private const int entry_count = 11;

        /// <summary>
        /// Asks the host for every mirrored struct size and compares it with <c>sizeof</c> of the
        /// managed mirror. Returns false after logging the first disagreement, so the caller can
        /// refuse to bind rather than run on a layout it does not understand.
        /// </summary>
        internal static bool verify(native_api* api) {
            if (api == null || api->abi_layout == null) {
                logger.write(api, "Managed host: the native host publishes no ABI layout (ABI 33 required).");
                return false;
            }
            abi_layout_entry* entries = stackalloc abi_layout_entry[entry_count];
            var status = api->abi_layout(entries, entry_count);
            if (status != script_status.success) {
                logger.write(api, $"Managed host: the native host rejected the ABI layout request ({status}).");
                return false;
            }
            for (int index = 0; index != entry_count; ++index) {
                var entry = entries[index];
                var expected = expected_size(entry.type);
                if (entry.size != expected) {
                    logger.write(api, $"Managed host: the layouts of '{entry.type}' disagree " +
                             $"({entry.size} bytes native, {expected} bytes managed).");
                    return false;
                }
            }
            return true;
        }

        private static uint expected_size(abi_type type) {
            return type switch {
                abi_type.native_api => (uint)sizeof(native_api),
                abi_type.managed_api => (uint)sizeof(managed_api),
                abi_type.frame_info => (uint)sizeof(frame_info),
                abi_type.transform_value => (uint)sizeof(transform_value),
                abi_type.input_event => (uint)sizeof(native_input_event),
                abi_type.point_light_desc => (uint)sizeof(point_light_desc),
                abi_type.material_override_desc => (uint)sizeof(material_override_desc),
                abi_type.startup_options => (uint)sizeof(startup_options),
                abi_type.head_basis_desc => (uint)sizeof(scene_api.head_basis_desc),
                abi_type.face_frame_desc => (uint)sizeof(scene_api.face_frame_desc),
                abi_type.animation_state => (uint)sizeof(animation_state),
                // An entry this build does not know: the caller reports the mismatch by name.
                _ => 0u,
            };
        }
    }
}
