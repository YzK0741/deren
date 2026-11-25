// module version: 0.1.0
// The editor UI overlay: one shader pair, one glyph atlas, and the geometry the managed frontend
// describes each frame. It reuses the RmlUi overlay's vertex format and pipeline shape on purpose -
// the editor owns its widget logic, not its shading - so the two overlays cannot drift apart in
// blending, sRGB handling or HiDPI mapping.
export module deren.ui.ui_overlay;

export import deren.promise.rhi;
export import deren.scripting.editor_ui_api;
export import deren.vstd;
import deren.utility;

export namespace deren::ui {
    class ui_overlay {
    public:
        explicit ui_overlay(deren::promise::rhi::api_core& core) noexcept;
        ~ui_overlay();
        ui_overlay(ui_overlay const&) = delete;
        ui_overlay& operator=(ui_overlay const&) = delete;

        // Loads the shared UI shaders and opens the font the glyph atlas rasterises from. The
        // fallback font is used when the primary one has no glyph for a code point (CJK text with a
        // Latin-only UI font), and may be empty to disable that.
        bool initialize(std::filesystem::path const& shaders, std::filesystem::path const& font,
                        std::filesystem::path const& fallback_font);
        // Copies one frame. Called from the frame thread after the managed update; the borrowed arrays
        // are copied, so the managed frontend may reuse its buffers straight away.
        bool submit(deren::scripting::editor_ui_frame const& frame);
        // Advance and line height of one single-line UTF-8 run, in logical pixels.
        bool measure_text(std::string_view text, float size, float& width, float& height);
        // Shaped glyphs of one run in logical pixels; see editor_ui_glyph. A zero capacity measures
        // only, which the caller uses to size its buffer.
        bool shape_text(std::string_view text, float size, deren::scripting::editor_ui_glyph* glyphs,
                        std::uint32_t capacity, std::uint32_t& count, float& width);
        // Logical window size and framing ratio of the last prepared frame. The frontend lays out in
        // these units; `scale` is framebuffer pixels per logical pixel.
        void query_viewport(float& width, float& height, float& scale) const noexcept;
        // How the glyph atlas is doing: its side in pixels, the number of cached glyphs, and how many
        // could not be rasterised because even the largest atlas was full. A frontend cannot tell a
        // full atlas from a font without a glyph, so it can ask instead of shipping invisible text.
        void atlas_stats(std::uint32_t& size, std::uint32_t& glyphs, std::uint32_t& skipped) const noexcept;
        // Turns the submitted frame into vertex and glyph-atlas data for @p slot. `width`/`height` are
        // framebuffer pixels and @p pixel_ratio converts them to the logical units the frame is in.
        bool prepare(std::uint32_t width, std::uint32_t height, deren::promise::rhi::image_format format,
                     std::uint32_t slot, float pixel_ratio);
        static deren::promise::rhi::mergeable_command_buffer_ref record(
            void* context, deren::promise::rhi::command_buffer_begin_info const& begin);

    private:
        struct vertex {
            float x, y;
            float u, v;
            std::uint32_t color;
        };
        struct batch {
            deren::promise::rhi::rect clip;
            std::uint32_t first_vertex;
            std::uint32_t vertex_count;
        };
        struct glyph {
            float x0, y0, x1, y1; // Quad relative to the pen position, y down from the baseline.
            float u0, v0, u1, v1;
            float advance;
        };
        struct frame_resources {
            deren::promise::rhi::unique_object<deren::promise::rhi::buffer> vertices;
            deren::utility::vessel::vector<batch> batches;
            std::uint64_t vertex_address = 0;
        };

        // Copies of the submitted frame, kept until prepare turns them into geometry.
        deren::utility::vessel::vector<deren::scripting::editor_ui_rect> rects;
        deren::utility::vessel::vector<deren::scripting::editor_ui_text> texts;
        deren::utility::vessel::vector<deren::scripting::editor_ui_draw> draws;
        deren::utility::vessel::vector<char> blob;
        float frame_width = 0.0F, frame_height = 0.0F, frame_scale = 1.0F;

        deren::promise::rhi::api_core& core;
        deren::utility::vessel::vector<std::uint8_t> vertex_code, fragment_code;
        deren::promise::rhi::unique_object<deren::promise::rhi::pipeline> pipeline;
        deren::promise::rhi::shared_object<deren::promise::rhi::sampler> sampler;
        deren::utility::vessel::array<frame_resources, deren::promise::rhi::max_frames_in_flight> frames;
        std::uint32_t current_slot = 0;
        std::uint32_t width = 0, height = 0;
        // Logical viewport of the prepared frame, as the managed frontend lays out in.
        float viewport_width = 0.0F, viewport_height = 0.0F, display_scale = 1.0F;
        deren::promise::rhi::image_format format = deren::promise::rhi::image_format::unknown;

        // Glyph atlas: one RGBA image, coverage in alpha, white in RGB so a rect can sample any texel.
        void* font_library = nullptr;
        void* font_face = nullptr;
        // HarfBuzz face/font over the same font file: the shaper decides which glyphs a run becomes and
        // where they land, FreeType only rasterises them.
        void* hb_blob = nullptr;
        void* hb_face = nullptr;
        void* hb_font = nullptr;
        void* hb_fallback_blob = nullptr;
        void* hb_fallback_face = nullptr;
        void* hb_fallback_font = nullptr;
        // The font file bytes back the HarfBuzz blobs; the faces only borrow from them.
        deren::utility::vessel::vector<std::uint8_t> font_bytes;
        deren::utility::vessel::vector<std::uint8_t> fallback_font_bytes;
        void* fallback_face = nullptr;
        // Pixel size each face was last sized to; FreeType tracks it per face, so both are needed once a
        // run mixes the primary and the fallback font.
        std::int32_t font_pixel_size[2] = {0, 0};
        deren::utility::vessel::vector<std::uint8_t> atlas_pixels;
        bool atlas_dirty = false;
        deren::promise::rhi::shared_object<deren::promise::rhi::image> atlas_image;
        deren::promise::rhi::resource_group_ref atlas_group;
        // Atlas shelf packer and the glyphs rasterised into it so far. The atlas starts at 1024 and
        // doubles (up to 4096) when a new glyph does not fit, so a long session cannot silently lose
        // glyphs after the first screenful of text.
        std::uint32_t atlas_size = 1024;
        std::uint32_t atlas_pen_x = 1, atlas_pen_y = 0, atlas_row_height = 0;
        deren::utility::vessel::map<std::uint64_t, glyph> glyphs;
        bool atlas_full_logged = false;
        // Glyphs that had nowhere to go. The overlay keeps drawing with what it has, and the count is
        // what `atlas_stats` reports so a frontend can see its own text start to disappear.
        std::uint32_t glyphs_skipped = 0;

        // One shaped glyph in device pixels with its cluster (UTF-8 byte offset in the whole text) and
        // the font it came from (0 primary, 1 fallback).
        struct shaped_glyph {
            float advance;
            float x_offset;
            float y_offset;
            std::uint32_t glyph;
            std::uint32_t cluster;
            std::uint32_t font_index;
        };

        glyph const* glyph_for(std::uint32_t font_index, std::uint32_t glyph_id, std::int32_t pixel_size);
        // Doubles the atlas and rescales every recorded uv, so nothing has to be rasterised twice.
        bool grow_atlas();
        // Shapes one run, splitting it wherever the primary font has no glyph so CJK text and Latin text
        // are shaped by the face that actually covers them. Clusters stay offsets into @p text.
        void shape_runs(std::string_view text, std::int32_t pixel_size,
                        deren::utility::vessel::vector<shaped_glyph>& out);
        // Shapes one segment that one font covers; @p text_offset is where the segment starts in the run
        // the caller is shaping.
        void shape_segment(std::string_view text, std::size_t text_offset, std::uint32_t font_index,
                           std::int32_t pixel_size, deren::utility::vessel::vector<shaped_glyph>& out);
        // Sizes the face FreeType rasterises from, remembering what each face is at.
        [[nodiscard]] bool ensure_pixel_size(void* face, std::size_t font_index, std::int32_t pixel_size);
        [[nodiscard]] std::uint32_t font_index_for_code_point(std::uint32_t codepoint, std::int32_t pixel_size) const;
        bool update_atlas_resources();
        void append_rounded_rect(deren::utility::vessel::vector<vertex>& out,
                                 deren::scripting::editor_ui_rect const& rect, std::uint32_t color, float inset);
        void append_text(deren::utility::vessel::vector<vertex>& out, deren::scripting::editor_ui_text const& text);
        [[nodiscard]] std::pair<float, float> measure_line(std::string_view text, std::int32_t pixel_size);
    };
} // namespace deren::ui
