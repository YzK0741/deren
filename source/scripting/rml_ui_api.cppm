// module version: 0.1.0
// Optional retained-document UI contract. No RmlUi types or borrowed string results cross this table.
export module deren.scripting.rml_ui_api;
export import deren.vstd;
export import deren.scripting.api_types;

export namespace deren::scripting {
    struct rml_ui_event {
        std::uint32_t type = 0; // 0: empty, 1: click, 2: change.
        std::uint32_t id_length = 0;
        std::uint32_t value_length = 0;
    };
    struct rml_ui_api {
        script_status (*open)(utf8_view markup, utf8_view font, utf8_view fallback_font, std::uint64_t* document) = nullptr;
        script_status (*close)(std::uint64_t document) = nullptr;
        script_status (*set_text)(std::uint64_t document, utf8_view element, utf8_view text) = nullptr;
        script_status (*set_value)(std::uint64_t document, utf8_view element, utf8_view value) = nullptr;
        script_status (*set_visible)(std::uint64_t document, utf8_view element, std::uint8_t visible) = nullptr;
        // Null buffer peeks. Non-null buffer copies id then value, without terminators, and consumes
        // exactly one event. Insufficient capacity leaves the event queued and the buffer untouched.
        script_status (*poll_event)(std::uint64_t document, rml_ui_event* event, char* buffer, std::uint32_t capacity) = nullptr;
    };
    static_assert(sizeof(rml_ui_api) == 6 * sizeof(void*));
    static_assert(sizeof(rml_ui_event) == 12);
} // namespace deren::scripting
