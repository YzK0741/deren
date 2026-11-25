using System;
using System.Globalization;

namespace deren.editor {
    internal sealed unsafe partial class editor_host {
        /// <summary>
        /// Applies the Initial Clip dropdown: -1 means "the asset's default" and any other value
        /// indexes the placement's own clip list. Authored only, never imported: the editor tree keeps
        /// its players paused, so the choice is what Play and standalone apply on entry.
        /// </summary>
        private void apply_animation_clip(string text) {
            if (document?.find(selection_state?.primary ?? Guid.Empty) is not { } node) { return; }
            if (!int.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var requested)) {
                notice = "Choose an animation clip from the list";
                return;
            }
            string[] clips = node.runtime.is_valid ? animation_player.get_clip_names(node.runtime) : [];
            if (requested < -1 || requested >= clips.Length) {
                notice = clips.Length == 0
                    ? "This placement has no animation clips"
                    : $"Clip index must be between 1 and {clips.Length.ToString(CultureInfo.InvariantCulture)}";
                refresh_panels(force_details: true);
                return;
            }
            int previous = node.animation_clip;
            if (previous == requested) { return; }
            bool assign(int value) {
                node.animation_clip = value;
                mark_dirty_and_refresh();
                return true;
            }
            assign(requested);
            record_undo("Set Initial Animation Clip", () => assign(previous), () => assign(requested));
            update_history_buttons();
        }

        /// <summary>
        /// Toggles whether Play starts this placement's clip immediately. Off leaves it paused, which
        /// is what the editor tree shows, so a two-character scene can hold one character still.
        /// </summary>
        private void toggle_animation_autoplay() {
            if (document?.find(selection_state?.primary ?? Guid.Empty) is not { } node) { return; }
            bool previous = node.animation_autoplay;
            bool assign(bool value) {
                node.animation_autoplay = value;
                mark_dirty_and_refresh();
                return true;
            }
            assign(!previous);
            record_undo("Set Animation Auto Play", () => assign(previous), () => assign(!previous));
            update_history_buttons();
        }
    }
}
