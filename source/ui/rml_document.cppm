// module version: 0.2.0
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
    // ABI 42: what the forwarded mouse input did, kept separately from `ui_event` because these are
    // not RmlUi dispatches - a drag keeps reporting moves after the pointer leaves the element it
    // started on, and no click is invented. Coordinates and every rect query are in
    // density-independent pixels, the unit documents author in.
    enum class pointer_type { down,
                              move,
                              up };
    struct pointer_event {
        pointer_type type = pointer_type::move;
        // The nearest ancestor with an id, so a hit on a splitter's child still names the splitter.
        deren::utility::vessel::string id;
        float x = 0.0F;
        float y = 0.0F;
        int button = 0;         // The forwarded input_event::button; 0 for moves.
        unsigned modifiers = 0; // The modifiers the window reported.
    };
    enum class style_status { success,
                              missing_element,
                              rejected };
    struct vertex {
        float x, y, u, v;
        std::uint8_t r, g, b, a; // Premultiplied RGBA.
    };

    struct texture_view {
        std::uint32_t width = 0, height = 0;
        std::span<std::byte const> pixels; // Premultiplied RGBA8, or empty for untextured draws.
        // A draw whose texture is the scene itself (`deren://scene`): the overlay samples the engine's
        // render-chain image instead of an atlas this document owns.
        bool scene_color = false;
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
        bool set_value(std::string_view id, std::string_view value);
        bool set_visible(std::string_view id, bool visible);
        deren::utility::vessel::string text(std::string_view id) const;
        deren::utility::vessel::string value(std::string_view id) const;
        deren::utility::vessel::vector<ui_event> take_events();
        // ABI 42: the pointer events forwarded since the previous call, in arrival order. At most 256
        // are retained; a session that never polls loses the ones beyond that rather than growing the
        // queue without bound. open() and close() start empty.
        deren::utility::vessel::vector<pointer_event> take_pointer_events();
        // ABI 42: the element's border box in density-independent pixels. An empty id reports the
        // document root, i.e. the whole viewport, which is how a managed UI derives its proportions.
        // The answer describes the last layout pass, so a style written through set_style is visible
        // here only after the next paint(), because RmlUi runs layout there.
        bool element_rect(std::string_view id, float& x, float& y, float& width, float& height) const;
        // ABI 42: one CSS declaration through the parser an inline style uses, so `width` with `360dp`
        // and any colour property work. missing_element means the id is unknown, rejected means the
        // parser refused the declaration. Nothing here raises a change event.
        style_status set_style(std::string_view id, std::string_view property, std::string_view value);

        // Draws are in document paint order. Views remain valid until the next paint, open or close.
        // `width` and `height` are framebuffer pixels; the document's pixel ratio (default 1) turns them
        // into density-independent document pixels.
        std::span<draw const> paint(std::uint32_t width, std::uint32_t height);

        // Framebuffer pixels per density-independent pixel, i.e. the window's content scale. A value
        // <= 0 (or a non-finite one) resets to 1. Set it before paint(); it is kept until changed again.
        void set_density_independent_pixel_ratio(float ratio) noexcept;

    private:
        friend struct rml_render_interface;
        friend struct rml_event_listener;
        deren::utility::vessel::vector<ui_event> events;
        deren::utility::vessel::vector<pointer_event> pointer_events;
        void push_pointer(pointer_type type, int x, int y, int button, unsigned modifiers, std::string_view id);

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
        float density_independent_pixel_ratio = 1.0F;
        rectangle scissor;
        std::uint32_t pressed_buttons = 0;
        bool rendering = false;
        bool suppress_events = false;
        bool scissor_enabled = false;
    };
} // namespace deren::ui
