using System;
using System.Globalization;
using System.IO;
using System.Text;

namespace deren.editor;

/// <summary>
/// The animation editor's Timeline: one row per animated channel of the preview asset's motion clip,
/// read straight from the `motion.gltf` that the asset points at. This slice is read-only; clicking a
/// row jumps the preview player to that channel's first key so the channel's effect is visible.
/// </summary>
internal sealed unsafe partial class editor_host {
    private motion_document? timeline_clip;
    private string timeline_asset = string.Empty;
    private int timeline_selected_channel = -1;

    /// <summary>Shows the Timeline tab and loads the preview asset's motion clip into it.</summary>
    private void enter_timeline() {
        timeline_clip = null;
        timeline_asset = string.Empty;
        timeline_selected_channel = -1;
        load_timeline_clip();
        if (ui != null) {
            ui.try_set_element_style("tab_timeline", "display", "block");
            set_dock_tab("tab_timeline");
        }
        refresh_timeline();
    }

    /// <summary>Hides the Timeline tab again and drops the loaded clip.</summary>
    private void leave_timeline() {
        timeline_clip = null;
        timeline_asset = string.Empty;
        timeline_selected_channel = -1;
        if (ui == null) { return; }
        set_dock_tab("tab_content");
        ui.try_set_element_style("tab_timeline", "display", "none");
        ui.set_text("timeline_summary", "No motion clip");
        ui.set_text("timeline_source", string.Empty);
        ui.try_set_markup("timeline_rows", string.Empty);
    }

    /// <summary>
    /// Resolves the preview placement's asset to its motion file and loads the clip. The preview scene
    /// is the animation editor's document, so the asset is simply the preview node's asset.
    /// </summary>
    private void load_timeline_clip() {
        if (animation_preview()?.asset is not { Length: > 0 } asset) { return; }
        timeline_asset = asset;
        if (!asset_authoring.try_open(out var authoring)) {
            notice = "The asset library is not available for the timeline";
            return;
        }
        string? motion_path = authoring.motion_path(asset);
        if (string.IsNullOrEmpty(motion_path)) {
            notice = $"'{asset}' has no motion clip";
            return;
        }
        if (!motion_document.try_load(motion_path, out var clip, out var error)) {
            notice = $"Could not read the motion clip: {error}";
            return;
        }
        timeline_clip = clip;
        if (clip == null) { return; }
        if (clip.channels.Count > 0) { timeline_selected_channel = 0; }
    }

    /// <summary>Writes the header and one row per channel into the Timeline tab.</summary>
    private void refresh_timeline() {
        if (ui == null || !animation_mode) { return; }
        if (timeline_clip is not { } clip) {
            ui.set_text("timeline_summary", animation_preview() == null
                ? "Empty preview: open an asset to animate"
                : "No motion clip");
            ui.set_text("timeline_source", string.Empty);
            ui.try_set_markup("timeline_rows", string.Empty);
            return;
        }
        var header = new StringBuilder();
        header.Append(clip.clip_name);
        header.Append(" - ");
        header.Append(clip.channels.Count.ToString(CultureInfo.InvariantCulture));
        header.Append(" channel");
        if (clip.channels.Count != 1) { header.Append('s'); }
        header.Append(", ");
        header.Append(clip.total_keys.ToString(CultureInfo.InvariantCulture));
        header.Append(" keys, ");
        header.Append(clip.duration.ToString("0.00", CultureInfo.InvariantCulture));
        header.Append(" s");
        ui.set_text("timeline_summary", header.ToString());
        ui.set_text("timeline_source", clip.buffer_path == null ? "embedded buffer" : Path.GetFileName(clip.buffer_path));
        var markup = new StringBuilder();
        for (var index = 0; index < clip.channels.Count; ++index) {
            var channel = clip.channels[index];
            markup.Append("<div id=\"timeline_channel_");
            markup.Append(index.ToString(CultureInfo.InvariantCulture));
            markup.Append("\" class=\"timeline_row");
            if (index == timeline_selected_channel) { markup.Append(" selected"); }
            markup.Append("\">");
            markup.Append("<div class=\"timeline_node\">");
            markup.Append(escape_markup(channel.node_name));
            markup.Append("</div><div class=\"timeline_path\">");
            markup.Append(escape_markup(channel.path));
            markup.Append("</div><div class=\"timeline_keys\">");
            markup.Append(channel.key_count.ToString(CultureInfo.InvariantCulture));
            markup.Append(" keys</div><div class=\"timeline_span\">");
            markup.Append(channel.start_time.ToString("0.00", CultureInfo.InvariantCulture));
            markup.Append(" - ");
            markup.Append(channel.end_time.ToString("0.00", CultureInfo.InvariantCulture));
            markup.Append(" s</div></div>");
        }
        ui.try_set_markup("timeline_rows", markup.ToString());
    }

    /// <summary>
    /// Routes Timeline clicks. The header's Open button fills the empty preview from the content
    /// browser selection; a row click seeks the preview player to that channel's first key, which makes
    /// the selection visible in the viewport without editing anything yet.
    /// </summary>
    private bool handle_timeline_pointer(rml_event input) {
        if (!animation_mode || active_dock_tab != "tab_timeline" || ui == null ||
            input.type != rml_event_type.click) {
            return false;
        }
        if (input.id == "timeline_open") {
            open_animation_selected();
            return true;
        }
        if (timeline_clip == null || !input.id.StartsWith("timeline_channel_", StringComparison.Ordinal)) {
            return false;
        }
        if (!int.TryParse(input.id["timeline_channel_".Length..], NumberStyles.Integer,
                         CultureInfo.InvariantCulture, out var index) || timeline_clip.find(index) is not { } channel) {
            return true;
        }
        timeline_selected_channel = index;
        refresh_timeline();
        seek_animation(channel.start_time);
        notice = $"{channel.describe()}: {channel.key_count} keys";
        return true;
    }

    private static string escape_markup(string text) {
        if (text.Length == 0) { return string.Empty; }
        var escaped = new StringBuilder(text.Length);
        foreach (var character in text) {
            switch (character) {
            case '&': escaped.Append("&amp;"); break;
            case '<': escaped.Append("&lt;"); break;
            case '>': escaped.Append("&gt;"); break;
            case '"': escaped.Append("&quot;"); break;
            default: escaped.Append(character); break;
            }
        }
        return escaped.ToString();
    }
}
