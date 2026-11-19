// module version: 0.1.0
export module deren.ui.rml_document;

export import deren.vstd;
export import deren.utility.shared;

namespace deren::ui {
    struct rml_render_interface;
    struct rml_event_listener;
} // namespace deren::ui

export namespace deren::ui {
    enum class input_type { move,
                            button,
                            wheel,
                            key,
                            character,
                            focus_lost,
                            mouse_leave };
    enum class key_code : unsigned { unknown = 0,
                                     backspace = 8,
                                     tab = 9,
                                     enter = 13,
                                     escape = 27,
                                     left = 256,
                                     right,
                                     up,
                                     down,
                                     delete_key,
                                     home,
                                     end,
                                     page_up,
                                     page_down };
    struct input_event {
        input_type type = input_type::focus_lost;
        int x = 0, y = 0, button = 0, modifiers = 0;
        unsigned key = 0, codepoint = 0;
        float wheel = 0;
        bool down = false;
    };
    struct ui_event {
        deren::utility::vessel::string type, id, value;
    };
    struct vertex {
        float x, y, u, v;
        std::uint8_t r, g, b, a; // Premultiplied RGBA.
    };

    struct texture_view {
        std::uint32_t width = 0, height = 0;
        std::span<std::byte const> pixels; // Premultiplied RGBA8, or empty for untextured draws.
    };

    struct draw {
        std::span<vertex const> vertices;
        std::span<std::uint32_t const> indices;
        texture_view texture;
        float offset_x, offset_y;
        std::int32_t clip_x, clip_y, clip_width, clip_height;
    };

    // RmlUi state is process-wide. Use on the main thread with one open document at a time.
    class rml_document {
    public:
        rml_document() = default;
        ~rml_document();
        rml_document(rml_document const&) = delete;
        rml_document& operator=(rml_document const&) = delete;

        bool open(std::filesystem::path const& markup, std::filesystem::path const& font,
                  std::filesystem::path const& fallback_font = {});
        void close();
        bool input(input_event const& event);
        bool wants_mouse() const;
        bool has_mouse_capture() const noexcept {
            return this->pressed_buttons != 0;
        }
        bool wants_keyboard() const;
        bool set_text(std::string_view id, std::string_view text);
        deren::utility::vessel::string text(std::string_view id) const;
        deren::utility::vessel::string value(std::string_view id) const;
        deren::utility::vessel::vector<ui_event> take_events();

        // Draws are in document paint order. Views remain valid until the next paint, open or close.
        std::span<draw const> paint(std::uint32_t width, std::uint32_t height);

    private:
        friend struct rml_render_interface;
        friend struct rml_event_listener;
        deren::utility::vessel::vector<ui_event> events;

        struct compiled_geometry {
            deren::utility::vessel::vector<vertex> vertices;
            deren::utility::vessel::vector<std::uint32_t> indices;
        };

        struct generated_texture {
            std::uint32_t width, height;
            deren::utility::vessel::vector<std::byte> pixels;
        };

        struct rectangle {
            int x = 0, y = 0, width = 0, height = 0;
        };

        deren::utility::vessel::vector<draw> draws;
        // A resource released during Render() can still be referenced by this frame's draws.
        deren::utility::vessel::vector<std::unique_ptr<compiled_geometry>> retired_geometry;
        deren::utility::vessel::vector<std::unique_ptr<generated_texture>> retired_textures;
        int viewport_width = 0;
        int viewport_height = 0;
        rectangle scissor;
        std::uint32_t pressed_buttons = 0;
        bool rendering = false;
        bool scissor_enabled = false;
    };
} // namespace deren::ui
