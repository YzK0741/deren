using System;

namespace deren.editor {
    /// <summary>
    /// One axis-aligned rectangle in logical pixels. The UI library works in logical units only and
    /// the native overlay converts to framebuffer pixels at submission, so nothing here knows about
    /// the display scale.
    /// </summary>
    internal readonly struct ui_rect {
        internal readonly float x;
        internal readonly float y;
        internal readonly float width;
        internal readonly float height;

        internal ui_rect(float x, float y, float width, float height) {
            this.x = x;
            this.y = y;
            this.width = width;
            this.height = height;
        }

        internal float right => x + width;
        internal float bottom => y + height;
        internal bool is_empty => width <= 0.0F || height <= 0.0F;
        internal static ui_rect empty => new(0.0F, 0.0F, 0.0F, 0.0F);

        /// <summary>Half-open test, so adjacent boxes never both claim the same pixel.</summary>
        internal bool contains(float point_x, float point_y) =>
            point_x >= x && point_x < right && point_y >= y && point_y < bottom;

        internal bool intersects(ui_rect other) =>
            other.x < right && other.right > x && other.y < bottom && other.bottom > y;

        internal ui_rect intersect(ui_rect other) {
            float left = MathF.Max(x, other.x);
            float top = MathF.Max(y, other.y);
            float right_edge = MathF.Min(right, other.right);
            float bottom_edge = MathF.Min(bottom, other.bottom);
            if (right_edge <= left || bottom_edge <= top) {
                return empty;
            }
            return new ui_rect(left, top, right_edge - left, bottom_edge - top);
        }

        internal ui_rect inset(float amount) =>
            new(x + amount, y + amount, width - amount * 2.0F, height - amount * 2.0F);

        internal ui_rect offset(float dx, float dy) => new(x + dx, y + dy, width, height);

        internal ui_rect slice_top(float amount) => new(x, y, width, MathF.Min(amount, height));

        internal ui_rect below(float amount) => new(x, y + amount, width, MathF.Max(0.0F, height - amount));
    }
}
