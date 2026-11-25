module;

#include <ft2build.h>
#include <hb-ot.h>
#include <hb.h>
#include FT_FREETYPE_H

module deren.ui.ui_overlay;

import deren.vstd;

namespace deren::ui {
    namespace rhi = deren::promise::rhi;
    namespace scripting = deren::scripting;
    namespace {
        constexpr std::uint32_t max_atlas_size = 4096;
        constexpr std::uint32_t white_texel = 0;

        struct draw_parameters {
            float offset_x, offset_y;
            float width, height;
            std::uint32_t srgb_output;
        };

        static_assert(sizeof(draw_parameters) == 20);

        // Decodes one UTF-8 code point, advancing @p index. Invalid bytes become U+FFFD so a broken
        // string cannot stall the loop.
        char32_t decode_utf8(std::string_view text, std::size_t& index) {
            if (index >= text.size()) {
                return U'\0';
            }
            auto const lead = static_cast<unsigned char>(text[index]);
            std::size_t length = 1;
            char32_t value = lead;
            if (lead >= 0xf0) {
                length = 4;
                value = lead & 0x07U;
            } else if (lead >= 0xe0) {
                length = 3;
                value = lead & 0x0fU;
            } else if (lead >= 0xc0) {
                length = 2;
                value = lead & 0x1fU;
            } else if (lead >= 0x80) {
                index += 1;
                return U'\ufffd';
            }
            if (index + length > text.size()) {
                index = text.size();
                return U'\ufffd';
            }
            for (std::size_t i = 1; i < length; ++i) {
                auto const next = static_cast<unsigned char>(text[index + i]);
                if ((next & 0xc0U) != 0x80U) {
                    index += i;
                    return U'\ufffd';
                }
                value = (value << 6) | (next & 0x3fU);
            }
            index += length;
            return value;
        }
    } // namespace

    ui_overlay::ui_overlay(rhi::api_core& core) noexcept
        : core(core) {
    }

    ui_overlay::~ui_overlay() {
        if (this->pipeline) {
            this->core.wait_idle();
        }
        for (auto* font : {&this->hb_font, &this->hb_fallback_font}) {
            if (*font != nullptr) {
                hb_font_destroy(static_cast<hb_font_t*>(*font));
                *font = nullptr;
            }
        }
        for (auto* face : {&this->hb_face, &this->hb_fallback_face}) {
            if (*face != nullptr) {
                hb_face_destroy(static_cast<hb_face_t*>(*face));
                *face = nullptr;
            }
        }
        for (auto* blob : {&this->hb_blob, &this->hb_fallback_blob}) {
            if (*blob != nullptr) {
                hb_blob_destroy(static_cast<hb_blob_t*>(*blob));
                *blob = nullptr;
            }
        }
        if (this->font_face != nullptr) {
            FT_Done_Face(static_cast<FT_Face>(this->font_face));
        }
        if (this->fallback_face != nullptr) {
            FT_Done_Face(static_cast<FT_Face>(this->fallback_face));
        }
        if (this->font_library != nullptr) {
            FT_Done_FreeType(static_cast<FT_Library>(this->font_library));
        }
    }

    bool ui_overlay::initialize(std::filesystem::path const& shaders, std::filesystem::path const& font,
                                std::filesystem::path const& fallback_font) {
        constexpr std::uintmax_t max_shader_bytes = 1024 * 1024;
        auto vertex_data = utility::read_binary_to_vector(shaders / "rmlui.vert.spv", max_shader_bytes);
        auto fragment_data = utility::read_binary_to_vector(shaders / "rmlui.frag.spv", max_shader_bytes);
        if (!vertex_data || !fragment_data) {
            return false;
        }
        this->sampler = rhi::shared_object<rhi::sampler>(
            this->core.resources->create_sampler({.address_mode = rhi::sampler_address_mode::clamp_to_edge}));
        if (!this->sampler) {
            return false;
        }
        this->vertex_code = std::move(*vertex_data);
        this->fragment_code = std::move(*fragment_data);

        FT_Library library = nullptr;
        if (FT_Init_FreeType(&library) != 0) {
            utility::error("editor ui: could not initialise FreeType");
            return false;
        }
        auto const font_path = font.string();
        FT_Face face = nullptr;
        if (FT_New_Face(library, font_path.c_str(), 0, &face) != 0) {
            utility::error("editor ui: could not open the font '{}'", font_path);
            FT_Done_FreeType(library);
            return false;
        }
        this->font_library = library;
        this->font_face = face;

        // The shaper works on the font file's bytes; FreeType only rasterises the glyph it picked. The
        // blob borrows the vector, which lives as long as the overlay.
        constexpr std::uintmax_t max_font_bytes = 64 * 1024 * 1024;
        auto font_data = utility::read_binary_to_vector(font, max_font_bytes);
        if (!font_data) {
            utility::error("editor ui: could not read the font '{}'", font_path);
            return false;
        }
        this->font_bytes = std::move(*font_data);
        this->hb_blob = hb_blob_create(reinterpret_cast<char const*>(this->font_bytes.data()),
                                       static_cast<unsigned int>(this->font_bytes.size()), HB_MEMORY_MODE_READONLY,
                                       nullptr, nullptr);
        this->hb_face = hb_face_create(static_cast<hb_blob_t*>(this->hb_blob), 0);
        this->hb_font = hb_font_create(static_cast<hb_face_t*>(this->hb_face));
        hb_ot_font_set_funcs(static_cast<hb_font_t*>(this->hb_font));

        // The fallback covers what the primary font cannot: a Latin-only UI font has no CJK glyphs, and
        // a run whose code points miss it is shaped and rasterised from this face instead.
        if (!fallback_font.empty()) {
            auto fallback_data = utility::read_binary_to_vector(fallback_font, max_font_bytes);
            if (fallback_data) {
                FT_Face fallback = nullptr;
                if (FT_New_Face(library, fallback_font.string().c_str(), 0, &fallback) == 0) {
                    this->fallback_face = fallback;
                    this->fallback_font_bytes = std::move(*fallback_data);
                    this->hb_fallback_blob = hb_blob_create(
                        reinterpret_cast<char const*>(this->fallback_font_bytes.data()),
                        static_cast<unsigned int>(this->fallback_font_bytes.size()), HB_MEMORY_MODE_READONLY, nullptr,
                        nullptr);
                    this->hb_fallback_face = hb_face_create(static_cast<hb_blob_t*>(this->hb_fallback_blob), 0);
                    this->hb_fallback_font = hb_font_create(static_cast<hb_face_t*>(this->hb_fallback_face));
                    hb_ot_font_set_funcs(static_cast<hb_font_t*>(this->hb_fallback_font));
                }
            }
        }

        // Reserve the white texel solid rectangles sample. It lives at (0,0) of every atlas generation
        // so a rect never needs its own texture.
        this->atlas_pixels.assign(static_cast<std::size_t>(this->atlas_size) * this->atlas_size * 4, 0);
        auto* pixels = this->atlas_pixels.data();
        for (std::uint32_t i = 0; i < 4; ++i) {
            pixels[i] = 255;
        }
        this->atlas_dirty = true;
        return true;
    }

    bool ui_overlay::submit(scripting::editor_ui_frame const& frame) {
        if (frame.version != 1U || frame.size != sizeof(scripting::editor_ui_frame)) {
            utility::error("editor ui: unsupported frame version {} (size {})", frame.version, frame.size);
            return false;
        }
        if (frame.rect_count != 0 && frame.rects == nullptr) {
            return false;
        }
        if (frame.text_count != 0 && (frame.texts == nullptr || frame.blob == nullptr)) {
            return false;
        }
        if (frame.draw_count != 0 && frame.draws == nullptr) {
            return false;
        }
        this->rects.clear();
        if (frame.rect_count != 0) {
            this->rects.assign(frame.rects, frame.rects + frame.rect_count);
        }
        this->texts.clear();
        if (frame.text_count != 0) {
            this->texts.assign(frame.texts, frame.texts + frame.text_count);
        }
        this->draws.clear();
        if (frame.draw_count != 0) {
            this->draws.assign(frame.draws, frame.draws + frame.draw_count);
        }
        this->blob.clear();
        if (frame.blob_size != 0) {
            this->blob.assign(frame.blob, frame.blob + frame.blob_size);
        }
        this->frame_width = frame.width;
        this->frame_height = frame.height;
        this->frame_scale = frame.scale > 0.0F ? frame.scale : 1.0F;
        return true;
    }

    ui_overlay::glyph const* ui_overlay::glyph_for(std::uint32_t font_index, std::uint32_t glyph_id,
                                                   std::int32_t pixel_size) {
        if (glyph_id == 0U) {
            return nullptr;
        }
        auto const key = (static_cast<std::uint64_t>(font_index) << 48) |
                         (static_cast<std::uint64_t>(static_cast<std::uint32_t>(pixel_size)) << 32) |
                         static_cast<std::uint64_t>(glyph_id);
        if (auto const found = this->glyphs.find(key); found != this->glyphs.end()) {
            return &found->second;
        }
        auto* face = static_cast<FT_Face>(font_index == 0U ? this->font_face : this->fallback_face);
        if (!this->ensure_pixel_size(face, font_index, pixel_size)) {
            return nullptr;
        }
        if (FT_Load_Glyph(face, static_cast<FT_UInt>(glyph_id), FT_LOAD_RENDER) != 0) {
            return nullptr;
        }
        auto const& slot = face->glyph;
        auto const& bitmap = slot->bitmap;
        auto const width = static_cast<std::uint32_t>(bitmap.width);
        auto const height = static_cast<std::uint32_t>(bitmap.rows);
        glyph entry{};
        entry.advance = static_cast<float>(slot->advance.x) / 64.0F;
        entry.x0 = static_cast<float>(slot->bitmap_left);
        entry.y0 = -static_cast<float>(slot->bitmap_top);
        entry.x1 = entry.x0 + static_cast<float>(width);
        entry.y1 = entry.y0 + static_cast<float>(height);
        entry.u0 = 0.0F;
        entry.v0 = 0.0F;
        entry.u1 = 0.0F;
        entry.v1 = 0.0F;
        if (width != 0 && height != 0) {
            if (this->atlas_pen_x + width + 1 > this->atlas_size) {
                this->atlas_pen_x = 1;
                this->atlas_pen_y += this->atlas_row_height + 1;
                this->atlas_row_height = 0;
            }
            if (this->atlas_pen_y + height + 1 > this->atlas_size) {
                if (!this->grow_atlas()) {
                    ++this->glyphs_skipped;
                    return nullptr;
                }
            }
            auto* pixels = this->atlas_pixels.data();
            for (std::uint32_t row = 0; row < height; ++row) {
                auto const* source = bitmap.buffer + static_cast<std::size_t>(row) * bitmap.pitch;
                auto* destination = pixels + (static_cast<std::size_t>(this->atlas_pen_y + row) * this->atlas_size +
                                              this->atlas_pen_x) *
                                                 4;
                for (std::uint32_t column = 0; column < width; ++column) {
                    // Coverage goes into alpha; RGB stays white so a rect can sample any texel of the atlas.
                    destination[column * 4 + 0] = 255;
                    destination[column * 4 + 1] = 255;
                    destination[column * 4 + 2] = 255;
                    destination[column * 4 + 3] = source[column];
                }
            }
            entry.u0 = (static_cast<float>(this->atlas_pen_x) + 0.5F) / static_cast<float>(this->atlas_size);
            entry.v0 = (static_cast<float>(this->atlas_pen_y) + 0.5F) / static_cast<float>(this->atlas_size);
            entry.u1 = (static_cast<float>(this->atlas_pen_x + width) - 0.5F) / static_cast<float>(this->atlas_size);
            entry.v1 = (static_cast<float>(this->atlas_pen_y + height) - 0.5F) / static_cast<float>(this->atlas_size);
            if (entry.u1 < entry.u0) {
                entry.u1 = entry.u0;
            }
            if (entry.v1 < entry.v0) {
                entry.v1 = entry.v0;
            }
            this->atlas_pen_x += width + 1;
            this->atlas_row_height = this->atlas_row_height > height ? this->atlas_row_height : height;
            this->atlas_dirty = true;
        }
        this->glyphs.try_emplace(key, entry);
        return &this->glyphs.find(key)->second;
    }

    bool ui_overlay::grow_atlas() {
        if (this->atlas_size >= max_atlas_size) {
            if (!this->atlas_full_logged) {
                utility::error("editor ui: the glyph atlas is full; later glyphs are skipped");
                this->atlas_full_logged = true;
            }
            return false;
        }
        auto const next = this->atlas_size * 2U;
        // The old pixels keep their top-left origin, so the copy is row by row and every recorded uv
        // just shrinks by the same ratio, which keeps already rasterised glyphs valid.
        deren::utility::vessel::vector<std::uint8_t> grown(static_cast<std::size_t>(next) * next * 4, 0);
        for (std::uint32_t row = 0; row < this->atlas_size; ++row) {
            auto const* source = this->atlas_pixels.data() + static_cast<std::size_t>(row) * this->atlas_size * 4;
            auto* destination = grown.data() + static_cast<std::size_t>(row) * next * 4;
            std::copy(source, source + static_cast<std::size_t>(this->atlas_size) * 4, destination);
        }
        this->atlas_pixels = std::move(grown);
        auto const factor = static_cast<float>(this->atlas_size) / static_cast<float>(next);
        for (auto& entry : this->glyphs) {
            entry.second.u0 *= factor;
            entry.second.v0 *= factor;
            entry.second.u1 *= factor;
            entry.second.v1 *= factor;
        }
        this->atlas_size = next;
        this->atlas_dirty = true;
        utility::log("editor ui: grew the glyph atlas to {}x{}", next, next);
        return true;
    }

    std::uint32_t ui_overlay::font_index_for_code_point(std::uint32_t codepoint,
                                                        std::int32_t /*pixel_size*/) const {
        if (this->hb_fallback_font == nullptr) {
            return 0U;
        }
        // The caller has already set the primary font's scale for this pixel size.
        auto* font = static_cast<hb_font_t*>(this->hb_font);
        hb_codepoint_t glyph = 0;
        return hb_font_get_nominal_glyph(font, static_cast<hb_codepoint_t>(codepoint), &glyph) ? 0U : 1U;
    }

    void ui_overlay::shape_runs(std::string_view text, std::int32_t pixel_size,
                                utility::vessel::vector<shaped_glyph>& out) {
        out.clear();
        if (text.empty() || this->hb_font == nullptr) {
            return;
        }
        auto* primary = static_cast<hb_font_t*>(this->hb_font);
        hb_font_set_scale(primary, pixel_size * 64, pixel_size * 64);
        std::size_t index = 0;
        std::size_t segment_begin = 0;
        std::uint32_t segment_font = 0;
        bool have_segment = false;
        while (index < text.size()) {
            std::size_t const start = index;
            auto const codepoint = decode_utf8(text, index);
            if (codepoint == U'\0') {
                break;
            }
            auto const font = this->font_index_for_code_point(static_cast<std::uint32_t>(codepoint), pixel_size);
            if (!have_segment) {
                segment_font = font;
                have_segment = true;
                continue;
            }
            if (font != segment_font) {
                this->shape_segment(text.substr(segment_begin, start - segment_begin), segment_begin, segment_font,
                                    pixel_size, out);
                segment_begin = start;
                segment_font = font;
            }
        }
        if (have_segment && segment_begin < text.size()) {
            this->shape_segment(text.substr(segment_begin), segment_begin, segment_font, pixel_size, out);
        }
    }

    void ui_overlay::shape_segment(std::string_view text, std::size_t text_offset, std::uint32_t font_index,
                                   std::int32_t pixel_size, utility::vessel::vector<shaped_glyph>& out) {
        if (text.empty()) {
            return;
        }
        auto* font = static_cast<hb_font_t*>(font_index == 0U ? this->hb_font : this->hb_fallback_font);
        if (font == nullptr) {
            return;
        }
        // The scale is the em size in 26.6 fixed point, so a HarfBuzz advance divided by 64 is a pixel.
        hb_font_set_scale(font, pixel_size * 64, pixel_size * 64);
        hb_buffer_t* buffer = hb_buffer_create();
        hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
        hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), 0, static_cast<int>(text.size()));
        hb_buffer_guess_segment_properties(buffer);
        hb_shape(font, buffer, nullptr, 0);
        unsigned int count = 0;
        auto const* infos = hb_buffer_get_glyph_infos(buffer, &count);
        auto const* positions = hb_buffer_get_glyph_positions(buffer, &count);
        out.reserve(out.size() + count);
        for (unsigned int i = 0; i < count; ++i) {
            out.push_back(shaped_glyph{
                .advance = static_cast<float>(positions[i].x_advance) / 64.0F,
                .x_offset = static_cast<float>(positions[i].x_offset) / 64.0F,
                .y_offset = static_cast<float>(positions[i].y_offset) / 64.0F,
                .glyph = infos[i].codepoint,
                .cluster = static_cast<std::uint32_t>(text_offset) + infos[i].cluster,
                .font_index = font_index,
            });
        }
        hb_buffer_destroy(buffer);
    }

    bool ui_overlay::ensure_pixel_size(void* face_pointer, std::size_t font_index, std::int32_t pixel_size) {
        if (face_pointer == nullptr || font_index >= 2U) {
            return false;
        }
        if (this->font_pixel_size[font_index] == pixel_size) {
            return true;
        }
        // Each face carries its own size, so the fallback has to be sized even when the primary already is.
        if (FT_Set_Pixel_Sizes(static_cast<FT_Face>(face_pointer), 0, static_cast<FT_UInt>(pixel_size)) != 0) {
            return false;
        }
        this->font_pixel_size[font_index] = pixel_size;
        return true;
    }

    std::pair<float, float> ui_overlay::measure_line(std::string_view text, std::int32_t pixel_size) {
        auto* face = static_cast<FT_Face>(this->font_face);
        if (face == nullptr) {
            return {0.0F, 0.0F};
        }
        if (!this->ensure_pixel_size(face, 0U, pixel_size)) {
            return {0.0F, 0.0F};
        }
        utility::vessel::vector<shaped_glyph> shaped;
        this->shape_runs(text, pixel_size, shaped);
        float width = 0.0F;
        for (auto const& glyph : shaped) {
            width += glyph.advance;
        }
        auto const height = static_cast<float>(face->size->metrics.height) / 64.0F;
        return {width, height};
    }

    bool ui_overlay::shape_text(std::string_view text, float size, scripting::editor_ui_glyph* glyphs,
                                std::uint32_t capacity, std::uint32_t& count, float& width) {
        float const scale = this->frame_scale > 0.0F ? this->frame_scale : 1.0F;
        auto const pixel_size = static_cast<std::int32_t>(std::lround(size * scale));
        count = 0U;
        width = 0.0F;
        if (pixel_size <= 0) {
            return false;
        }
        utility::vessel::vector<shaped_glyph> shaped;
        this->shape_runs(text, pixel_size, shaped);
        auto const inv = 1.0F / scale;
        float total = 0.0F;
        for (std::size_t i = 0; i < shaped.size(); ++i) {
            total += shaped[i].advance * inv;
            if (glyphs != nullptr && i < capacity) {
                glyphs[i] = scripting::editor_ui_glyph{
                    .advance = shaped[i].advance * inv,
                    .x_offset = shaped[i].x_offset * inv,
                    .y_offset = shaped[i].y_offset * inv,
                    .glyph = shaped[i].glyph,
                    .cluster = shaped[i].cluster,
                };
            }
        }
        count = static_cast<std::uint32_t>(shaped.size());
        width = total;
        return true;
    }

    void ui_overlay::query_viewport(float& width, float& height, float& scale) const noexcept {
        width = this->viewport_width;
        height = this->viewport_height;
        scale = this->display_scale;
    }

    void ui_overlay::atlas_stats(std::uint32_t& size, std::uint32_t& glyphs,
                                 std::uint32_t& skipped) const noexcept {
        size = this->atlas_size;
        glyphs = static_cast<std::uint32_t>(this->glyphs.size());
        skipped = this->glyphs_skipped;
    }

    bool ui_overlay::measure_text(std::string_view text, float size, float& width, float& height) {
        // The request is in logical pixels; glyphs are rasterized at the device density and the
        // measured box is reported back in logical pixels so the layout stays scale independent.
        float const scale = this->frame_scale > 0.0F ? this->frame_scale : 1.0F;
        auto const pixel_size = static_cast<std::int32_t>(std::lround(size * scale));
        if (pixel_size <= 0) {
            width = 0.0F;
            height = 0.0F;
            return false;
        }
        auto const [measured_width, measured_height] = this->measure_line(text, pixel_size);
        width = measured_width / scale;
        height = measured_height / scale;
        return true;
    }

    bool ui_overlay::update_atlas_resources() {
        if (!this->atlas_dirty) {
            return true;
        }
        // Atlas uploads are rare (a handful while the session warms up), so re-uploading the whole
        // image and recreating its group beats tracking in-flight generations.
        if (this->atlas_group) {
            this->core.wait_idle();
        }
        auto image = this->core.resources->create_image({
            .extent = {.width = this->atlas_size, .height = this->atlas_size, .depth = 1},
            .format = rhi::image_format::rgba8_unorm,
            .flags = rhi::to_bits(rhi::image_flag::sampled),
            .initial_bytes = std::as_bytes(std::span(this->atlas_pixels.data(), this->atlas_pixels.size())),
        });
        if (!image) {
            return false;
        }
        this->atlas_image = rhi::shared_object<rhi::image>(std::move(image));
        rhi::resource_group_entry const entries[] = {
            {.binding = 0, .kind = rhi::resource_kind::sampled_image, .resource = this->atlas_image},
            {.binding = 1, .kind = rhi::resource_kind::sampler, .resource = this->sampler},
        };
        auto group = this->core.resources->create_resource_group({.entries = entries});
        if (!group) {
            return false;
        }
        this->atlas_group = std::move(*group);
        this->atlas_dirty = false;
        return true;
    }

    void ui_overlay::append_rounded_rect(utility::vessel::vector<vertex>& out,
                                         scripting::editor_ui_rect const& rect, std::uint32_t color, float inset) {
        auto const scale = this->frame_scale;
        float const x = rect.x + inset;
        float const y = rect.y + inset;
        float width = rect.width - inset * 2.0F;
        float height = rect.height - inset * 2.0F;
        if (width <= 0.0F || height <= 0.0F) {
            return;
        }
        float radius[4];
        for (std::size_t i = 0; i < 4; ++i) {
            radius[i] = rect.radius[i] - inset;
            if (radius[i] < 0.0F) {
                radius[i] = 0.0F;
            }
        }
        auto const max_radius = (width < height ? width : height) * 0.5F;
        for (float& value : radius) {
            value = value < max_radius ? value : max_radius;
        }
        auto const u = static_cast<float>(white_texel) / static_cast<float>(this->atlas_size) + 0.5F / static_cast<float>(this->atlas_size);
        auto const v = u;
        auto const emit = [&](float px, float py) {
            // The shader maps the vertex straight into the framebuffer, so the logical coordinate is
            // converted here and nowhere else.
            out.push_back({px * scale, py * scale, u, v, color});
        };
        auto const plain = radius[0] <= 0.0F && radius[1] <= 0.0F && radius[2] <= 0.0F && radius[3] <= 0.0F;
        if (plain) {
            emit(x, y);
            emit(x + width, y);
            emit(x + width, y + height);
            emit(x, y);
            emit(x + width, y + height);
            emit(x, y + height);
            return;
        }
        // Perimeter points in clockwise order, one arc per rounded corner, then a fan from the centre.
        utility::vessel::vector<std::pair<float, float>> perimeter;
        auto const arc = [&](float cx, float cy, float r, float from, float to) {
            auto const steps = static_cast<int>(std::ceil(r * scale * 0.5F));
            auto const count = steps < 2 ? 2 : (steps > 16 ? 16 : steps);
            for (int step = 0; step <= count; ++step) {
                auto const angle = from + (to - from) * static_cast<float>(step) / static_cast<float>(count);
                perimeter.push_back({cx + std::cos(angle) * r, cy + std::sin(angle) * r});
            }
        };
        constexpr float pi = 3.14159265358979323846F;
        if (radius[0] > 0.0F) {
            arc(x + radius[0], y + radius[0], radius[0], pi, pi * 1.5F);
        } else {
            perimeter.push_back({x, y});
        }
        if (radius[1] > 0.0F) {
            arc(x + width - radius[1], y + radius[1], radius[1], pi * 1.5F, pi * 2.0F);
        } else {
            perimeter.push_back({x + width, y});
        }
        if (radius[2] > 0.0F) {
            arc(x + width - radius[2], y + height - radius[2], radius[2], 0.0F, pi * 0.5F);
        } else {
            perimeter.push_back({x + width, y + height});
        }
        if (radius[3] > 0.0F) {
            arc(x + radius[3], y + height - radius[3], radius[3], pi * 0.5F, pi);
        } else {
            perimeter.push_back({x, y + height});
        }
        auto const centre_x = x + width * 0.5F;
        auto const centre_y = y + height * 0.5F;
        for (std::size_t i = 0; i < perimeter.size(); ++i) {
            auto const& current = perimeter[i];
            auto const& next = perimeter[(i + 1) % perimeter.size()];
            emit(centre_x, centre_y);
            emit(current.first, current.second);
            emit(next.first, next.second);
        }
    }

    void ui_overlay::append_text(utility::vessel::vector<vertex>& out, scripting::editor_ui_text const& text) {
        if (text.length == 0 || this->blob.empty()) {
            return;
        }
        auto const scale = this->frame_scale;
        // Glyphs are rasterized at the device density so text stays sharp; dividing their metrics by
        // the scale keeps the pen in logical units, and the quads are multiplied at emission.
        auto const inv = 1.0F / scale;
        auto const pixel_size = static_cast<std::int32_t>(std::lround(text.size * scale));
        if (pixel_size <= 0) {
            return;
        }
        auto* face = static_cast<FT_Face>(this->font_face);
        if (face == nullptr) {
            return;
        }
        if (!this->ensure_pixel_size(face, 0U, pixel_size)) {
            return;
        }
        // y is the top of the line box, as reported by measure_text, so the baseline sits one ascender below.
        float pen_x = text.x;
        float const baseline = text.y + static_cast<float>(face->size->metrics.ascender) / 64.0F * inv;
        auto const begin = static_cast<std::size_t>(text.offset);
        auto const end = begin + static_cast<std::size_t>(text.length);
        if (end > this->blob.size()) {
            return;
        }
        std::string_view const run(this->blob.data() + begin, static_cast<std::size_t>(text.length));
        utility::vessel::vector<shaped_glyph> shaped;
        this->shape_runs(run, pixel_size, shaped);
        for (auto const& glyph : shaped) {
            auto const* raster = this->glyph_for(glyph.font_index, glyph.glyph, pixel_size);
            if (raster == nullptr) {
                // A glyph the atlas cannot hold still advances the pen, otherwise the rest of the run
                // would pile up on the same spot.
                pen_x += glyph.advance * inv;
                continue;
            }
            if (raster->x1 > raster->x0 && raster->y1 > raster->y0) {
                float const pen = pen_x + glyph.x_offset * inv;
                float const base = baseline + glyph.y_offset * inv;
                float const left = (pen + raster->x0 * inv) * scale;
                float const top = (base + raster->y0 * inv) * scale;
                float const right = (pen + raster->x1 * inv) * scale;
                float const bottom = (base + raster->y1 * inv) * scale;
                out.push_back({left, top, raster->u0, raster->v0, text.color});
                out.push_back({right, top, raster->u1, raster->v0, text.color});
                out.push_back({right, bottom, raster->u1, raster->v1, text.color});
                out.push_back({left, top, raster->u0, raster->v0, text.color});
                out.push_back({right, bottom, raster->u1, raster->v1, text.color});
                out.push_back({left, bottom, raster->u0, raster->v1, text.color});
            }
            pen_x += glyph.advance * inv;
        }
    }

    bool ui_overlay::prepare(std::uint32_t width, std::uint32_t height, rhi::image_format format, std::uint32_t slot,
                             float pixel_ratio) {
        if (slot >= this->frames.size()) {
            return false;
        }
        this->current_slot = slot;
        // The frontend lays out in logical pixels; a headless window reports no ratio and uses 1.
        float const ratio = pixel_ratio > 0.0F ? pixel_ratio : 1.0F;
        this->display_scale = ratio;
        this->viewport_width = static_cast<float>(width) / ratio;
        this->viewport_height = static_cast<float>(height) / ratio;
        auto& frame = this->frames[slot];
        if (width == 0 || height == 0) {
            frame.batches.clear();
            return true;
        }
        if (!this->update_atlas_resources()) {
            return false;
        }

        rhi::unique_object<rhi::pipeline> new_pipeline;
        if (!this->pipeline || this->format != format) {
            rhi::blend_mode const blend = rhi::blend_mode::alpha;
            rhi::resource_group_binding const layout[] = {
                {0, rhi::resource_kind::sampled_image, 8, 2},
                {1, rhi::resource_kind::sampler, 12, 2},
            };
            rhi::pipeline_desc desc{};
            desc.color_formats = std::span(&format, 1);
            desc.vertex_code = std::as_bytes(std::span(this->vertex_code));
            desc.fragment_code = std::as_bytes(std::span(this->fragment_code));
            desc.depth_test = false;
            desc.blend_modes = std::span(&blend, 1);
            desc.debug_name = "editor ui";
            desc.resource_group = layout;
            new_pipeline = this->core.resources->create_pipeline(desc);
            if (!new_pipeline) {
                return false;
            }
        }

        utility::vessel::vector<vertex> expanded;
        utility::vessel::vector<batch> new_batches;
        float const surface_width = static_cast<float>(width);
        float const surface_height = static_cast<float>(height);
        for (auto const& draw : this->draws) {
            if (draw.rect_first > this->rects.size() || draw.rect_count > this->rects.size() - draw.rect_first ||
                draw.text_first > this->texts.size() || draw.text_count > this->texts.size() - draw.text_first) {
                return false;
            }
            if (draw.rect_count == 0 && draw.text_count == 0) {
                continue;
            }
            auto const first_vertex = static_cast<std::uint32_t>(expanded.size());
            for (std::uint32_t i = 0; i < draw.rect_count; ++i) {
                auto const& rect = this->rects[draw.rect_first + i];
                if ((rect.flags & 1U) != 0U) {
                    this->append_rounded_rect(expanded, rect, rect.fill, 0.0F);
                }
                if ((rect.flags & 2U) != 0U && rect.border_width > 0.0F) {
                    // Border as an outer shape in the border colour with the fill inset: correct for any
                    // radius without a per-pixel distance test, and the inset ring reads as one pixel at
                    // 100% because the widths come from the caller's already-rounded layout.
                    this->append_rounded_rect(expanded, rect, rect.border, 0.0F);
                    if ((rect.flags & 1U) != 0U) {
                        this->append_rounded_rect(expanded, rect, rect.fill, rect.border_width);
                    }
                }
            }
            for (std::uint32_t i = 0; i < draw.text_count; ++i) {
                this->append_text(expanded, this->texts[draw.text_first + i]);
            }
            auto const vertex_count = static_cast<std::uint32_t>(expanded.size()) - first_vertex;
            if (vertex_count == 0) {
                continue;
            }
            float const left = std::floor(draw.clip.x * this->frame_scale);
            float const top = std::floor(draw.clip.y * this->frame_scale);
            float const right = std::ceil((draw.clip.x + draw.clip.width) * this->frame_scale);
            float const bottom = std::ceil((draw.clip.y + draw.clip.height) * this->frame_scale);
            auto const clip_left = left < 0.0F ? 0.0F : (left > surface_width ? surface_width : left);
            auto const clip_top = top < 0.0F ? 0.0F : (top > surface_height ? surface_height : top);
            auto const clip_right = right > surface_width ? surface_width : (right < 0.0F ? 0.0F : right);
            auto const clip_bottom = bottom > surface_height ? surface_height : (bottom < 0.0F ? 0.0F : bottom);
            if (clip_right <= clip_left || clip_bottom <= clip_top) {
                continue;
            }
            new_batches.push_back({
                .clip = {static_cast<std::int32_t>(clip_left), static_cast<std::int32_t>(clip_top),
                         static_cast<std::uint32_t>(clip_right - clip_left),
                         static_cast<std::uint32_t>(clip_bottom - clip_top)},
                .first_vertex = first_vertex,
                .vertex_count = vertex_count,
            });
        }

        rhi::unique_object<rhi::buffer> new_vertices;
        std::uint64_t address = 0;
        if (!expanded.empty()) {
            auto const bytes = std::as_bytes(std::span(expanded));
            new_vertices = this->core.resources->create_buffer({
                .size = bytes.size(),
                .usage = rhi::buffer_usage::storage_coherent,
                .flags = rhi::to_bits(rhi::buffer_flag::device_address),
                .initial_bytes = bytes,
            });
            auto const* addresses = rhi::query_extension<rhi::device_address>(this->core);
            if (!new_vertices || !addresses || !(address = addresses->buffer_address(*new_vertices, 0))) {
                return false;
            }
        }
        if (new_pipeline && this->pipeline) {
            this->core.wait_idle();
        }
        if (new_pipeline) {
            this->pipeline = std::move(new_pipeline);
        }
        frame.vertices = std::move(new_vertices);
        frame.batches = std::move(new_batches);
        frame.vertex_address = address;
        this->format = format;
        this->width = width;
        this->height = height;
        return true;
    }

    rhi::mergeable_command_buffer_ref ui_overlay::record(void* context, rhi::command_buffer_begin_info const& begin) {
        auto const& overlay = *static_cast<ui_overlay*>(context);
        auto const& frame = overlay.frames[overlay.current_slot];
        if (frame.batches.empty()) {
            return {};
        }
        auto segment = overlay.core.commands->make_mergeable_command_buffer({});
        if (!segment || segment->begin_recording(begin) != rhi::error::ok) {
            utility::error("editor ui: could not begin overlay recording");
            return {};
        }
        auto const width = static_cast<float>(overlay.width);
        auto const height = static_cast<float>(overlay.height);
        auto const srgb = overlay.format == rhi::image_format::rgba8_srgb || overlay.format == rhi::image_format::bgra8_srgb;
        bool recorded = segment->bind_pipeline(*overlay.pipeline) == rhi::error::ok;
        if (recorded) {
            segment->set_viewport({.width = width, .height = height});
            segment->set_cull_mode(rhi::cull_mode::none);
            segment->set_depth_write(false);
            for (auto const& batch : frame.batches) {
                segment->set_scissor(batch.clip);
                auto const address = frame.vertex_address + sizeof(vertex) * batch.first_vertex;
                draw_parameters const parameters{0.0F, 0.0F, width, height, srgb ? 1u : 0u};
                if (segment->load_resource_group(overlay.atlas_group) != rhi::error::ok ||
                    segment->push_constants(0, std::as_bytes(std::span(&address, 1))) != rhi::error::ok ||
                    segment->push_constants(16, std::as_bytes(std::span(&parameters, 1))) != rhi::error::ok) {
                    recorded = false;
                    break;
                }
                segment->draw(batch.vertex_count, 1, 0, 0);
            }
        }
        if (segment->end_recording() != rhi::error::ok || !recorded) {
            utility::error("editor ui: could not record overlay draws");
            return {};
        }
        return segment;
    }
} // namespace deren::ui
