using System;
using System.Runtime.InteropServices;
using System.Threading;

namespace deren.scripting {
    /// <summary>ABI 47: one rounded rectangle of the editor frame, in logical pixels.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct editor_ui_rect_layout {
        internal float x;
        internal float y;
        internal float width;
        internal float height;
        /// <summary>Corner radii: top-left, top-right, bottom-right, bottom-left.</summary>
        internal fixed float radius[4];
        internal uint fill;
        internal uint border;
        internal float border_width;
        /// <summary>Bit 0 fills, bit 1 strokes.</summary>
        internal uint flags;
        internal uint reserved0;
        internal uint reserved1;
    }

    /// <summary>ABI 47: one text run; the string lives in the frame's blob.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct editor_ui_text_layout {
        internal float x;
        internal float y;
        internal float size;
        internal uint color;
        internal uint offset;
        internal uint length;
        internal uint reserved0;
        internal uint reserved1;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct editor_ui_clip_layout {
        internal float x;
        internal float y;
        internal float width;
        internal float height;
    }

    /// <summary>ABI 47: one draw range; its rectangles are painted before its texts.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct editor_ui_draw_layout {
        internal uint rect_first;
        internal uint rect_count;
        internal uint text_first;
        internal uint text_count;
        internal editor_ui_clip_layout clip;
    }

    /// <summary>ABI 47: one submitted frame. Buffers are borrowed for the duration of submit().</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct editor_ui_frame_layout {
        internal uint version;
        internal uint size;
        internal float width;
        internal float height;
        internal float scale;
        internal float reserved0;
        internal editor_ui_rect_layout* rects;
        internal uint rect_count;
        internal uint reserved1;
        internal editor_ui_text_layout* texts;
        internal uint text_count;
        internal uint reserved2;
        internal byte* blob;
        internal uint blob_size;
        internal uint reserved3;
        internal editor_ui_draw_layout* draws;
        internal uint draw_count;
        internal uint reserved4;
    }

    /// <summary>ABI 47: one shaped glyph in logical pixels; several glyphs may share one cluster.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct editor_ui_glyph_layout {
        internal float advance;
        internal float x_offset;
        internal float y_offset;
        internal uint glyph;
        internal uint cluster;
    }

    /// <summary>ABI 47: one composition notification; `text` is the preview or the committed run.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct editor_ui_ime_event_layout {
        internal uint kind; // 0 start, 1 update, 2 commit, 3 end.
        internal utf8_view text;
        internal int caret;
        internal uint reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct editor_ui_stats_layout {
        internal uint atlas_size; // Side of the glyph atlas in pixels.
        internal uint glyphs;     // Glyphs cached in it.
        internal uint skipped;    // Glyphs dropped because even the largest atlas was full.
        internal uint reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct editor_ui_api_layout {
        internal delegate* unmanaged<editor_ui_frame_layout*, script_status> submit;
        internal delegate* unmanaged<utf8_view, float, float*, float*, script_status> measure_text;
        internal delegate* unmanaged<float*, float*, float*, script_status> query_viewport;
        internal delegate* unmanaged<utf8_view, float, editor_ui_glyph_layout*, uint, uint*, float*, script_status> shape_text;
        internal delegate* unmanaged<delegate* unmanaged<void*, editor_ui_ime_event_layout*, void>, void*, script_status> set_ime_handler;
        internal delegate* unmanaged<float, float, float, float, script_status> set_ime_caret_rect;
        internal delegate* unmanaged<byte, script_status> set_pointer_claim;
        internal delegate* unmanaged<editor_ui_stats_layout*, script_status> query_stats;
        internal readonly bool is_valid() =>
            submit != null && measure_text != null && query_viewport != null && shape_text != null &&
            set_ime_handler != null && set_ime_caret_rect != null && set_pointer_claim != null &&
            query_stats != null;
    }

    internal static unsafe class editor_ui_api {
        internal static editor_ui_api_layout table;
        private static int generation;
        private static int owner_thread;
        internal static int epoch => Volatile.Read(ref generation);
        internal static bool is_current(int value) => value == epoch && table.is_valid();
        internal static bool is_ready() {
            if (!host_call_guard.host_calls_allowed || Environment.CurrentManagedThreadId != owner_thread) {
                host_call_guard.note_refusal();
                return false;
            }
            return table.is_valid();
        }
        /// <summary>
        /// Logical window size and framing ratio of the frame being prepared. The frontend lays out in
        /// these units, so it asks instead of guessing from the render extent.
        /// </summary>
        internal static bool try_query_viewport(out float width, out float height, out float scale) {
            width = 1.0F;
            height = 1.0F;
            scale = 1.0F;
            if (!is_ready()) { return false; }
            float resolved_width = 0.0F;
            float resolved_height = 0.0F;
            float resolved_scale = 1.0F;
            if (table.query_viewport(&resolved_width, &resolved_height, &resolved_scale) != script_status.success ||
                resolved_width <= 0.0F || resolved_height <= 0.0F) {
                return false;
            }
            width = resolved_width;
            height = resolved_height;
            scale = resolved_scale > 0.0F ? resolved_scale : 1.0F;
            return true;
        }
        /// <summary>
        /// Registers the composition handler. Passing a null handler stops the traffic, which is what a
        /// frontend does when no field has the keyboard focus.
        /// </summary>
        internal static bool try_set_ime_handler(delegate* unmanaged<void*, editor_ui_ime_event_layout*, void> handler,
                                                 void* opaque) {
            if (!is_ready()) { return false; }
            return table.set_ime_handler(handler, opaque) == script_status.success;
        }
        /// <summary>
        /// Publishes the caret's box in logical pixels so the candidate window can point at it. A zero
        /// size clears it, which is what happens when the field loses the focus.
        /// </summary>
        internal static bool set_ime_caret_rect(float x, float y, float width, float height) {
            if (!is_ready()) { return false; }
            return table.set_ime_caret_rect(x, y, width, height) == script_status.success;
        }
        /// <summary>
        /// True while the pointer sits over a control the frontend owns. The window seam reads it
        /// before chaining a pointer event to the camera, so dragging a control never orbits the
        /// preview as well; the frontend still receives the event itself.
        /// </summary>
        internal static bool set_pointer_claim(bool over_ui) {
            if (!is_ready()) { return false; }
            return table.set_pointer_claim(over_ui ? (byte)1 : (byte)0) == script_status.success;
        }
        /// <summary>
        /// How the glyph atlas is doing: its side, how many glyphs it holds, and how many were dropped
        /// because even the largest atlas was full. Text that disappears looks exactly like text a font
        /// cannot spell, so a frontend can tell the two apart by asking here.
        /// </summary>
        internal static bool try_query_stats(out uint atlas_size, out uint glyphs, out uint skipped) {
            atlas_size = 0U;
            glyphs = 0U;
            skipped = 0U;
            if (!is_ready()) { return false; }
            var stats = default(editor_ui_stats_layout);
            if (table.query_stats(&stats) != script_status.success) { return false; }
            atlas_size = stats.atlas_size;
            glyphs = stats.glyphs;
            skipped = stats.skipped;
            return true;
        }
        internal static void shutdown() {
            table = default;
            owner_thread = 0;
            Interlocked.Increment(ref generation);
        }
        internal static script_status initialize(native_api* api) {
            shutdown();
            if (api == null || api->get_module == null) {
                return script_status.module_unavailable;
            }
            void* value = null;
            uint size = 0;
            var status = api->get_module((uint)native_module.editor_ui, &value, &size);
            if (status != script_status.success) {
                return status;
            }
            uint current_size = (uint)sizeof(editor_ui_api_layout);
            if (value == null || size != current_size) {
                return script_status.module_invalid;
            }
            editor_ui_api_layout resolved = default;
            Buffer.MemoryCopy(value, &resolved, current_size, size);
            if (!resolved.is_valid()) {
                return script_status.module_invalid;
            }
            table = resolved;
            owner_thread = Environment.CurrentManagedThreadId;
            return script_status.success;
        }
    }
}
