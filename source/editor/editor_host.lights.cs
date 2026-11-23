using System;
using System.Collections.Generic;
using deren;

namespace deren.editor {
    internal sealed unsafe partial class editor_host {
        private readonly Dictionary<string, string> light_fields = new(StringComparer.Ordinal);
        private Guid detail_light_uid;
        private point_light_component? detail_light;
        private scene_document? lights_document;
        private int lights_revision = -1;
        private bool lights_were_dragged;
        private bool lights_applied;

        private bool add_point_light() {
            if (document == null) { return false; }
            if (scene_lights.count(document.roots) >= scene_lighting.max_point_lights) {
                notice = $"A scene supports at most {scene_lighting.max_point_lights} point lights";
                return false;
            }
            orbit_camera.try_get(out var camera);
            var position = camera.target;
            position.y += 1.0f;
            if (!add_node(position: position, light: point_light_component.default_light)) { return false; }
            notice = "Added point light";
            return true;
        }

        private void refresh_point_light_details(document_node? node) {
            var uid = node?.uid ?? Guid.Empty;
            var light = node?.light;
            if (detail_light_uid == uid && detail_light == light) { return; }
            detail_light_uid = uid;
            detail_light = light;
            light_fields.Clear();
            ui?.try_set_element_style("point_light_details", "display", light.HasValue ? "block" : "none");
            if (light is not { } value) { return; }
            light_fields.Add("light_colour", project_look.vector(value.colour));
            light_fields.Add("light_intensity", project_look.number(value.intensity));
            light_fields.Add("light_range", project_look.number(value.range));
            foreach (var (key, text) in light_fields) { ui?.set_value(key, text); }
        }

        private void apply_point_light() {
            if (document?.find(selection_state?.primary ?? Guid.Empty) is not { light: { } before } node ||
                light_fields.Count != 3) { return; }
            if (!project_look.try_vector(light_fields["light_colour"], 0, 1, out var colour) ||
                !project_look.try_number(light_fields["light_intensity"], 0, float.MaxValue, out var intensity) ||
                !project_look.try_number(light_fields["light_range"], float.Epsilon, float.MaxValue, out var range)) {
                notice = "Use RGB from 0 to 1, non-negative intensity, and a positive range";
                return;
            }
            var after = new point_light_component(colour, intensity, range);
            if (before == after) { return; }
            bool assign(point_light_component value) {
                node.light = value;
                mark_dirty_and_refresh();
                return true;
            }
            assign(after);
            record_undo("Edit Point Light", () => assign(before), () => assign(after));
            update_history_buttons();
            notice = "Point light updated";
        }

        private void sync_point_lights() {
            if (document == null) { return; }
            bool dragging = gizmo.dragging != null;
            if (lights_document == document && lights_revision == document.revision && !dragging && !lights_were_dragged) {
                return;
            }
            lights_document = document;
            lights_revision = document.revision;
            lights_were_dragged = dragging;
            var lights = scene_lights.collect(document);
            if (lights.Length == 0 && !lights_applied) { return; }
            if (scene_lights.publish(lights)) { lights_applied = lights.Length != 0; }
        }
    }
}
