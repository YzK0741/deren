using deren.scripting;
using System.Runtime.InteropServices;

namespace deren {
    // `--node-name-selftest`: exercises the ABI 40 rename boundary on the first frame and then keeps
    // succeeding. It needs no asset: an empty activated tree plus one node is enough, so the same
    // checks run against the default demo.
    internal static unsafe class node_name_selftest {
        internal static bool requested;
        private static bool checked_once;
        private static scene_tree? tree;

        internal static script_status initialize(managed_api* scripts) {
            // The host refuses to run a project that never activated a scene tree.
            if (!scene_tree.try_create(out var created) || created == null) {
                return script_status.script_failure;
            }
            tree = created;
            if (!tree.try_activate()) {
                return script_status.script_failure;
            }
            scripts->update = &update;
            scripts->stop = &stop;
            return script_status.success;
        }

        [UnmanagedCallersOnly]
        private static script_status update(frame_info* frame) {
            if (checked_once) {
                return script_status.success;
            }
            checked_once = true;
            var local = new transform_value { scale = new vector_3 { x = 1.0f, y = 1.0f, z = 1.0f } };
            if (!tree!.try_create_node(default, "before", local, out var node) || !node.is_valid) {
                return script_status.script_failure;
            }
            // A rename is accepted, the next read answers the new name, and the node keeps its handle.
            if (!tree.try_set_name(node, "renamed-node")) {
                return script_status.script_failure;
            }
            if (!tree.try_get_name(node, out var name) || name != "renamed-node") {
                return script_status.script_failure;
            }
            // An empty name is refused by the wrapper before it reaches the host.
            if (tree.try_set_name(node, string.Empty, out var empty_status) ||
                empty_status != script_status.invalid_argument) {
                return script_status.script_failure;
            }
            // A name over the 256-byte cap is refused by the host.
            if (tree.try_set_name(node, new string('x', 300), out var long_status) ||
                long_status != script_status.invalid_argument) {
                return script_status.script_failure;
            }
            // A well-formed handle no node carries is invalid_node, not invalid_argument.
            var stale = new node_id(new node_handle { value = 0xDEAD });
            if (tree.try_set_name(stale, "ignored", out var stale_status) ||
                stale_status != script_status.invalid_node) {
                return script_status.script_failure;
            }
            // The refusals left the stored name alone.
            return tree.try_get_name(node, out var kept) && kept == "renamed-node"
                ? script_status.success
                : script_status.script_failure;
        }

        [UnmanagedCallersOnly]
        private static script_status stop() {
            return script_status.success;
        }
    }
}
