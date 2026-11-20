#include "vk_test.h"

import deren.vstd;
import deren.ui.rml_document;
import deren.ui.rml_scripting;

int main() {
    using namespace deren::scripting;
    using namespace deren::ui;
    auto view = [](std::string const& value) -> utf8_view {
        return {value.data(), static_cast<std::uint32_t>(value.size())};
    };
    std::filesystem::path const demo = VR_RMLUI_DEMO_DIR;
    auto markup = (demo / "hud.rml").generic_string();
    auto font = (demo / "LatoLatin-Regular.ttf").generic_string();
    rml_document document;
    auto const& api = rml_scripting::table();
    std::uint64_t stale = 0;
    {
        rml_scripting host(document);
        std::uint64_t id = 0;
        CHECK(api.open(view(markup), view(font), {}, &id) == script_status::success);
        CHECK(id != 0);
        stale = id;
        std::uint64_t duplicate = 99;
        CHECK(api.open(view(markup), view(font), {}, &duplicate) == script_status::scene_exists);
        CHECK(duplicate == 0);
        CHECK(api.set_text(id, {"status", 6}, {"<b>Copied & safe</b>", 20}) == script_status::success);
        CHECK(document.text("status") == "<b>Copied & safe</b>");
        CHECK(api.set_text(id, {"missing", 7}, {}) == script_status::invalid_node);
        CHECK(api.set_value(id, {"name", 4}, {"hello", 5}) == script_status::success);
        CHECK(document.value("name") == "hello");
        CHECK(api.set_visible(id, {"panel", 5}, 0) == script_status::success);
        document.paint(640, 480);
        document.input({.type = input_type::move, .x = 60, .y = 190});
        CHECK(!document.wants_mouse());
        CHECK(api.set_visible(id, {"panel", 5}, 1) == script_status::success);
        document.paint(640, 480);
        document.input({.type = input_type::move, .x = 60, .y = 190});
        CHECK(document.wants_mouse());
        document.input({.type = input_type::button, .button = 0, .down = true});
        document.input({.type = input_type::button, .button = 0, .down = false});
        rml_ui_event event{};
        CHECK(api.poll_event(id, &event, nullptr, 0) == script_status::success);
        CHECK(event.type == 1);
        CHECK(event.id_length == 5);
        char small[2] = {'x', 'y'};
        CHECK(api.poll_event(id, &event, small, 1) == script_status::invalid_argument);
        CHECK(small[0] == 'x' && small[1] == 'y');
        char payload[64]{};
        CHECK(api.poll_event(id, &event, payload, sizeof(payload)) == script_status::success);
        CHECK(std::string_view(payload, event.id_length) == "apply");
        // ABI 42: the forwarded mouse input follows the DOM events in arrival order. The first move
        // happened while the panel was hidden, so it names no element.
        CHECK(api.poll_event(id, &event, payload, sizeof(payload)) == script_status::success);
        CHECK(event.type == 4 && event.id_length == 0);
        CHECK(api.poll_event(id, &event, payload, sizeof(payload)) == script_status::success);
        CHECK(event.type == 4 && std::string_view(payload, event.id_length) == "apply");
        CHECK(event.x == 60.0F && event.y == 190.0F && event.button == 0 && event.modifiers == 0);
        CHECK(api.poll_event(id, &event, payload, sizeof(payload)) == script_status::success);
        CHECK(event.type == 3 && std::string_view(payload, event.id_length) == "apply");
        CHECK(api.poll_event(id, &event, payload, sizeof(payload)) == script_status::success);
        CHECK(event.type == 5 && std::string_view(payload, event.id_length) == "apply");
        CHECK(api.poll_event(id, &event, payload, sizeof(payload)) == script_status::success);
        CHECK(event.type == 0);
        script_status worker_result = script_status::success;
        std::thread worker([&] { worker_result = api.set_text(id, {"status", 6}, {}); });
        worker.join();
        CHECK(worker_result == script_status::module_unavailable);
        CHECK(api.close(id) == script_status::success);
        CHECK(api.set_text(id, {"status", 6}, {}) == script_status::invalid_node);
        CHECK(api.open(view(markup), view(font), {}, &id) == script_status::success);
        CHECK(id != stale);
        CHECK(api.close(stale) == script_status::invalid_node);
        stale = id;
    }
    CHECK(document.paint(640, 480).empty());
    CHECK(api.close(stale) == script_status::module_unavailable);
    return deren::vk_test::finish("test_rml_scripting");
}
