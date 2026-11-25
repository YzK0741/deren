using System;

namespace deren.editor {
    /// <summary>
    /// The Dope Sheet as one area of the scene screen. The sheet's own builders are reused unchanged, so
    /// the animation editor and this leaf paint the same lanes, the same keys and the same transport; only
    /// the rect and the frame around them differ. The sheet is a leaf like any other: it has an area
    /// header, it can be maximized, and it never owns the window.
    /// </summary>
    internal sealed unsafe partial class editor_host {
        private const string scene_dope_panel_key = "dope_panel";

        /// <summary>
        /// The sheet inside the leaf's rect. The rows take whatever is left above the transport, and the
        /// channel list is told that height so the wheel and the reveal both agree with the painting.
        /// </summary>
        private ui_element build_scene_dope_leaf(area_node node, float width, float height, string path) {
            var clip = timeline_clip;
            float time = 0.0F;
            float duration = 0.0F;
            bool playing = false;
            if (animation_preview_player() is { } player && animation_player.try_get_info(player, out var info) &&
                info.available) {
                time = info.time;
                duration = info.duration;
                playing = info.playing;
            }
            if (duration <= 0.0F) { duration = clip?.duration ?? 0.0F; }
            next_ui_duration = duration;
            float body = MathF.Max(1.0F, height - scene_caption_height);
            float transport = MathF.Min(sheet_transport_height, MathF.Max(0.0F, body - sheet_ruler_height));
            float rows = MathF.Max(sheet_ruler_height + ui_theme.row_height + 4.0F, body - transport);
            // The rows builder insets its content by 16 px (8 px padding on both sides) and puts the ruler
            // above the lanes; the list is told the rest, or the last row would hide under the transport.
            next_ui_list_height = MathF.Max(channel_list.row_height, rows - 16.0F - sheet_ruler_height - 2.0F);
            string clip_name = next_ui_edit_name.Length > 0
                ? next_ui_edit_name
                : clip?.clip_name ?? (next_ui_asset.Length > 0 ? next_ui_asset : "No motion clip");
            var column = ui_node.column(scene_dope_panel_key).size(width, height).bg(ui_theme.window).clipped()
                                .spacing(0.0F);
            column.add(build_scene_caption("dope_caption", string.Empty, node, path, width));
            column.add(build_sheet_rows(clip, rows, time, duration));
            if (transport > 0.0F) {
                column.add(build_sheet_transport(clip, clip_name, time, duration, playing));
            }
            column.add(ui_node.spacer("dope_filler"));
            return column;
        }

        /// <summary>The sheet's rect from the last submitted frame, or an empty rect when it is not showing.</summary>
        private ui_rect scene_dope_area() =>
            next_ui?.last_tree?.find(scene_dope_panel_key)?.rect ?? ui_rect.empty;

        private bool scene_dope_contains(float x, float y) => scene_dope_area().contains(x, y);

        /// <summary>
        /// Routing rule: a press inside the sheet's rect is the sheet's, and it goes to the same pointer
        /// handler the animation editor uses. Keys the sheet does not know fall through to nothing.
        /// </summary>
        private bool press_scene_dope(float x, float y) {
            if (!scene_dope_contains(x, y)) { return false; }
            press_next_ui(x, y);
            return true;
        }

        /// <summary>Whether the sheet is in the middle of a drag, so moves and the release belong to it.</summary>
        private bool scene_dope_gesture => next_ui_gesture != sheet_gesture.none;

        private bool scroll_scene_dope(float wheel, float x, float y) {
            if (!scene_dope_contains(x, y)) { return false; }
            scroll_next_ui(wheel, x, y);
            return true;
        }
    }
}
