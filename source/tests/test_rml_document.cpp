#include "vk_test.h"

#include <cmath>
import deren.vstd;
import deren.ui.rml_document;
int main() {
    using namespace deren::ui;
    std::filesystem::path const demo = VR_RMLUI_DEMO_DIR;
    rml_document document;
    CHECK(!document.open(demo / "missing.rml", demo / "LatoLatin-Regular.ttf"));
    CHECK(document.open(demo / "hud.rml", demo / "LatoLatin-Regular.ttf"));
    CHECK(!document.paint(640, 480).empty());
    CHECK(document.paint(0, 480).empty());
    CHECK(!document.paint(800, 600).empty());
    CHECK(document.set_text("status", "Updated"));
    CHECK(document.text("status") == "Updated");
    CHECK(!document.set_text("missing", "text"));
    CHECK(document.set_text("status", "<b>A & B</b>"));
    CHECK(document.text("status") == "<b>A & B</b>");
    auto const snapshot = document.paint(640, 480);
    std::size_t vertex_count = 0;
    for (auto const& draw : snapshot) {
        vertex_count += draw.vertices.size();
    }
    CHECK(document.set_text("status", "Updated again"));
    std::size_t retained_count = 0;
    for (auto const& draw : snapshot) {
        retained_count += draw.vertices.size();
        for (auto const& vertex : draw.vertices) {
            CHECK(std::isfinite(vertex.x) && std::isfinite(vertex.y));
        }
    }
    CHECK(retained_count == vertex_count);

    rml_document second;
    CHECK(!second.open(demo / "hud.rml", demo / "LatoLatin-Regular.ttf"));
    document.paint(640, 480);
    document.input({.type = input_type::move, .x = 50, .y = 140});
    CHECK(document.wants_mouse());
    document.input({.type = input_type::button, .button = 0, .down = true});
    document.input({.type = input_type::button, .button = 0, .down = false});
    document.input({.type = input_type::button, .button = 0, .down = true});
    CHECK(document.input({.type = input_type::move, .x = 500, .y = 400}));
    CHECK(document.wants_mouse());
    CHECK(document.input({.type = input_type::button, .button = 0, .down = false}));
    CHECK(!document.wants_mouse());
    CHECK(document.wants_keyboard());
    document.input({.type = input_type::character, .codepoint = 'A'});
    document.input({.type = input_type::character, .codepoint = 0x4e2d});
    CHECK(document.value("name").find("A") != std::string::npos);
    CHECK(document.value("name").find("\xe4\xb8\xad") != std::string::npos);
    document.paint(640, 480);
    document.input({.type = input_type::move, .x = 60, .y = 190});
    document.input({.type = input_type::button, .button = 0, .down = true});
    document.input({.type = input_type::button, .button = 0, .down = false});
    bool clicked = false;
    for (auto const& event : document.take_events()) {
        clicked |= event.type == "click" && event.id == "apply";
    }
    CHECK(clicked);
    document.input({.type = input_type::focus_lost});
    CHECK(!document.wants_keyboard());
    document.close();
    CHECK(document.paint(640, 480).empty());
    CHECK(second.open(demo / "hud.rml", demo / "LatoLatin-Regular.ttf"));
    second.close();
    return deren::vk_test::finish("test_rml_document");
}
