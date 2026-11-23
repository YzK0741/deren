using System;
using System.Globalization;
using System.Numerics;
using System.Text;
using deren;

namespace deren.editor;

internal sealed class viewport_box_outline {
    private readonly record struct projected_corner(Vector2 point, bool visible);
    private readonly projected_corner[] previous = new projected_corner[8];
    private Vector2 previous_extent;
    private bool previous_solid;
    private bool shown;

    internal void layout(rml_document ui, rml_rect viewport, document_node? node, Matrix4x4 parent,
                         uint width, uint height) {
        if (node == null || !(node.box_trigger || node.collision_box) || width == 0 || height == 0) {
            if (shown) { ui.try_set_element_style("box_outline", "display", "none"); }
            shown = false;
            return;
        }
        var world = viewport_gizmo.local_matrix(node.local) * parent;
        Span<projected_corner> corners = stackalloc projected_corner[8];
        for (int index = 0; index < corners.Length; ++index) {
            var corner = new Vector3((index & 1) == 0 ? -0.5f : 0.5f,
                                     (index & 2) == 0 ? -0.5f : 0.5f,
                                     (index & 4) == 0 ? -0.5f : 0.5f);
            bool visible = viewport_gizmo.project(Vector3.Transform(corner, world), viewport, width, height, out var point, out _);
            corners[index] = new projected_corner(point, visible);
        }
        var extent = new Vector2(viewport.width, viewport.height);
        if (shown && node.collision_box == previous_solid && extent == previous_extent && corners.SequenceEqual(previous)) { return; }
        var markup = new StringBuilder();
        string classes = node.collision_box ? "box_edge solid_edge" : "box_edge";
        for (int start = 0; start < corners.Length; ++start) {
            if (!corners[start].visible) { continue; }
            for (int bit = 1; bit <= 4; bit <<= 1) {
                int end = start | bit;
                if (end == start || !corners[end].visible) { continue; }
                var position = corners[start].point;
                var delta = corners[end].point - position;
                float first = 0;
                float last = 1;
                if (!clip_axis(position.X, delta.X, extent.X, ref first, ref last) ||
                    !clip_axis(position.Y, delta.Y, extent.Y, ref first, ref last)) { continue; }
                position += delta * first;
                delta *= last - first;
                float length = delta.Length();
                if (!float.IsFinite(length) || length < 0.5f) { continue; }
                float angle = MathF.Atan2(delta.Y, delta.X) * (180 / MathF.PI);
                markup.Append(CultureInfo.InvariantCulture,
                    $"""<div class="{classes}" style="left: {position.X:0.##}dp; top: {position.Y:0.##}dp; width: {length:0.##}dp; transform: rotate({angle:0.##}deg);"></div>""");
            }
        }
        if (!ui.try_set_markup("box_outline", markup.ToString())) { return; }
        corners.CopyTo(previous);
        previous_extent = extent;
        previous_solid = node.collision_box;
        ui.try_set_element_style("box_outline", "display", "block");
        shown = true;
    }

    private static bool clip_axis(float start, float delta, float limit, ref float first, ref float last) {
        if (MathF.Abs(delta) < 0.0001f) { return start >= 0 && start <= limit; }
        float a = -start / delta;
        float b = (limit - start) / delta;
        first = MathF.Max(first, MathF.Min(a, b));
        last = MathF.Min(last, MathF.Max(a, b));
        return first <= last;
    }
}
