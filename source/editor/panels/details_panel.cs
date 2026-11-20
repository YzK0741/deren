using System;
using System.Globalization;

namespace deren.editor {
    /// <summary>
    /// The editable properties of one node, addressed by the fixed row ids authored in ui/editor.rml.
    /// </summary>
    internal enum detail_field {
        name,
        position_x,
        position_y,
        position_z,
        rotation_x,
        rotation_y,
        rotation_z,
        scale_x,
        scale_y,
        scale_z,
        flags,
    }

    /// <summary>
    /// Drives the details panel of ui/editor.rml: the editable properties of the selected node.
    /// </summary>
    /// <remarks>
    /// The managed RmlUi bridge can only address elements that already exist in the markup, so the
    /// rows are authored once and reused here - the same approach the outliner takes. Values are
    /// rewritten only when the selection changes or an edit asks for it; rewriting every frame would
    /// overwrite the text the user is still typing. Transform rows are plain inputs on purpose:
    /// RmlUi gives a checkbox change event a value this bridge cannot round-trip reliably, while a
    /// button row is unambiguous.
    /// </remarks>
    internal sealed class details_panel {
        private const string empty_id = "details_empty";
        private const string form_id = "details_form";
        private const string title_id = "details_title";
        private const string name_id = "detail_name";
        private const string visible_id = "detail_visible";
        private const string flags_id = "detail_flags";
        private const string asset_id = "detail_asset";
        private const string pipeline_id = "detail_pipeline";

        private readonly rml_document ui;
        private Guid shown = Guid.Empty;

        internal details_panel(rml_document ui) {
            this.ui = ui;
        }

        /// <summary>
        /// Rewrites every row from the selected node. The redraw is skipped unless the selection changed
        /// or <paramref name="force"/> is set, so a frame cannot overwrite an in-progress edit.
        /// </summary>
        internal void refresh(scene_document document, selection selection, bool force = false) {
            if (!force && shown == selection.primary) {
                return;
            }
            shown = selection.primary;
            var node = selection.primary == Guid.Empty ? null : document.find(selection.primary);
            ui.set_visible(empty_id, node == null);
            ui.set_visible(form_id, node != null);
            ui.try_set_element_style(empty_id, "display", node == null ? "block" : "none");
            ui.try_set_element_style(form_id, "display", node != null ? "block" : "none");
            if (node == null) {
                ui.set_text(title_id, "Details");
                return;
            }

            ui.set_text(title_id, node.name.Length == 0 ? "<unnamed>" : node.name);
            ui.set_value(name_id, node.name);
            ui.set_value("detail_position_x", format(node.local.position.x));
            ui.set_value("detail_position_y", format(node.local.position.y));
            ui.set_value("detail_position_z", format(node.local.position.z));
            ui.set_value("detail_rotation_x", format(node.local.rotation_degrees.x));
            ui.set_value("detail_rotation_y", format(node.local.rotation_degrees.y));
            ui.set_value("detail_rotation_z", format(node.local.rotation_degrees.z));
            ui.set_value("detail_scale_x", format(node.local.scale.x));
            ui.set_value("detail_scale_y", format(node.local.scale.y));
            ui.set_value("detail_scale_z", format(node.local.scale.z));
            ui.set_value(flags_id, node.flags.ToString(CultureInfo.InvariantCulture));
            ui.set_text(visible_id, node.visible ? "Visible: yes" : "Visible: no");
            ui.set_text(asset_id, node.asset ?? "-");
            ui.set_text(pipeline_id, node.pipeline ?? "-");
        }

        /// <summary>
        /// Maps a change-event id back to the property it edits; false for every other element.
        /// </summary>
        internal static bool try_field(string id, out detail_field field) {
            switch (id) {
                case name_id: field = detail_field.name; return true;
                case "detail_position_x": field = detail_field.position_x; return true;
                case "detail_position_y": field = detail_field.position_y; return true;
                case "detail_position_z": field = detail_field.position_z; return true;
                case "detail_rotation_x": field = detail_field.rotation_x; return true;
                case "detail_rotation_y": field = detail_field.rotation_y; return true;
                case "detail_rotation_z": field = detail_field.rotation_z; return true;
                case "detail_scale_x": field = detail_field.scale_x; return true;
                case "detail_scale_y": field = detail_field.scale_y; return true;
                case "detail_scale_z": field = detail_field.scale_z; return true;
                case flags_id: field = detail_field.flags; return true;
                default: field = default; return false;
            }
        }

        /// <summary>Human readable property name for the undo label, mirroring Unreal's "Edit X".</summary>
        internal static string label(detail_field field) {
            switch (field) {
                case detail_field.name: return "Rename";
                case detail_field.position_x: return "Edit Position X";
                case detail_field.position_y: return "Edit Position Y";
                case detail_field.position_z: return "Edit Position Z";
                case detail_field.rotation_x: return "Edit Rotation X";
                case detail_field.rotation_y: return "Edit Rotation Y";
                case detail_field.rotation_z: return "Edit Rotation Z";
                case detail_field.scale_x: return "Edit Scale X";
                case detail_field.scale_y: return "Edit Scale Y";
                case detail_field.scale_z: return "Edit Scale Z";
                case detail_field.flags: return "Edit Flags";
                default: return "Edit Property";
            }
        }

        /// <summary>Formats a transform lane without trailing zeros but with enough digits to edit it.</summary>
        private static string format(float value) =>
            value.ToString("0.####", CultureInfo.InvariantCulture);
    }
}
