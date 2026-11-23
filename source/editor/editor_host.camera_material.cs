using System;
using System.Collections.Generic;
using System.Numerics;
using deren;

namespace deren.editor {
    internal sealed unsafe partial class editor_host {
        private Guid camera_details_uid;
        private float camera_details_fov;
        private bool camera_details_main;
        private string camera_fov_draft = string.Empty;
        private readonly Dictionary<string, bool> material_support = new(StringComparer.Ordinal);
        private Guid material_details_uid;
        private instance_material? material_details_value;
        private readonly Dictionary<string, string> material_fields = new(StringComparer.Ordinal);

        private void add_camera() {
            if (!orbit_camera.try_get(out var view)) { return; }
            transform_value placement = new() { scale = new vector_3 { x = 1, y = 1, z = 1 } };
            if (!scene_cameras.try_align(view, Matrix4x4.Identity, ref placement) ||
                !add_node(camera_placement: placement, camera_fov: view.vertical_fov_degrees)) { return; }
            notice = "Added camera";
        }

        private void refresh_camera_material_details(document_node? node) {
            var camera = node?.camera == true ? node : null;
            var camera_uid = camera?.uid ?? Guid.Empty;
            if (camera_details_uid != camera_uid || camera_details_fov != (camera?.camera_fov ?? 0) ||
                camera_details_main != (camera?.main_camera ?? false)) {
                camera_details_uid = camera_uid;
                camera_details_fov = camera?.camera_fov ?? 0;
                camera_details_main = camera?.main_camera ?? false;
                ui?.try_set_element_style("camera_details", "display", camera != null ? "block" : "none");
                if (camera != null) {
                    camera_fov_draft = project_look.number(camera.camera_fov);
                    ui?.set_value("camera_fov", camera_fov_draft);
                    ui?.set_text("camera_main", camera.main_camera ? "Clear Main Camera" : "Set as Main Camera");
                }
            }
            bool has_model = node?.asset is { } id && supports_material_override(id);
            var uid = has_model ? node!.uid : Guid.Empty;
            var material = has_model ? node!.material : null;
            if (material_details_uid == uid && material_details_value == material) { return; }
            material_details_uid = uid;
            material_details_value = material;
            material_fields.Clear();
            ui?.try_set_element_style("material_details", "display", has_model ? "block" : "none");
            if (!has_model) { return; }
            var value = material ?? instance_material.imported;
            material_fields.Add("material_tint", project_look.vector(value.tint));
            material_fields.Add("material_metallic", project_look.number(value.metallic));
            material_fields.Add("material_roughness", project_look.number(value.roughness));
            material_fields.Add("material_emissive", project_look.number(value.emissive));
            foreach (var (key, text) in material_fields) { ui?.set_value(key, text); }
            ui?.set_text("material_mode", material.HasValue ? "Instance override" : "Imported material");
        }

        private bool supports_material_override(string id) {
            if (material_support.TryGetValue(id, out var supported)) { return supported; }
            if (!asset_manager.try_load(id, out var asset)) { return false; }
            try {
                supported = asset_manager.supported_pipelines(asset, out var status).Length > 0;
                if (status == deren.scripting.script_status.success) { material_support[id] = supported; }
                return supported;
            } finally {
                asset_manager.release(asset);
            }
        }

        private void camera_command(string command) {
            if (document == null ||
                document.find(selection_state?.primary ?? Guid.Empty) is not { camera: true } camera) { return; }
            if (command == "camera_main") {
                var previous = scene_cameras.main(document.roots);
                var next = previous == camera ? null : camera;
                bool assign(document_node? from, document_node? to) {
                    if (from != null) { from.main_camera = false; }
                    if (to != null) { to.main_camera = true; }
                    mark_dirty_and_refresh();
                    return true;
                }
                assign(previous, next);
                record_undo("Set Main Camera", () => assign(next, previous), () => assign(previous, next));
                update_history_buttons();
                notice = next != null ? "Main camera set" : "Main camera cleared";
                return;
            }
            if (command == "camera_apply_fov") {
                if (!project_look.try_number(camera_fov_draft, 5, 150, out var fov)) {
                    notice = "Use a vertical field of view from 5 to 150 degrees";
                    return;
                }
                var previous = camera.camera_fov;
                if (previous == fov) { return; }
                bool assign(float value) {
                    camera.camera_fov = value;
                    mark_dirty_and_refresh();
                    return true;
                }
                assign(fov);
                record_undo("Set Camera Field of View", () => assign(previous), () => assign(fov));
                update_history_buttons();
                notice = "Camera field of view updated";
                return;
            }
            if (command == "camera_view") {
                notice = scene_cameras.try_pose(document, camera, out var pose) && pose.try_apply()
                    ? "Viewing through camera" : "Cannot preview the camera transform";
                return;
            }
            var before = camera.local;
            var after = before;
            if (!orbit_camera.try_get(out var view) ||
                !scene_cameras.try_parent_matrix(document.roots, camera, Matrix4x4.Identity, out var parent) ||
                !scene_cameras.try_align(view, parent, ref after)) {
                notice = "Cannot align the camera under this transform";
                return;
            }
            bool apply(transform_value value) {
                var previous = camera.local;
                camera.local = value;
                if (!push_runtime(camera)) {
                    camera.local = previous;
                    return false;
                }
                mark_dirty_and_refresh();
                return true;
            }
            if (!apply(after)) { return; }
            record_undo("Align Camera to View", () => apply(before), () => apply(after));
            update_history_buttons();
            notice = "Camera aligned to view";
        }

        private void apply_material_override(bool reset) {
            if (document?.find(selection_state?.primary ?? Guid.Empty) is not { asset: not null } node ||
                material_fields.Count != 4) { return; }
            instance_material? after = null;
            if (!reset) {
                if (!project_look.try_vector(material_fields["material_tint"], 0, 1, out var tint) ||
                    !project_look.try_number(material_fields["material_metallic"], 0, float.MaxValue, out var metallic) ||
                    !project_look.try_number(material_fields["material_roughness"], 0, float.MaxValue, out var roughness) ||
                    !project_look.try_number(material_fields["material_emissive"], 0, float.MaxValue, out var emissive)) {
                    notice = "Use RGB from 0 to 1 and non-negative material multipliers";
                    return;
                }
                after = new instance_material(tint, metallic, roughness, emissive);
            }
            var before = node.material;
            if (before == after) {
                if (reset) {
                    material_details_uid = Guid.Empty;
                    refresh_camera_material_details(node);
                }
                return;
            }
            bool assign(instance_material? value) {
                var previous = node.material;
                node.material = value;
                if (!push_runtime(node)) {
                    node.material = previous;
                    _ = push_runtime(node);
                    return false;
                }
                mark_dirty_and_refresh();
                return true;
            }
            if (!assign(after)) { return; }
            record_undo(reset ? "Reset Instance Material" : "Edit Instance Material",
                        () => assign(before), () => assign(after));
            update_history_buttons();
            notice = reset ? "Imported material restored" : "Instance material updated";
        }
    }
}
