using System;

namespace deren.editor {
    internal sealed unsafe partial class editor_host {
        private readonly viewport_box_outline box_outline = new();

        private void toggle_volume(bool solid) {
            if (document?.find(selection_state?.primary ?? Guid.Empty) is not { } node) { return; }
            bool before = solid ? node.collision_box : node.box_trigger;
            bool assign(bool value) {
                if (solid) { node.collision_box = value; }
                else { node.box_trigger = value; }
                mark_dirty_and_refresh();
                return true;
            }
            assign(!before);
            record_undo(solid ? "Set Solid Box" : "Set Trigger Volume", () => assign(before), () => assign(!before));
            update_history_buttons();
        }

        private void toggle_trigger_target() {
            if (document?.find(selection_state?.primary ?? Guid.Empty) is not { } node) { return; }
            bool old = node.trigger_target;
            bool assign(bool value) {
                node.trigger_target = value;
                mark_dirty_and_refresh();
                return true;
            }
            assign(!old);
            record_undo("Set Trigger Target", () => assign(old), () => assign(!old));
            update_history_buttons();
        }
    }
}
