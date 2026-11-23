using System;
using System.Collections.Generic;
using System.Numerics;
using deren.scripting;

namespace deren.editor {
    internal readonly record struct point_light_component(vector_3 colour, float intensity, float range) {
        internal static point_light_component default_light => new(new vector_3 { x = 1, y = 1, z = 1 }, 15, 5);

        internal bool valid => colour.x is >= 0 and <= 1 && colour.y is >= 0 and <= 1 &&
                               colour.z is >= 0 and <= 1 && float.IsFinite(intensity) && intensity >= 0 &&
                               float.IsFinite(range) && range > 0;
    }

    internal static class scene_lights {
        internal static int count(IReadOnlyList<document_node> nodes) {
            int total = 0;
            foreach (var node in nodes) {
                total += (node.light.HasValue ? 1 : 0) + count(node.children);
            }
            return total;
        }

        internal static point_light[] collect(scene_document document) {
            var lights = new List<point_light>();
            visit(document.roots, Matrix4x4.Identity, lights);
            return [.. lights];
        }

        private static void visit(IReadOnlyList<document_node> nodes, Matrix4x4 parent, List<point_light> lights) {
            foreach (var node in nodes) {
                if (!node.visible) { continue; }
                var world = viewport_gizmo.local_matrix(node.local) * parent;
                if (node.light is { } light) {
                    lights.Add(new point_light {
                        position = new vector_3 { x = world.M41, y = world.M42, z = world.M43 },
                        colour = light.colour, intensity = light.intensity, range = light.range,
                    });
                }
                visit(node.children, world, lights);
            }
        }

        internal static bool publish(point_light[] lights) {
            if (scene_lighting.try_set_point_lights(lights, out var status)) { return true; }
            host_logger.write($"Scene: could not apply point lights ({status})");
            return false;
        }
    }
}
