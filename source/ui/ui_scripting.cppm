// module version: 0.1.0
// Main-thread bridge between the managed editor frontend and one ui_overlay. The table is installed
// once per process; a session that asks for the classic shell never sees it.
export module deren.ui.ui_scripting;

export import deren.vstd;
export import deren.scripting.editor_ui_api;
export import deren.ui.ui_overlay;

export namespace deren::ui {
    class ui_scripting {
    public:
        explicit ui_scripting(ui_overlay& overlay)
            : overlay(overlay)
            , owner(std::this_thread::get_id()) {
            if (!active) {
                active = this;
            }
        }
        ~ui_scripting() {
            if (active == this) {
                active = nullptr;
            }
        }
        ui_scripting(ui_scripting const&) = delete;
        ui_scripting& operator=(ui_scripting const&) = delete;
        static scripting::editor_ui_api const& table() {
            static scripting::editor_ui_api const value{submit, measure_text, query_viewport, shape_text,
                                                        set_ime_handler, set_ime_caret_rect, set_pointer_claim,
                                                        query_stats};
            return value;
        }
        // True while the pointer sits over an element the frontend owns. The window seam reads this
        // before it chains a pointer event to the camera, so a drag that started on a control never
        // orbits the preview as well. The frontend itself keeps receiving the event.
        static bool pointer_claimed() {
            return active && pointer_over_ui;
        }
        // The caret box the frontend last published, in logical pixels. The app forwards it to the
        // window's composition seam, which converts it to client and then screen pixels. False means
        // no field wants the candidate list, so the seam keeps its last position.
        static bool ime_caret_rect(float& x, float& y, float& width, float& height) {
            if (!active || !caret_published) {
                return false;
            }
            x = caret_x;
            y = caret_y;
            width = caret_width;
            height = caret_height;
            return true;
        }
        // Forwards one composition notification to the frontend that registered a handler. The app's
        // window seam calls this on the render thread; with no handler the event is dropped, which is
        // what a frontend without a focused text field wants.
        static bool ime_attached() {
            return active && handler;
        }
        static void dispatch_ime(std::uint32_t kind, std::string_view text, std::int32_t caret) {
            if (!active || !handler) {
                return;
            }
            scripting::editor_ui_ime_event event{};
            event.kind = kind;
            event.text = {.data = text.empty() ? nullptr : text.data(),
                          .length = static_cast<std::uint32_t>(text.size())};
            event.caret = caret;
            handler(handler_opaque, &event);
        }

    private:
        using status = scripting::script_status;
        inline static ui_scripting* active = nullptr;
        inline static scripting::editor_ui_ime_handler handler = nullptr;
        inline static void* handler_opaque = nullptr;
        inline static bool caret_published = false;
        inline static bool pointer_over_ui = false;
        inline static float caret_x = 0.0F;
        inline static float caret_y = 0.0F;
        inline static float caret_width = 0.0F;
        inline static float caret_height = 0.0F;
        ui_overlay& overlay;
        std::thread::id owner;

        static bool available() {
            return active && active->owner == std::this_thread::get_id();
        }
        static status submit(scripting::editor_ui_frame const* frame) {
            if (!frame) {
                return status::invalid_argument;
            }
            if (!available()) {
                return status::module_unavailable;
            }
            return active->overlay.submit(*frame) ? status::success : status::script_failure;
        }
        static status measure_text(scripting::utf8_view text, float size, float* width, float* height) {
            if (!available()) {
                return status::module_unavailable;
            }
            if (text.length != 0 && text.data == nullptr) {
                return status::invalid_argument;
            }
            auto const view = text.length == 0 ? std::string_view{}
                                               : std::string_view(text.data, static_cast<std::size_t>(text.length));
            float measured_width = 0.0F, measured_height = 0.0F;
            if (!active->overlay.measure_text(view, size, measured_width, measured_height)) {
                return status::invalid_argument;
            }
            if (width) {
                *width = measured_width;
            }
            if (height) {
                *height = measured_height;
            }
            return status::success;
        }
        static status query_viewport(float* width, float* height, float* scale) {
            if (!available()) {
                return status::module_unavailable;
            }
            float viewport_width = 0.0F, viewport_height = 0.0F, display_scale = 1.0F;
            active->overlay.query_viewport(viewport_width, viewport_height, display_scale);
            if (width) {
                *width = viewport_width;
            }
            if (height) {
                *height = viewport_height;
            }
            if (scale) {
                *scale = display_scale;
            }
            return status::success;
        }
        static status shape_text(scripting::utf8_view text, float size, scripting::editor_ui_glyph* glyphs, std::uint32_t capacity, std::uint32_t* count, float* width) {
            if (!available()) {
                return status::module_unavailable;
            }
            if (text.length != 0 && text.data == nullptr) {
                return status::invalid_argument;
            }
            auto const view = text.length == 0 ? std::string_view{}
                                               : std::string_view(text.data, static_cast<std::size_t>(text.length));
            std::uint32_t glyph_count = 0U;
            float shaped_width = 0.0F;
            if (!active->overlay.shape_text(view, size, glyphs, capacity, glyph_count, shaped_width)) {
                return status::invalid_argument;
            }
            if (count) {
                *count = glyph_count;
            }
            if (width) {
                *width = shaped_width;
            }
            return status::success;
        }
        static status set_ime_handler(scripting::editor_ui_ime_handler next, void* opaque) {
            if (!available()) {
                return status::module_unavailable;
            }
            handler = next;
            handler_opaque = next ? opaque : nullptr;
            return status::success;
        }
        static status set_ime_caret_rect(float x, float y, float width, float height) {
            if (!available()) {
                return status::module_unavailable;
            }
            caret_published = width > 0.0F && height > 0.0F;
            caret_x = x;
            caret_y = y;
            caret_width = width;
            caret_height = height;
            return status::success;
        }
        static status set_pointer_claim(std::uint8_t over_ui) {
            if (!available()) {
                return status::module_unavailable;
            }
            pointer_over_ui = over_ui != 0U;
            return status::success;
        }
        static status query_stats(scripting::editor_ui_stats* stats) {
            if (!stats) {
                return status::invalid_argument;
            }
            if (!available()) {
                return status::module_unavailable;
            }
            std::uint32_t side = 0U, glyphs = 0U, skipped = 0U;
            active->overlay.atlas_stats(side, glyphs, skipped);
            stats->atlas_size = side;
            stats->glyphs = glyphs;
            stats->skipped = skipped;
            stats->reserved = 0U;
            return status::success;
        }
    };
} // namespace deren::ui
