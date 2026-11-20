// module version: 0.2.0
// Optional retained-document UI contract. No RmlUi types or borrowed string results cross this table.
export module deren.scripting.rml_ui_api;
export import deren.vstd;
export import deren.scripting.api_types;

export namespace deren::scripting {
    struct rml_ui_event {
        // ABI 42: 3-5 report what the forwarded mouse input did, not what RmlUi dispatched, so a
        // drag keeps reporting move events after the pointer leaves the element it started on.
        // 0: empty, 1: click, 2: change, 3: pointer_down, 4: pointer_move, 5: pointer_up.
        std::uint32_t type = 0;
        std::uint32_t id_length = 0;
        std::uint32_t value_length = 0; // Pointer events carry no value.
        // Pointer events only, in density-independent pixels - the unit the managed side authors in
        // and the unit element_rect reports. The host divides its framebuffer pixels by the pixel
        // ratio, so a 360dp panel reports the same numbers at any window scale.
        float x = 0.0F;
        float y = 0.0F;
        // Pointer events only: the window button index the host forwarded (0 left, 1 right, ...),
        // the same numbering as input_event::button, and the modifiers bit mask.
        std::int32_t button = 0;
        std::uint32_t modifiers = 0;
    };
    // ABI 42: a laid-out element's border box, in density-independent pixels. It describes the
    // last layout pass: a style written through set_element_style is visible here after the host's
    // next paint, because that is where RmlUi re-runs layout.
    struct element_rect {
        float x = 0.0F;
        float y = 0.0F;
        float width = 0.0F;
        float height = 0.0F;
    };
    // ABI 42: the last two entries are optional. A host built before them leaves them null and the
    // managed side reports module_unavailable for those calls only; everything above keeps working.
    struct rml_ui_api {
        script_status (*open)(utf8_view markup, utf8_view font, utf8_view fallback_font, std::uint64_t* document) = nullptr;
        script_status (*close)(std::uint64_t document) = nullptr;
        script_status (*set_text)(std::uint64_t document, utf8_view element, utf8_view text) = nullptr;
        script_status (*set_value)(std::uint64_t document, utf8_view element, utf8_view value) = nullptr;
        script_status (*set_visible)(std::uint64_t document, utf8_view element, std::uint8_t visible) = nullptr;
        // Null buffer peeks. Non-null buffer copies id then value, without terminators, and consumes
        // exactly one event. Insufficient capacity leaves the event queued and the buffer untouched.
        script_status (*poll_event)(std::uint64_t document, rml_ui_event* event, char* buffer, std::uint32_t capacity) = nullptr;
        // ABI 42: the element's border box. An empty element id reports the document root, i.e. the
        // viewport; null rect -> invalid_argument, unknown element -> invalid_node.
        script_status (*get_element_rect)(std::uint64_t document, utf8_view element, element_rect* rect) = nullptr;
        // ABI 42: writes one CSS declaration through RmlUi's property parser, the same path an inline
        // style takes; a malformed declaration is rejected with invalid_argument. Nothing here raises
        // a change event, so a drag that resizes a panel cannot feed managed callbacks back into itself.
        script_status (*set_element_style)(std::uint64_t document, utf8_view element, utf8_view property,
                                           utf8_view value) = nullptr;
    };
    static_assert(sizeof(rml_ui_api) == 8 * sizeof(void*));
    static_assert(sizeof(rml_ui_event) == 28);
    static_assert(sizeof(element_rect) == 16);
} // namespace deren::scripting
