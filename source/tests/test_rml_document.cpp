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
    // Only a real widget under the pointer takes the mouse: empty space over the document is not an
    // interaction, so a HUD covering the screen cannot swallow the game's mouse.
    document.input({.type = input_type::move, .x = 600, .y = 300});
    CHECK(!document.wants_mouse());
    // Geometry authored in `dp` follows the content scale: the same 640x480 framebuffer at ratio 2
    // lays the 320dp panel out at 640 px instead of 320 px.
    document.set_density_independent_pixel_ratio(2.0F);
    float widest = 0;
    for (auto const& draw : document.paint(640, 480)) {
        for (auto const& vertex : draw.vertices) {
            widest = vertex.x > widest ? vertex.x : widest;
        }
    }
    CHECK(widest > 500.0F);
    document.set_density_independent_pixel_ratio(1.0F);
    document.paint(640, 480);
    document.input({.type = input_type::move, .x = 60, .y = 190});
    document.input({.type = input_type::button, .button = 0, .down = true});
    document.input({.type = input_type::button, .button = 0, .down = false});
    bool clicked = false;
    for (auto const& event : document.take_events()) {
        clicked |= event.type == "click" && event.id == "apply";
    }
    CHECK(clicked);
    // A click focuses the button and RmlUi keeps it focused for Tab navigation, but a focused button
    // must not take the keyboard from the game: only text entry does.
    CHECK(!document.wants_keyboard());
    CHECK(!document.input({.type = input_type::key, .key = 'W', .down = true}));
    CHECK(!document.input({.type = input_type::character, .codepoint = 'x'}));
    CHECK(document.value("name").find("x") == std::string::npos);
    // Hiding an unfocused element must not synthesize a focus-loss: a focus-loss clears the hover chain,
    // and the editor's outliner hides its unused rows every frame, which used to wipe the hovered element
    // on the very frame the press was resolved, so no click could ever land on a button.
    document.input({.type = input_type::move, .x = 60, .y = 190});
    CHECK(document.wants_mouse());
    for (int index = 0; index < 4; ++index) {
        CHECK(document.set_visible("status", false));
        CHECK(document.set_visible("status", true));
    }
    CHECK(document.wants_mouse());
    document.input({.type = input_type::button, .button = 0, .down = true});
    document.input({.type = input_type::button, .button = 0, .down = false});
    bool clicked_after_hides = false;
    for (auto const& event : document.take_events()) {
        clicked_after_hides |= event.type == "click" && event.id == "apply";
    }
    CHECK(clicked_after_hides);
    document.input({.type = input_type::focus_lost});
    CHECK(!document.wants_keyboard());
    document.close();
    CHECK(document.paint(640, 480).empty());
    // A root with an explicit size makes `right:`/`bottom:` anchors resolve against the window: an
    // unsized root leaves the containing block zero wide and the layer ends up off the left edge.
    CHECK(document.open(demo / "anchored.rml", demo / "LatoLatin-Regular.ttf"));
    bool inside = true;
    for (auto const& draw : document.paint(640, 480)) {
        for (auto const& vertex : draw.vertices) {
            float const x = vertex.x + static_cast<float>(draw.offset_x);
            float const y = vertex.y + static_cast<float>(draw.offset_y);
            inside = inside && x >= 0.0F && x <= 640.0F && y >= 0.0F && y <= 480.0F;
        }
    }
    CHECK(inside);
    CHECK(document.set_style("corner", "transform", "rotate(90deg)") == style_status::success);
    float min_x = 640.0F, min_y = 480.0F, max_x = 0.0F, max_y = 0.0F;
    for (auto const& draw : document.paint(640, 480)) {
        for (auto const& vertex : draw.vertices) {
            float const x = vertex.x + draw.offset_x;
            float const y = vertex.y + draw.offset_y;
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
        }
    }
    CHECK(std::abs(max_x - min_x - 60.0F) < 0.1F);
    CHECK(std::abs(max_y - min_y - 120.0F) < 0.1F);
    document.close();
    CHECK(second.open(demo / "hud.rml", demo / "LatoLatin-Regular.ttf"));
    second.close();
    // Exercise the editor's actual Rml rather than a copy of the styles.
    auto const editor = demo.parent_path().parent_path() / "editor" / "ui" / "editor.rml";
    CHECK(document.open(editor, demo / "LatoLatin-Regular.ttf"));
    CHECK(document.set_visible("menu_file_items", true));
    document.paint(1600, 900);
    float first_x = 0, first_y = 0, first_width = 0, first_height = 0;
    CHECK(document.element_rect("menu_file_new", first_x, first_y, first_width, first_height));
    float previous_y = first_y;
    for (auto const* id : {"menu_file_open", "menu_file_open_path", "menu_file_save",
                           "menu_file_save_as", "menu_file_project_settings"}) {
        float x = 0, y = 0, width = 0, height = 0;
        CHECK(document.element_rect(id, x, y, width, height));
        CHECK(std::abs(x - first_x) < 0.5f && std::abs(width - first_width) < 0.5f);
        CHECK(y >= previous_y + first_height - 0.5f);
        previous_y = y;
    }
    CHECK(document.set_visible("menu_file_items", false));
    CHECK(document.set_style("project_settings_prompt", "display", "flex") == style_status::success);
    CHECK(document.set_value("look_background", "0"));
    document.paint(1600, 900);
    float select_x = 0, select_y = 0, select_width = 0, select_height = 0;
    CHECK(document.element_rect("look_background", select_x, select_y, select_width, select_height));
    auto const click_at = [&document](int x, int y) {
        document.input({.type = input_type::move, .x = x, .y = y});
        document.input({.type = input_type::button, .button = 0, .down = true});
        document.input({.type = input_type::button, .button = 0, .down = false});
    };
    click_at(static_cast<int>(select_x + 20), static_cast<int>(select_y + select_height * 0.5f));
    float popup_top = 900.0f;
    for (auto const& draw : document.paint(1600, 900)) {
        for (auto const& vertex : draw.vertices) {
            float const x = vertex.x + draw.offset_x;
            float const y = vertex.y + draw.offset_y;
            if (vertex.r == 0x25 && vertex.g == 0x25 && vertex.b == 0x25 && vertex.a == 255 &&
                x >= select_x - 2 && x <= select_x + select_width + 2 && y >= select_y + select_height) {
                popup_top = std::min(popup_top, y);
            }
        }
    }
    CHECK(popup_top < 900.0f);
    // The center of the second 26dp option, measured from the painted popup.
    click_at(static_cast<int>(select_x + 20), static_cast<int>(popup_top + 40));
    CHECK(document.value("look_background") == "1");
    // A SELECT'S CHANGE EVENT MUST REACH THE QUEUE. RmlUi's widget stops the option click from
    // bubbling, so the change event is the only signal a panel gets that the user picked an option:
    // without it the value moves inside RmlUi while the managed editor never hears about it, which
    // is exactly how the environment dropdowns looked broken.
    bool chose_background = false;
    for (auto const& event : document.take_events()) {
        chose_background |= event.type == "change" && event.id == "look_background" && event.value == "1";
    }
    CHECK(chose_background);
    // REPLACING A SELECT'S OPTIONS MUST BE SILENT. Rebuilding the option list drops RmlUi's selection
    // and that used to queue a `change` event with an EMPTY value, so a panel that repainted its
    // dropdown from the document read its own repaint as "the field was cleared" and undid the edit -
    // the per-object environment row reset itself to the scene map the moment it was painted.
    CHECK(document.set_markup("look_background", "<option value=\"0\">Sky</option><option value=\"1\">Image</option>"));
    CHECK(document.set_value("look_background", "1"));
    document.paint(1600, 900);
    bool repaint_reported_change = false;
    for (auto const& event : document.take_events()) {
        repaint_reported_change |= event.type == "change" && event.id == "look_background";
    }
    CHECK(!repaint_reported_change);
    CHECK(document.value("look_background") == "1");
    // A SECOND MARKUP WRITE REPLACES A SELECT'S OPTIONS. RmlUi keeps them in the drop-down widget rather
    // than in the element tree, so parsing markup alone leaves the widget's first list in place and moves
    // the new one in beside it: the popup then lists every entry twice. The editor rewrites its
    // environment lanes' options on every scene change, so it hit exactly that.
    CHECK(document.set_markup("look_background", "<option value=\"lane-a\">Lane-A</option>"));
    document.paint(1600, 900);
    CHECK(document.option_count("look_background") == 1);
    CHECK(document.set_markup("look_background", "<option value=\"lane-a\">Lane-A</option>"));
    document.paint(1600, 900);
    CHECK(document.option_count("look_background") == 1);
    // A NON-SELECT ELEMENT HAS NO OPTIONS TO COUNT, and an unknown id counts nothing either: the accessor
    // only reports the drop-down widget's list, so it must not pretend a plain element had options.
    CHECK(document.option_count("details") == 0);
    CHECK(document.option_count("no_such_element") == 0);
    // A FAILED MARKUP WRITE MUST NOT LEAVE EVENTS SUPPRESSED. The value write below is one a panel makes
    // right after painting a row; if the failure path forgot to restore the flag, the next real user
    // change would be swallowed and the row would look dead.
    CHECK(!document.set_markup("no_such_element", "<option value=\"lane-a\">Lane-A</option>"));
    CHECK(document.set_value("look_background", "lane-a"));
    document.paint(1600, 900);
    CHECK(document.value("look_background") == "lane-a");
    document.close();

    return deren::vk_test::finish("test_rml_document");
}
