module;

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/StringUtilities.h>
#include <stb/stb_image.h>

module deren.ui.rml_document;

import deren.vstd;
import deren.utility;

namespace deren::ui {
    struct rml_render_interface final : Rml::RenderInterface {
        using compiled_geometry = rml_document::compiled_geometry;
        using generated_texture = rml_document::generated_texture;
        rml_document* document = nullptr;

        Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<Rml::Vertex const> vertices,
                                                    Rml::Span<int const> indices) override {
            auto geometry = std::make_unique<compiled_geometry>();
            geometry->vertices.reserve(vertices.size());
            for (auto const& v : vertices) {
                geometry->vertices.push_back({v.position.x, v.position.y, v.tex_coord.x, v.tex_coord.y,
                                              v.colour.red, v.colour.green, v.colour.blue, v.colour.alpha});
            }
            geometry->indices.reserve(indices.size());
            for (int index : indices) {
                geometry->indices.push_back(static_cast<std::uint32_t>(index));
            }
            return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry.release());
        }

        void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f offset,
                            Rml::TextureHandle texture_handle) override {
            auto const& geometry = *reinterpret_cast<compiled_geometry const*>(handle);
            auto const viewport = Rml::Rectanglei::FromSize({this->document->viewport_width, this->document->viewport_height});
            auto const& scissor = this->document->scissor;
            auto const clip = this->document->scissor_enabled
                                  ? viewport.IntersectIfValid(Rml::Rectanglei::FromPositionSize({scissor.x, scissor.y}, {scissor.width, scissor.height}))
                                  : viewport;
            if (geometry.indices.empty() || clip.Width() <= 0 || clip.Height() <= 0) {
                return;
            }

            texture_view texture;
            if (auto const* data = reinterpret_cast<generated_texture const*>(texture_handle)) {
                texture = {data->width, data->height, data->pixels};
            }
            this->document->draws.push_back({geometry.vertices, geometry.indices, texture, offset.x, offset.y, clip.Left(),
                                             clip.Top(), clip.Width(), clip.Height()});
        }

        void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override {
            std::unique_ptr<compiled_geometry> geometry(reinterpret_cast<compiled_geometry*>(handle));
            if (this->document->rendering || !this->document->draws.empty()) {
                this->document->retired_geometry.push_back(std::move(geometry));
            }
        }

        Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, Rml::String const& source) override {
            auto bytes = utility::read_binary_to_vector(std::filesystem::path(source), 64u * 1024u * 1024u);
            if (!bytes) {
                return 0;
            }
            int width = 0, height = 0, channels = 0;
            if (!stbi_info_from_memory(bytes->data(), static_cast<int>(bytes->size()), &width, &height, &channels) ||
                width <= 0 || height <= 0 || static_cast<std::uint64_t>(width) * height > 64u * 1024u * 1024u) {
                return 0;
            }
            auto* pixels = stbi_load_from_memory(bytes->data(), static_cast<int>(bytes->size()), &width, &height, &channels, 4);
            if (!pixels) {
                return 0;
            }
            std::size_t const size = static_cast<std::size_t>(width) * height * 4;
            for (std::size_t i = 0; i < size; i += 4) {
                for (unsigned c = 0; c < 3; ++c) {
                    pixels[i + c] = static_cast<unsigned char>((pixels[i + c] * pixels[i + 3] + 127) / 255);
                }
            }
            auto handle = this->GenerateTexture({pixels, size}, {width, height});
            stbi_image_free(pixels);
            if (handle) {
                dimensions = {width, height};
            }
            return handle;
        }

        Rml::TextureHandle GenerateTexture(Rml::Span<Rml::byte const> source, Rml::Vector2i dimensions) override {
            if (dimensions.x <= 0 || dimensions.y <= 0 ||
                static_cast<std::uint64_t>(dimensions.x) * dimensions.y * 4 != source.size()) {
                return 0;
            }
            auto texture = std::make_unique<generated_texture>();
            texture->width = static_cast<std::uint32_t>(dimensions.x);
            texture->height = static_cast<std::uint32_t>(dimensions.y);
            auto const pixels = std::as_bytes(std::span(source.data(), source.size()));
            texture->pixels.assign(pixels.begin(), pixels.end());
            return reinterpret_cast<Rml::TextureHandle>(texture.release());
        }

        void ReleaseTexture(Rml::TextureHandle handle) override {
            std::unique_ptr<generated_texture> texture(reinterpret_cast<generated_texture*>(handle));
            if (this->document->rendering || !this->document->draws.empty()) {
                this->document->retired_textures.push_back(std::move(texture));
            }
        }

        void EnableScissorRegion(bool enable) override {
            this->document->scissor_enabled = enable;
        }

        void SetScissorRegion(Rml::Rectanglei region) override {
            this->document->scissor = {region.Left(), region.Top(), region.Width(), region.Height()};
        }
    };

    namespace {
        rml_render_interface render_interface;
        constexpr char context_name[] = "deren.ui";
    } // namespace

    struct rml_event_listener final : Rml::EventListener {
        void ProcessEvent(Rml::Event& event) override {
            auto* target = event.GetTargetElement();
            if (!target || target->GetId().empty() || !render_interface.document) {
                return;
            }
            auto* control = target->GetTagName() == "input" || target->GetTagName() == "textarea" || target->GetTagName() == "select"
                                ? static_cast<Rml::ElementFormControl*>(target)
                                : nullptr;
            ui_event result;
            result.type.assign(event.GetType().data(), event.GetType().size());
            result.id.assign(target->GetId().data(), target->GetId().size());
            if (control) {
                auto value = control->GetValue();
                result.value.assign(value.data(), value.size());
            }
            render_interface.document->events.push_back(std::move(result));
        }
    };
    namespace {
        rml_event_listener event_listener;
    }

    rml_document::~rml_document() {
        this->close();
    }

    bool rml_document::open(std::filesystem::path const& markup, std::filesystem::path const& font,
                            std::filesystem::path const& fallback_font) {
        this->close();
        if (render_interface.document) {
            return false;
        }

        Rml::SetRenderInterface(&render_interface);
        if (!Rml::Initialise()) {
            Rml::SetRenderInterface(nullptr);
            return false;
        }
        render_interface.document = this;

        if (!Rml::LoadFontFace(font.generic_string()) ||
            (!fallback_font.empty() && !Rml::LoadFontFace(fallback_font.generic_string(), true))) {
            this->close();
            return false;
        }

        auto* const context = Rml::CreateContext(context_name, {1, 1}, &render_interface);
        if (!context) {
            this->close();
            return false;
        }
        auto* const document = context->LoadDocument(markup.generic_string());
        if (!document) {
            this->close();
            return false;
        }
        context->AddEventListener("click", &event_listener);
        context->AddEventListener("change", &event_listener);
        document->Show();
        return true;
    }

    void rml_document::close() {
        if (render_interface.document != this) {
            return;
        }
        this->draws.clear();
        this->retired_geometry.clear();
        this->retired_textures.clear();
        // Shutdown releases the context's resources through the active render interface.
        Rml::Shutdown();
        render_interface.document = nullptr;
        Rml::SetRenderInterface(nullptr);
        this->events.clear();
        this->pressed_buttons = 0;
    }

    std::span<draw const> rml_document::paint(std::uint32_t width, std::uint32_t height) {
        if (render_interface.document != this) {
            return {};
        }
        this->draws.clear();
        this->retired_geometry.clear();
        this->retired_textures.clear();
        constexpr auto max_dimension = static_cast<std::uint32_t>(std::numeric_limits<int>::max());
        if (width == 0 || height == 0 || width > max_dimension || height > max_dimension) {
            return {};
        }

        Rml::Vector2i const dimensions(static_cast<int>(width), static_cast<int>(height));
        this->viewport_width = dimensions.x;
        this->viewport_height = dimensions.y;
        this->scissor_enabled = false;
        auto* const context = Rml::GetContext(context_name);
        context->SetDimensions(dimensions);
        if (!context->Update()) {
            return {};
        }

        this->rendering = true;
        bool const rendered = context->Render();
        this->rendering = false;
        if (!rendered) {
            return {};
        }
        return this->draws;
    }
    bool rml_document::set_text(std::string_view id, std::string_view text) {
        auto* context = render_interface.document == this ? Rml::GetContext(context_name) : nullptr;
        auto* element = context ? context->GetDocument(0)->GetElementById(Rml::String(id)) : nullptr;
        if (!element) {
            return false;
        }
        element->SetInnerRML(Rml::StringUtilities::EncodeRml(Rml::String(text)));
        return true;
    }
    utility::vessel::string rml_document::text(std::string_view id) const {
        auto* context = render_interface.document == this ? Rml::GetContext(context_name) : nullptr;
        auto* element = context ? context->GetDocument(0)->GetElementById(Rml::String(id)) : nullptr;
        utility::vessel::string result;
        auto append = [&](auto&& self, Rml::Element* node) -> void {
            if (node->GetTagName() == "#text") {
                auto const& text = static_cast<Rml::ElementText*>(node)->GetText();
                result.append(text.data(), text.size());
            } else {
                for (int i = 0; i < node->GetNumChildren(); ++i) {
                    self(self, node->GetChild(i));
                }
            }
        };
        if (element) {
            append(append, element);
        }
        return result;
    }
    utility::vessel::string rml_document::value(std::string_view id) const {
        auto* context = render_interface.document == this ? Rml::GetContext(context_name) : nullptr;
        auto* element = context ? context->GetDocument(0)->GetElementById(Rml::String(id)) : nullptr;
        if (!element || (element->GetTagName() != "input" && element->GetTagName() != "textarea" && element->GetTagName() != "select")) {
            return {};
        }
        auto const value = static_cast<Rml::ElementFormControl*>(element)->GetValue();
        return utility::vessel::string(value.data(), value.size());
    }
    utility::vessel::vector<ui_event> rml_document::take_events() {
        utility::vessel::vector<ui_event> result;
        result.swap(this->events);
        return result;
    }
    bool rml_document::wants_mouse() const {
        auto* context = render_interface.document == this ? Rml::GetContext(context_name) : nullptr;
        auto* hover = context ? context->GetHoverElement() : nullptr;
        return this->pressed_buttons != 0 || (hover && hover != context->GetRootElement() && hover->GetTagName() != "body");
    }
    bool rml_document::wants_keyboard() const {
        auto* context = render_interface.document == this ? Rml::GetContext(context_name) : nullptr;
        auto* focus = context ? context->GetFocusElement() : nullptr;
        if (!focus) {
            return false;
        }
        auto const& tag = focus->GetTagName();
        return tag == "input" || tag == "textarea" || tag == "select" || tag == "button";
    }
    bool rml_document::input(input_event const& event) {
        auto* context = render_interface.document == this ? Rml::GetContext(context_name) : nullptr;
        if (!context) {
            return false;
        }
        bool const mouse = this->wants_mouse();
        bool const keyboard = this->wants_keyboard();
        switch (event.type) {
        case input_type::move:
            context->ProcessMouseMove(event.x, event.y, event.modifiers);
            return this->wants_mouse();
        case input_type::button:
            if (event.button < 0 || event.button >= 8) {
                return mouse;
            }
            if (event.down) {
                if (mouse) {
                    this->pressed_buttons |= 1u << event.button;
                }
                context->ProcessMouseButtonDown(event.button, event.modifiers);
            } else {
                context->ProcessMouseButtonUp(event.button, event.modifiers);
                this->pressed_buttons &= ~(1u << event.button);
            }
            return mouse;
        case input_type::wheel:
            context->ProcessMouseWheel(Rml::Vector2f{0, -event.wheel}, event.modifiers);
            return mouse;
        case input_type::character:
            if (event.codepoint <= 0x10ffff && !(event.codepoint >= 0xd800 && event.codepoint <= 0xdfff)) {
                context->ProcessTextInput(static_cast<Rml::Character>(event.codepoint));
            }
            return keyboard;
        case input_type::key: {
            using namespace Rml::Input;
            auto key = KI_UNKNOWN;
            if (event.key >= 'A' && event.key <= 'Z') {
                key = static_cast<KeyIdentifier>(KI_A + event.key - 'A');
            } else if (event.key >= '0' && event.key <= '9') {
                key = static_cast<KeyIdentifier>(KI_0 + event.key - '0');
            } else {
                switch (static_cast<key_code>(event.key)) {
                case key_code::backspace:
                    key = KI_BACK;
                    break;
                case key_code::tab:
                    key = KI_TAB;
                    break;
                case key_code::enter:
                    key = KI_RETURN;
                    break;
                case key_code::escape:
                    key = KI_ESCAPE;
                    break;
                case key_code::left:
                    key = KI_LEFT;
                    break;
                case key_code::right:
                    key = KI_RIGHT;
                    break;
                case key_code::up:
                    key = KI_UP;
                    break;
                case key_code::down:
                    key = KI_DOWN;
                    break;
                case key_code::delete_key:
                    key = KI_DELETE;
                    break;
                case key_code::home:
                    key = KI_HOME;
                    break;
                case key_code::end:
                    key = KI_END;
                    break;
                case key_code::page_up:
                    key = KI_PRIOR;
                    break;
                case key_code::page_down:
                    key = KI_NEXT;
                    break;
                default:
                    break;
                }
            }
            if (event.key == ' ') {
                key = KI_SPACE;
            }
            if (event.down) {
                context->ProcessKeyDown(key, event.modifiers);
            } else {
                context->ProcessKeyUp(key, event.modifiers);
            }
            return keyboard;
        }
        case input_type::mouse_leave:
            context->ProcessMouseLeave();
            return this->has_mouse_capture();
        case input_type::focus_lost:
            this->pressed_buttons = 0;
            if (auto* focus = context->GetFocusElement()) {
                focus->Blur();
            }
            context->ProcessMouseLeave();
            for (int button = 0; button < 3; ++button) {
                context->ProcessMouseButtonUp(button, 0);
            }
            return false;
        }
        return false;
    }
} // namespace deren::ui
