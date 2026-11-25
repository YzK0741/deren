using System;
using deren;

namespace deren.editor {
    internal sealed unsafe partial class editor_host {
        /// <summary>
        /// Applies one component value from the inspector. The lanes mirror the classic details panel but
        /// each one writes the whole component as a single undo step, so a colour edit and a range edit are
        /// separate history entries rather than the classic draft-and-apply pair.
        /// </summary>
        private bool apply_scene_component(document_node node, string lane, string text) {
            switch (lane) {
                case "flags":
                    return apply_detail_change(detail_field.flags, text);
                case "camera_fov": {
                    if (!project_look.try_number(text, 5.0F, 150.0F, out var fov)) {
                        notice = "Use a vertical field of view from 5 to 150 degrees";
                        return false;
                    }
                    var before = node.camera_fov;
                    if (before == fov) { return false; }
                    bool assign(float value) {
                        node.camera_fov = value;
                        mark_dirty_and_refresh();
                        return true;
                    }
                    assign(fov);
                    record_undo("Set Camera Field of View", () => assign(before), () => assign(fov));
                    return true;
                }
                case "light_colour":
                case "light_intensity":
                case "light_range":
                    return apply_scene_light_lane(node, lane, text);
                case "material_tint":
                case "material_metallic":
                case "material_roughness":
                case "material_emissive":
                    return apply_scene_material_lane(node, lane, text);
                case "audio_file":
                case "audio_volume":
                    return apply_scene_audio_lane(node, lane, text);
                case "animation_clip": {
                    var previous = node.animation_clip;
                    apply_animation_clip(text);
                    return node.animation_clip != previous;
                }
                default:
                    return false;
            }
        }

        private bool apply_scene_light_lane(document_node node, string lane, string text) {
            if (node.light is not { } before) { return false; }
            var colour = before.colour;
            float intensity = before.intensity;
            float range = before.range;
            switch (lane) {
                case "light_colour":
                    if (!project_look.try_vector(text, 0.0F, 1.0F, out colour)) {
                        notice = "Use RGB from 0 to 1";
                        return false;
                    }
                    break;
                case "light_intensity":
                    if (!project_look.try_number(text, 0.0F, float.MaxValue, out intensity)) {
                        notice = "Use a non-negative intensity";
                        return false;
                    }
                    break;
                default:
                    if (!project_look.try_number(text, float.Epsilon, float.MaxValue, out range)) {
                        notice = "Use a positive range";
                        return false;
                    }
                    break;
            }
            var after = new point_light_component(colour, intensity, range);
            if (after == before) { return false; }
            // A light is a document value the lighting pass publishes from, so no runtime push is needed;
            // bumping the document revision is what makes the next frame send it to the renderer.
            bool assign(point_light_component value) {
                node.light = value;
                mark_dirty_and_refresh();
                return true;
            }
            assign(after);
            record_undo("Edit Point Light", () => assign(before), () => assign(after));
            return true;
        }

        private bool apply_scene_material_lane(document_node node, string lane, string text) {
            var before = node.material;
            var value = before ?? instance_material.imported;
            var tint = value.tint;
            float metallic = value.metallic;
            float roughness = value.roughness;
            float emissive = value.emissive;
            switch (lane) {
                case "material_tint":
                    if (!project_look.try_vector(text, 0.0F, 1.0F, out tint)) {
                        notice = "Use RGB from 0 to 1";
                        return false;
                    }
                    break;
                case "material_metallic":
                    if (!project_look.try_number(text, 0.0F, float.MaxValue, out metallic)) {
                        notice = "Use a non-negative metallic multiplier";
                        return false;
                    }
                    break;
                case "material_roughness":
                    if (!project_look.try_number(text, 0.0F, float.MaxValue, out roughness)) {
                        notice = "Use a non-negative roughness multiplier";
                        return false;
                    }
                    break;
                default:
                    if (!project_look.try_number(text, 0.0F, float.MaxValue, out emissive)) {
                        notice = "Use a non-negative emissive multiplier";
                        return false;
                    }
                    break;
            }
            var after = new instance_material(tint, metallic, roughness, emissive);
            if (after == before) { return false; }
            bool assign(instance_material? next) {
                var previous = node.material;
                node.material = next;
                if (!push_runtime(node)) {
                    node.material = previous;
                    _ = push_runtime(node);
                    return false;
                }
                mark_dirty_and_refresh();
                return true;
            }
            if (!assign(after)) { return false; }
            record_undo("Edit Instance Material", () => assign(before), () => assign(after));
            return true;
        }

        private bool apply_scene_audio_lane(document_node node, string lane, string text) {
            if (node.audio_source is not { } before) { return false; }
            string file = before.file;
            float volume = before.volume;
            if (lane == "audio_file") {
                file = text.Trim();
                if (file.Length != 0 && !audio_source_component.valid_reference(file)) {
                    notice = "Use a clip path under the project audio folder";
                    return false;
                }
            } else {
                if (!project_look.try_number(text, 0.0F, 1.0F, out volume)) {
                    notice = "Use a volume from 0 to 1";
                    return false;
                }
            }
            var after = new audio_source_component(file, volume, before.loop);
            if (after == before) { return false; }
            bool assign(audio_source_component value) {
                node.audio_source = value;
                mark_dirty_and_refresh();
                return true;
            }
            assign(after);
            record_undo("Edit Audio Source", () => assign(before), () => assign(after));
            return true;
        }

        /// <summary>Flips one boolean component; the classic shell's own toggles do the work where they exist.</summary>
        private bool toggle_scene_component(document_node node, string lane) {
            switch (lane) {
                case "collision_box":
                    toggle_volume(solid: true);
                    return true;
                case "box_trigger":
                    toggle_volume(solid: false);
                    return true;
                case "moving_solid":
                    toggle_moving_solid();
                    return true;
                case "trigger_target":
                    toggle_trigger_target();
                    return true;
                case "animation_autoplay":
                    toggle_animation_autoplay();
                    return true;
                case "main_camera":
                    camera_command("camera_main");
                    return true;
                case "audio_loop": {
                    if (node.audio_source is not { } before) { return false; }
                    var after = new audio_source_component(before.file, before.volume, !before.loop);
                    bool assign(audio_source_component value) {
                        node.audio_source = value;
                        mark_dirty_and_refresh();
                        return true;
                    }
                    assign(after);
                    record_undo("Set Audio Loop", () => assign(before), () => assign(after));
                    return true;
                }
                default:
                    return false;
            }
        }

        /// <summary>Seeds the inline editor for one component lane from the node it belongs to.</summary>
        private string? seed_scene_component(document_node node, string lane) {
            switch (lane) {
                case "flags":
                    return node.flags.ToString(System.Globalization.CultureInfo.InvariantCulture);
                case "camera_fov":
                    return node.camera_fov.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture);
                case "light_colour":
                    return node.light is { } light ? format_scene_vector(light.colour) : null;
                case "light_intensity":
                    return node.light is { } intensity
                        ? intensity.intensity.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture)
                        : null;
                case "light_range":
                    return node.light is { } range
                        ? range.range.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture)
                        : null;
                case "material_tint":
                    return format_scene_vector((node.material ?? instance_material.imported).tint);
                case "material_metallic":
                    return format_scene_number((node.material ?? instance_material.imported).metallic);
                case "material_roughness":
                    return format_scene_number((node.material ?? instance_material.imported).roughness);
                case "material_emissive":
                    return format_scene_number((node.material ?? instance_material.imported).emissive);
                case "audio_file":
                    return node.audio_source?.file ?? string.Empty;
                case "audio_volume":
                    return node.audio_source is { } audio ? format_scene_number(audio.volume) : null;
                case "animation_clip":
                    return node.animation_clip.ToString(System.Globalization.CultureInfo.InvariantCulture);
                default:
                    return null;
            }
        }

        private static string format_scene_number(float value) {
            return value.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture);
        }
    }
}
