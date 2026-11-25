using System;

namespace deren.editor {
    /// <summary>
    /// A vertical list that builds only the rows it can show. The Dope Sheet has one row per animated
    /// channel and an asset can animate hundreds of them, while the sheet shows a couple of dozen; the
    /// list therefore keeps the index of its first visible row and asks the screen for those rows only.
    /// The wheel moves that index by whole rows, so a row never sits half outside the clip, and a thin
    /// scrollbar beside the rows shows where the window sits in the whole list.
    /// State lives in this object and not in the tree, because the tree is rebuilt every frame.
    /// </summary>
    internal sealed class ui_list_view {
        internal float row_height = ui_theme.row_height;
        internal float row_gap = 2.0F;
        internal int first;

        internal float pitch => row_height + row_gap;

        /// <summary>How many whole rows fit in a list of this height; at least one, so a shrinking
        /// window still shows something and the index stays meaningful.</summary>
        internal int visible_rows(float height) => Math.Max(1, (int)MathF.Floor((height + row_gap) / pitch));

        internal int maximum_first(int item_count, float height) =>
            Math.Max(0, item_count - visible_rows(height));

        /// <summary>Keeps the first visible row inside the list, which a resize can move it past.</summary>
        internal void clamp(int item_count, float height) =>
            first = Math.Clamp(first, 0, maximum_first(item_count, height));

        /// <summary>Moves the window by whole rows, one row per wheel step.</summary>
        internal void scroll(int item_count, float height, float wheel) {
            clamp(item_count, height);
            first = Math.Clamp(first - (int)MathF.Round(wheel), 0, maximum_first(item_count, height));
        }

        /// <summary>
        /// The list itself: the visible rows in a clipped column and its scrollbar beside them. The
        /// caller builds a row for an absolute index, so the same control serves channels, assets and
        /// any other long vertical list. Rows keep their absolute index in their key, which is what the
        /// pointer handling parses back out.
        /// </summary>
        internal ui_element build(string key, float height, int item_count, Func<int, ui_element> row_for) {
            clamp(item_count, height);
            int rows = visible_rows(height);
            var list = ui_node.column(key).size(float.NaN, height).spacing(row_gap).clipped();
            for (var index = first; index < Math.Min(item_count, first + rows); ++index) {
                list.add(row_for(index));
            }
            var frame = ui_node.row(key + "_frame").size(float.NaN, height).spacing(4.0F);
            frame.add(list.weight(1.0F));
            frame.add(ui_node.box(key + "_scroll").size(6.0F, height)
                          .painted((paint, area) => paint_scrollbar(paint, area, item_count, rows)));
            return frame;
        }

        /// <summary>The scrollbar: a rounded track and a thumb whose length and offset are the window.</summary>
        private void paint_scrollbar(ui_paint paint, ui_rect area, int item_count, int rows) {
            if (item_count <= rows || area.is_empty) { return; }
            float radius = area.width * 0.5F;
            paint.add_rect(area, radius, ui_theme.row_odd, 0U, 0.0F);
            float thumb_height = MathF.Max(18.0F, area.height * rows / item_count);
            float maximum = MathF.Max(1, item_count - rows);
            float thumb_y = area.y + (area.height - thumb_height) * Math.Clamp(first / (float)maximum, 0.0F, 1.0F);
            paint.add_rect(new ui_rect(area.x, thumb_y, area.width, thumb_height), radius, ui_theme.outline, 0U, 0.0F);
        }
    }
}
