// module version: 0.2.0
// Main-thread document ownership and a copied-event bridge for one managed session.
export module deren.ui.rml_scripting;
export import deren.vstd;
export import deren.scripting.rml_ui_api;
export import deren.ui.rml_document;

export namespace deren::ui {
    class rml_scripting {
    public:
        explicit rml_scripting(rml_document& document)
            : document(document)
            , owner(std::this_thread::get_id()) {
            if (!active) {
                active = this;
            }
        }
        ~rml_scripting() {
            if (active == this) {
                if (this->id != 0) {
                    this->document.close();
                }
                active = nullptr;
            }
        }
        rml_scripting(rml_scripting const&) = delete;
        rml_scripting& operator=(rml_scripting const&) = delete;
        [[nodiscard]] bool has_managed_document() const noexcept {
            return this->id != 0;
        }
        static scripting::rml_ui_api const& table() {
            static scripting::rml_ui_api const value{open, close, set_text, set_value, set_visible, poll_event,
                                                     get_element_rect, set_element_style};
            return value;
        }

    private:
        using status = scripting::script_status;
        inline static rml_scripting* active = nullptr;
        inline static std::uint64_t next_id = 1;
        rml_document& document;
        std::thread::id owner;
        std::uint64_t id = 0;
        utility::vessel::vector<ui_event> pending;
        utility::vessel::vector<pointer_event> pending_pointer;
        std::size_t cursor = 0;
        std::size_t pointer_cursor = 0;

        static bool available() {
            return active && active->owner == std::this_thread::get_id();
        }
        static status validate(std::uint64_t id) {
            return !available() ? status::module_unavailable : id == 0 || id != active->id ? status::invalid_node
                                                                                           : status::success;
        }
        static bool valid(scripting::utf8_view value, bool empty = false) {
            return value.length <= 1024 * 1024 && (value.length == 0 ? empty : value.data != nullptr) &&
                   (value.length == 0 || std::string_view(value.data, value.length).find('\0') == std::string_view::npos);
        }
        static std::string_view view(scripting::utf8_view value) {
            return value.length == 0 ? std::string_view{} : std::string_view(value.data, value.length);
        }
        static std::filesystem::path path(scripting::utf8_view value) {
            auto bytes = view(value);
            return std::filesystem::path(std::u8string_view(reinterpret_cast<char8_t const*>(bytes.data()), bytes.size()));
        }
        static status open(scripting::utf8_view markup, scripting::utf8_view font,
                           scripting::utf8_view fallback, std::uint64_t* result) {
            if (!result) {
                return status::invalid_argument;
            }
            *result = 0;
            if (!available()) {
                return status::module_unavailable;
            }
            if (!valid(markup) || !valid(font) || !valid(fallback, true)) {
                return status::invalid_argument;
            }
            if (active->id != 0) {
                return status::scene_exists;
            }
            if (next_id == std::numeric_limits<std::uint64_t>::max()) {
                return status::script_failure;
            }
            if (!active->document.open(path(markup), path(font), fallback.length == 0 ? std::filesystem::path{} : path(fallback))) {
                return status::asset_not_found;
            }
            active->pending.clear();
            active->pending_pointer.clear();
            active->cursor = 0;
            active->pointer_cursor = 0;
            active->id = next_id++;
            *result = active->id;
            return status::success;
        }
        static status close(std::uint64_t id) {
            auto const checked = validate(id);
            if (checked != status::success) {
                return checked;
            }
            active->document.close();
            active->id = 0;
            active->pending.clear();
            active->pending_pointer.clear();
            active->cursor = 0;
            active->pointer_cursor = 0;
            return status::success;
        }
        static status set_text(std::uint64_t id, scripting::utf8_view element, scripting::utf8_view text) {
            auto const checked = validate(id);
            if (checked != status::success) {
                return checked;
            }
            if (!valid(element) || !valid(text, true)) {
                return status::invalid_argument;
            }
            return active->document.set_text(view(element), view(text)) ? status::success : status::invalid_node;
        }
        static status set_value(std::uint64_t id, scripting::utf8_view element, scripting::utf8_view value) {
            auto const checked = validate(id);
            if (checked != status::success) {
                return checked;
            }
            if (!valid(element) || !valid(value, true)) {
                return status::invalid_argument;
            }
            return active->document.set_value(view(element), view(value)) ? status::success : status::invalid_node;
        }
        static status set_visible(std::uint64_t id, scripting::utf8_view element, std::uint8_t visible) {
            auto const checked = validate(id);
            if (checked != status::success) {
                return checked;
            }
            if (!valid(element) || visible > 1) {
                return status::invalid_argument;
            }
            return active->document.set_visible(view(element), visible != 0) ? status::success : status::invalid_node;
        }
        static status poll_event(std::uint64_t id, scripting::rml_ui_event* result, char* buffer, std::uint32_t capacity) {
            if (!result) {
                return status::invalid_argument;
            }
            *result = {};
            auto const checked = validate(id);
            if (checked != status::success) {
                return checked;
            }
            // ABI 42: the DOM's click/change events come first, then the pointer events forwarded in
            // the same frame, so a session written before these events existed sees the same sequence.
            if (active->cursor == active->pending.size() && active->pointer_cursor == active->pending_pointer.size()) {
                active->pending = active->document.take_events();
                active->pending_pointer = active->document.take_pointer_events();
                active->cursor = 0;
                active->pointer_cursor = 0;
            }
            if (active->cursor != active->pending.size()) {
                auto const& event = active->pending[active->cursor];
                if (event.id.size() + event.value.size() > 1024 * 1024) {
                    ++active->cursor;
                    return status::invalid_argument;
                }
                *result = {event.type == "click" ? 1U : 2U, static_cast<std::uint32_t>(event.id.size()),
                           static_cast<std::uint32_t>(event.value.size())};
                if (!buffer) {
                    return capacity == 0 ? status::success : status::invalid_argument;
                }
                if (capacity < result->id_length + result->value_length) {
                    return status::invalid_argument;
                }
                std::copy(event.id.begin(), event.id.end(), buffer);
                std::copy(event.value.begin(), event.value.end(), buffer + result->id_length);
                ++active->cursor;
                return status::success;
            }
            if (active->pointer_cursor == active->pending_pointer.size()) {
                return status::success;
            }
            auto const& event = active->pending_pointer[active->pointer_cursor];
            if (event.id.size() > 1024 * 1024) {
                ++active->pointer_cursor;
                return status::invalid_argument;
            }
            auto const type = event.type == pointer_type::down ? 3U : event.type == pointer_type::move ? 4U
                                                                                                       : 5U;
            *result = {type, static_cast<std::uint32_t>(event.id.size()), 0, event.x, event.y, event.button, event.modifiers};
            if (!buffer) {
                return capacity == 0 ? status::success : status::invalid_argument;
            }
            if (capacity < result->id_length) {
                return status::invalid_argument;
            }
            std::copy(event.id.begin(), event.id.end(), buffer);
            ++active->pointer_cursor;
            return status::success;
        }
        static status get_element_rect(std::uint64_t id, scripting::utf8_view element, scripting::element_rect* result) {
            if (!result) {
                return status::invalid_argument;
            }
            *result = {};
            auto const checked = validate(id);
            if (checked != status::success) {
                return checked;
            }
            // An empty id asks for the document root, so viewport proportions need no element name.
            if (!valid(element, true)) {
                return status::invalid_argument;
            }
            float x = 0.0F, y = 0.0F, width = 0.0F, height = 0.0F;
            if (!active->document.element_rect(view(element), x, y, width, height)) {
                return status::invalid_node;
            }
            *result = {x, y, width, height};
            return status::success;
        }
        static status set_element_style(std::uint64_t id, scripting::utf8_view element, scripting::utf8_view property,
                                        scripting::utf8_view value) {
            auto const checked = validate(id);
            if (checked != status::success) {
                return checked;
            }
            if (!valid(element) || !valid(property) || !valid(value, true)) {
                return status::invalid_argument;
            }
            switch (active->document.set_style(view(element), view(property), view(value))) {
            case style_status::success:
                return status::success;
            case style_status::missing_element:
                return status::invalid_node;
            case style_status::rejected:
                return status::invalid_argument;
            }
            return status::script_failure;
        }
    };
} // namespace deren::ui
