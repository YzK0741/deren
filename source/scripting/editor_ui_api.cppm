// module version: 0.1.0
// Immediate-mode editor UI contract. The managed frontend owns layout, interaction and paint order;
// this table only hands the native side one finished frame plus a text measurement. No engine type
// crosses it: the frame is a flat description of shaded rectangles and UTF-8 text runs, so a second
// frontend (or none) can be installed without the managed side knowing which one runs.
export module deren.scripting.editor_ui_api;
export import deren.vstd;
export import deren.scripting.api_types;

export namespace deren::scripting {
    // One rectangle in logical (density-independent) pixels, filled and/or stroked. Colour is RGBA8:
    // byte 0 red, 1 green, 2 blue, 3 alpha, matching the overlay's vertex colour order.
    // flags: bit 0 draws the fill, bit 1 draws the border. The border is inset from the outline by
    // border_width, so a 1px border sits inside the reported rect and no layout shifts when it appears.
    struct editor_ui_rect {
        float x = 0.0F;
        float y = 0.0F;
        float width = 0.0F;
        float height = 0.0F;
        // Corner radii, clockwise from the top-left: used by the fill and by the border's outer edge.
        float radius[4] = {};
        std::uint32_t fill = 0U;
        std::uint32_t border = 0U;
        float border_width = 0.0F;
        std::uint32_t flags = 0U;
        std::uint32_t reserved[2] = {};
    };
    // One single-line UTF-8 run. The overlay lays the glyphs out itself: no wrapping, no markup. The
    // baseline follows the font's ascender, so y is the top of the line box the caller measured with
    // measure_text.
    struct editor_ui_text {
        float x = 0.0F;
        float y = 0.0F;
        // Em size in logical pixels. The overlay rounds to the nearest whole pixel when it rasterises.
        float size = 0.0F;
        std::uint32_t color = 0xffffffffU;
        std::uint32_t offset = 0U; // Byte offset into editor_ui_frame::blob.
        std::uint32_t length = 0U; // Byte length; the run may contain multi-byte code points.
        std::uint32_t reserved[2] = {};
    };
    // A clip rectangle in logical pixels; the overlay converts it to a framebuffer scissor.
    struct editor_ui_clip {
        float x = 0.0F;
        float y = 0.0F;
        float width = 0.0F;
        float height = 0.0F;
    };
    // One contiguous slice of the frame that shares a clip rectangle. Within a draw the rectangles are
    // painted before the text runs, so a caller orders a panel's background, its rows and its labels by
    // placing them in one draw or by splitting draws when their clips differ.
    struct editor_ui_draw {
        std::uint32_t rect_first = 0U;
        std::uint32_t rect_count = 0U;
        std::uint32_t text_first = 0U;
        std::uint32_t text_count = 0U;
        editor_ui_clip clip{};
    };
    // One finished frame. Pointers are borrowed for the duration of submit and copied into GPU
    // resources before it returns, so the managed side may reuse its buffers immediately afterwards.
    // width/height are logical pixels; scale is framebuffer pixels per logical pixel (Windows scaling).
    struct editor_ui_frame {
        std::uint32_t version = 1U;
        std::uint32_t size = sizeof(editor_ui_frame);
        float width = 0.0F;
        float height = 0.0F;
        float scale = 1.0F;
        float reserved0 = 0.0F;
        editor_ui_rect const* rects = nullptr;
        std::uint32_t rect_count = 0U;
        std::uint32_t reserved1 = 0U;
        editor_ui_text const* texts = nullptr;
        std::uint32_t text_count = 0U;
        std::uint32_t reserved2 = 0U;
        char const* blob = nullptr;
        std::uint32_t blob_size = 0U;
        std::uint32_t reserved3 = 0U;
        editor_ui_draw const* draws = nullptr;
        std::uint32_t draw_count = 0U;
        std::uint32_t reserved4 = 0U;
    };
    // One shaped glyph of a single-line run, in logical pixels. `cluster` is the UTF-8 byte offset in
    // the run that produced it, so a caret sits between clusters rather than between code points (a
    // ligature or mark sequence is one cluster); several glyphs may share one cluster.
    struct editor_ui_glyph {
        float advance = 0.0F;   // Advance to the next glyph, including kerning.
        float x_offset = 0.0F;  // Placement offset before the pen advance.
        float y_offset = 0.0F;
        std::uint32_t glyph = 0U; // Font glyph id, for diagnostics only.
        std::uint32_t cluster = 0U;
    };
    // One IME notification. `text` is the composition preview for `update` and the committed text for
    // `commit`; `caret` is the preview caret as a UTF-16 unit offset, which is what C# string indices
    // are, and is zero for every other kind.
    struct editor_ui_ime_event {
        std::uint32_t kind = 0U; // 0 start, 1 update, 2 commit, 3 end.
        utf8_view text{};
        std::int32_t caret = 0;
        std::uint32_t reserved = 0U;
    };
    using editor_ui_ime_handler = void (*)(void* opaque, editor_ui_ime_event const* event);
    // How the glyph atlas is doing: its current side in pixels, how many glyphs are cached in it, and
    // how many could not be rasterised because even the largest atlas was full. A frontend that only
    // sees its own text cannot tell a full atlas from a font without a glyph, so it can ask here.
    struct editor_ui_stats {
        std::uint32_t atlas_size = 0U;
        std::uint32_t glyphs = 0U;
        std::uint32_t skipped = 0U;
        std::uint32_t reserved = 0U;
    };
    struct editor_ui_api {
        // Copies the frame. A zero-size window or a frame with no draws clears the overlay for that
        // frame instead of failing, which is what a hidden editor wants.
        script_status (*submit)(editor_ui_frame const* frame) = nullptr;
        // Advances and line height of one single-line run, in logical pixels. Null pointers are
        // ignored, so a caller can ask for either alone.
        script_status (*measure_text)(utf8_view text, float size, float* width, float* height) = nullptr;
        // Logical window size and the framebuffer-pixels-per-logical-pixel ratio of the frame being
        // prepared. The frontend lays out in logical units, so it asks for these instead of guessing
        // from the render extent. Null pointers are ignored.
        script_status (*query_viewport)(float* width, float* height, float* scale) = nullptr;
        // Shapes one single-line run with the same shaper and font the overlay paints with, and
        // reports the glyphs in logical pixels. A capacity of zero only measures: `count` receives
        // the number of glyphs and `width` the run width, so a caller can size its buffer first.
        script_status (*shape_text)(utf8_view text, float size, editor_ui_glyph* glyphs,
                                    std::uint32_t capacity, std::uint32_t* count, float* width) = nullptr;
        // Receives the window's composition and commit notifications. A null handler unregisters, so
        // a frontend without a focused field can stop the traffic. The handler runs on the render
        // thread, between frames, which is where the frontend already runs.
        script_status (*set_ime_handler)(editor_ui_ime_handler handler, void* opaque) = nullptr;
        // Where the composition UI should point, in logical pixels: the caret's box. A zero width or
        // height clears it, which is what a frontend does when no field has the focus.
        script_status (*set_ime_caret_rect)(float x, float y, float width, float height) = nullptr;
        // True while the pointer is over an element the frontend owns, so the app can keep the camera
        // from orbiting under a drag that belongs to a control. The window seam only gates the
        // camera forwarding: the frontend still receives the event itself.
        script_status (*set_pointer_claim)(std::uint8_t over_ui) = nullptr;
        // Fills the atlas report. The overlay grows the atlas by doubling it, so a frontend that reads
        // `skipped` growing past zero knows the text will start to lose glyphs.
        script_status (*query_stats)(editor_ui_stats* stats) = nullptr;
    };
    static_assert(sizeof(editor_ui_api) == 8 * sizeof(void*));
    static_assert(sizeof(editor_ui_stats) == 16);

    static_assert(sizeof(editor_ui_rect) == 56);
    static_assert(sizeof(editor_ui_text) == 32);
    static_assert(sizeof(editor_ui_glyph) == 20);
    static_assert(sizeof(editor_ui_clip) == 16);
    static_assert(sizeof(editor_ui_draw) == 32);
    static_assert(sizeof(editor_ui_frame) == 88);
} // namespace deren::scripting
