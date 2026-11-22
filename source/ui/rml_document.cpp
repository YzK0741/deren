module;

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <RmlUi/Core/Box.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
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
    namespace {
        // The one URI a document can name to ask the overlay for the 3D scene instead of a file texture,
        // and the sentinel handle LoadTexture hands out for it. Its low value cannot collide with
        // `generated_texture` pointers and lets ReleaseTexture skip a real free.
        constexpr std::string_view scene_color_uri = "deren://scene";
        Rml::TextureHandle const scene_color_texture = reinterpret_cast<Rml::TextureHandle>(std::uintptr_t{1});

        // RMLUI SPEAKS UTF-8 AND URL PATHS. generic_string() answers in the ANSI code page and a narrow
        // std::filesystem::path constructor reads that same code page, while a native Windows path
        // (backslashes plus a drive colon) is not a URL: RmlUi resolves relative references against the
        // document source URL, so handing it the native form makes every relative texture and font
        // reference fail. Both directions therefore go through the generic UTF-8 form.
        std::string utf8_text(std::filesystem::path const& path) {
            auto const text = path.generic_u8string();
            return std::string(text.begin(), text.end());
        }
        std::filesystem::path path_from_utf8(std::string_view text) {
            return std::filesystem::path(std::u8string(reinterpret_cast<char8_t const*>(text.data()), text.size()));
        }
    } // namespace

    struct rml_render_interface final : Rml::RenderInterface {
        using compiled_geometry = rml_document::compiled_geometry;
        using generated_texture = rml_document::generated_texture;
        rml_document* document = nullptr;
        std::optional<Rml::Matrix4f> transform;

        void SetTransform(Rml::Matrix4f const* value) override {
            if (value) {
                this->transform = *value;
            } else {
                this->transform.reset();
            }
        }

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
            if (texture_handle == scene_color_texture) {
                texture.scene_color = true;
            } else if (auto const* data = reinterpret_cast<generated_texture const*>(texture_handle)) {
                texture = {data->width, data->height, data->pixels};
            }
            auto const* vertices = &geometry.vertices;
            if (this->transform) {
                auto transformed = std::make_unique<compiled_geometry>();
                transformed->vertices.reserve(geometry.vertices.size());
                for (auto vertex : geometry.vertices) {
                    auto const position = *this->transform * Rml::Vector4f{vertex.x + offset.x, vertex.y + offset.y, 0.0F, 1.0F};
                    if (std::abs(position.w) < 1.0e-6F) {
                        return;
                    }
                    vertex.x = position.x / position.w;
                    vertex.y = position.y / position.w;
                    transformed->vertices.push_back(vertex);
                }
                vertices = &transformed->vertices;
                this->document->retired_geometry.push_back(std::move(transformed));
                offset = {};
            }
            this->document->draws.push_back({*vertices, geometry.indices, texture, offset.x, offset.y, clip.Left(),
                                             clip.Top(), clip.Width(), clip.Height()});
        }

        void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override {
            std::unique_ptr<compiled_geometry> geometry(reinterpret_cast<compiled_geometry*>(handle));
            if (this->document->rendering || !this->document->draws.empty()) {
                this->document->retired_geometry.push_back(std::move(geometry));
            }
        }

        Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, Rml::String const& source) override {
            // `deren://scene` is answered without touching the filesystem: the overlay resolves it to the
            // frame's render-chain image. The dimensions are the intrinsic size placeholder RmlUi wants;
            // a document sizes the viewport element in CSS, so this only matters if it does not.
            if (source == scene_color_uri) {
                this->document->scene_texture_loaded = true;
                dimensions = {1920, 1080};
                return scene_color_texture;
            }
            auto bytes = utility::read_binary_to_vector(path_from_utf8(source), 64u * 1024u * 1024u);
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
            if (handle == scene_color_texture) {
                return; // borrowed from the overlay, never owned here
            }
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
            if (render_interface.document->suppress_events) {
                return;
            }
            ui_event result;
            result.type.assign(event.GetType().data(), event.GetType().size());
            result.id.assign(target->GetId().data(), target->GetId().size());
            if (control) {
                auto value = control->GetValue();
                result.value.assign(value.data(), value.size());
            }
            // THE DOM QUEUE IS CAPPED TOO: a session that never polls must not grow it without bound.
            // The oldest event goes first so the newest interaction is the one that survives.
            constexpr std::size_t max_events = 256;
            auto& queue = render_interface.document->events;
            if (queue.size() >= max_events) {
                queue.erase(queue.begin());
            }
            queue.push_back(std::move(result));
        }
    };
    namespace {
        rml_event_listener event_listener;

        // ABI 42: the id a hit reports is the nearest ancestor that has one. A splitter's future
        // children then still name the splitter instead of reporting an empty id the editor cannot
        // match, and the root element reports empty, the same as hitting nothing.
        std::string_view hit_id(Rml::Element* element, Rml::Element* root) {
            while (element != nullptr && element != root && element->GetId().empty()) {
                element = element->GetParentNode();
            }
            return element != nullptr && element != root ? std::string_view(element->GetId().data(), element->GetId().size())
                                                         : std::string_view{};
        }
    } // namespace

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

        if (!Rml::LoadFontFace(utf8_text(font)) ||
            (!fallback_font.empty() && !Rml::LoadFontFace(utf8_text(fallback_font), true))) {
            this->close();
            return false;
        }

        auto* const context = Rml::CreateContext(context_name, {1, 1}, &render_interface);
        if (!context) {
            this->close();
            return false;
        }
        auto* const document = context->LoadDocument(utf8_text(markup));
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
        this->pointer_events.clear();
        this->pressed_buttons = 0;
        this->scene_texture_loaded = false;
    }

    void rml_document::set_density_independent_pixel_ratio(float ratio) noexcept {
        this->density_independent_pixel_ratio = ratio > 0.0F && std::isfinite(ratio) ? ratio : 1.0F;
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
        // Framebuffer pixels in, document pixels out: without this a 150%/200% window draws the whole
        // document at half the size it was authored for.
        context->SetDensityIndependentPixelRatio(this->density_independent_pixel_ratio);
        context->SetDimensions(dimensions);
        if (!context->Update()) {
            return {};
        }

        render_interface.SetTransform(nullptr);
        this->rendering = true;
        bool const rendered = context->Render();
        this->rendering = false;
        if (!rendered) {
            return {};
        }
        return this->draws;
    }
    bool rml_document::set_text(std::string_view id, std::string_view text) {
        return set_markup(id, Rml::StringUtilities::EncodeRml(Rml::String(text)));
    }

    bool rml_document::set_markup(std::string_view id, std::string_view markup) {
        auto* context = render_interface.document == this ? Rml::GetContext(context_name) : nullptr;
        auto* element = context ? context->GetDocument(0)->GetElementById(Rml::String(id)) : nullptr;
        if (!element) {
            return false;
        }
        // A SELECT KEEPS ITS OPTIONS IN ITS WIDGET, NOT IN THE ELEMENT TREE. RmlUi moves the option
        // children of a `select` into the drop-down widget on the next update, so parsing new markup
        // under the element only stages a second set of options: the widget keeps the first set and
        // gains the new one, and the popup lists every entry twice. Clearing the widget's list first
        // makes a markup write a replacement, which is what every caller means.
        //
        // THE WRITE IS SILENT: replacing a select's options drops its selection, which makes RmlUi
        // dispatch a `change` event carrying an EMPTY value. A panel that repaints its controls from
        // the document would read that as "the user cleared the field" and undo the edit the repaint
        // was there to show (the per-object environment row reset itself to the scene map this way).
        auto const previous_suppress = this->suppress_events;
        this->suppress_events = true;
        if (element->GetTagName() == "select") {
            static_cast<Rml::ElementFormControlSelect*>(element)->RemoveAll();
        }
        element->SetInnerRML(Rml::String(markup));
        this->suppress_events = previous_suppress;
        return true;
    }
    bool rml_document::set_value(std::string_view id, std::string_view value) {
        auto* context = render_interface.document == this ? Rml::GetContext(context_name) : nullptr;
        auto* element = context ? context->GetDocument(0)->GetElementById(Rml::String(id)) : nullptr;
        if (!element) {
            return false;
        }
        // The tree builds with -fno-rtti in Release, so the downcast is proven by tag name - only `input`,
        // `textarea` and `select` derive from ElementFormControl - and then made with static_cast, the same
        // idiom text() uses for `#text` nodes.
        auto const& tag = element->GetTagName();
        if (tag == "img") {
            element->SetAttribute("src", Rml::String(value));
            return true;
        }
        if (tag != "input" && tag != "textarea" && tag != "select") {
            return false;
        }
        auto* const control = static_cast<Rml::ElementFormControl*>(element);
        // Programmatic updates must not feed the managed change-event handler back into itself. The flag is
        // saved and restored rather than cleared, so a caller already suppressing events (set_markup, or a
        // future nested update) keeps its own suppression alive.
        bool const previous_suppress = this->suppress_events;
        this->suppress_events = true;
        control->SetValue(Rml::String(value));
        this->suppress_events = previous_suppress;
        return true;
    }
    bool rml_document::set_visible(std::string_view id, bool visible) {
        auto* context = render_interface.document == this ? Rml::GetContext(context_name) : nullptr;
        auto* element = context ? context->GetDocument(0)->GetElementById(Rml::String(id)) : nullptr;
        if (!element) {
            return false;
        }
        if (!visible) {
            // Hiding a subtree only has to drop keyboard focus when the focused control lives inside it, so
            // walk the focus chain instead of synthesizing a full focus-loss. A synthesized focus-loss also
            // clears the hover chain, and a panel that hides its unused rows every frame would then wipe the
            // hovered element on the frame the mouse is over it - pointer presses could never land, because
            // the click target is resolved from the hover chain (the editor's outliner hides rows per frame).
            auto* focus = context->GetFocusElement();
            for (auto* node = focus; node != nullptr; node = node->GetParentNode()) {
                if (node == element) {
                    // BLUR THE CONTROL, DO NOT SYNTHESIZE A WINDOW FOCUS LOSS: the synthetic event also
                    // queues a pointer cancel and drops the hover chain, which is exactly what a panel
                    // that hides its rows every frame must not do to the row under the pointer.
                    focus->Blur();
                    break;
                }
            }
        }
        element->SetProperty("visibility", visible ? "visible" : "hidden");
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
    std::uint32_t rml_document::option_count(std::string_view id) const {
        auto* context = render_interface.document == this ? Rml::GetContext(context_name) : nullptr;
        auto* element = context ? context->GetDocument(0)->GetElementById(Rml::String(id)) : nullptr;
        if (!element || element->GetTagName() != "select") {
            return 0;
        }
        // The count is the widget's, not the element's: RmlUi moves a select's options into the drop-down
        // widget, so the information the popup shows is the only honest answer.
        return static_cast<std::uint32_t>(static_cast<Rml::ElementFormControlSelect*>(element)->GetNumOptions());
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
    utility::vessel::vector<pointer_event> rml_document::take_pointer_events() {
        utility::vessel::vector<pointer_event> result;
        result.swap(this->pointer_events);
        return result;
    }
    void rml_document::push_pointer(pointer_type type, int x, int y, int button, unsigned modifiers, std::string_view id, float wheel) {
        auto const ratio = this->density_independent_pixel_ratio;
        pointer_event event;
        event.type = type;
        if (!id.empty()) {
            event.id.assign(id.data(), id.size());
        }
        // The managed side authors in dp, so the framebuffer pixels the window reports are divided
        // here once, and pointer coordinates and rects then share a unit.
        event.x = type == pointer_type::wheel ? wheel : static_cast<float>(x) / ratio;
        event.y = static_cast<float>(y) / ratio;
        event.button = button;
        event.modifiers = modifiers;
        // EVERY MOVE IS REPORTED (a managed drag needs the moves after the pointer leaves the pressed
        // element), so the queue is not coalesced; overflow protection lives in the cap below.
        // A session that never polls must not grow this without bound. Once the cap is reached, the
        // continuous input goes first: an older move is evicted to make room for a press or a release,
        // and a press or a release that has nothing left to evict is kept rather than dropped (an
        // unpaired release is harmless, an unpaired press would look like a drag that never ends).
        constexpr std::size_t max_pointer_events = 256;
        if (this->pointer_events.size() >= max_pointer_events) {
            bool evicted = false;
            for (auto it = this->pointer_events.begin(); it != this->pointer_events.end(); ++it) {
                if (it->type == pointer_type::move) {
                    this->pointer_events.erase(it);
                    evicted = true;
                    break;
                }
            }
            if (!evicted && (event.type == pointer_type::move || event.type == pointer_type::wheel)) {
                return;
            }
            if (!evicted) {
                this->pointer_events.erase(this->pointer_events.begin());
            }
        }
        this->pointer_events.push_back(std::move(event));
    }
    bool rml_document::element_rect(std::string_view id, float& x, float& y, float& width, float& height) const {
        x = 0.0F;
        y = 0.0F;
        width = 0.0F;
        height = 0.0F;
        auto* context = render_interface.document == this ? Rml::GetContext(context_name) : nullptr;
        if (!context) {
            return false;
        }
        Rml::Element* element = nullptr;
        if (id.empty()) {
            element = context->GetRootElement();
        } else if (auto* document = context->GetDocument(0)) {
            element = document->GetElementById(Rml::String(id));
        }
        if (!element) {
            return false;
        }
        auto const offset = element->GetAbsoluteOffset(Rml::BoxArea::Border);
        auto const size = element->GetBox().GetSize();
        auto const ratio = this->density_independent_pixel_ratio;
        x = offset.x / ratio;
        y = offset.y / ratio;
        width = size.x / ratio;
        height = size.y / ratio;
        return true;
    }
    style_status rml_document::set_style(std::string_view id, std::string_view property, std::string_view value) {
        auto* context = render_interface.document == this ? Rml::GetContext(context_name) : nullptr;
        auto* const document = context ? context->GetDocument(0) : nullptr;
        auto* element = document && !id.empty() ? document->GetElementById(Rml::String(id)) : nullptr;
        if (!element) {
            return style_status::missing_element;
        }
        return element->SetProperty(Rml::String(property), Rml::String(value)) ? style_status::success : style_status::rejected;
    }
    bool rml_document::wants_mouse() const {
        auto* context = render_interface.document == this ? Rml::GetContext(context_name) : nullptr;
        if (this->pressed_buttons != 0) {
            return context != nullptr;
        }
        auto* const hover = context ? context->GetHoverElement() : nullptr;
        if (hover == nullptr || hover == context->GetRootElement()) {
            return false;
        }
        // A document shows the 3D scene through <img src="deren://scene">: while the pointer rests on that
        // image the game keeps the mouse, so the overlay reports no mouse exposure and the camera behind it
        // can orbit. Every other element under the pointer is a UI hit.
        for (auto* node = hover; node != nullptr; node = node->GetParentNode()) {
            if (node->GetTagName() == "img" && node->GetAttribute("src", Rml::String{}) == Rml::String(scene_color_uri)) {
                return false;
            }
        }
        // The document element is the RML <body>: it fills the viewport, so resting the mouse over empty
        // HUD space must not look like an interaction - the game keeps its mouse. Any element under the
        // pointer is a hit unless it (or a parent) opted out with `pointer-events: none`, which makes
        // GetElementAtPoint return nothing at all and lets the hover fall through to <body>.
        if (hover->GetTagName() == "body") {
            return false;
        }
        return hover->GetOwnerDocument() != hover;
    }
    bool rml_document::wants_keyboard() const {
        auto* context = render_interface.document == this ? Rml::GetContext(context_name) : nullptr;
        auto* focus = context ? context->GetFocusElement() : nullptr;
        if (!focus) {
            return false;
        }
        // Buttons keep focus after a click (so Tab navigation keeps working upstream), but for a game
        // overlay only text entry should take the keyboard away from the game.
        auto const& tag = focus->GetTagName();
        return tag == "input" || tag == "textarea" || tag == "select";
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
            this->pointer_x = event.x;
            this->pointer_y = event.y;
            context->ProcessMouseMove(event.x, event.y, event.modifiers);
            // ABI 42: every forwarded move is reported, including the moves of a drag that has left
            // the element it started on, because the editor locks its target on pointer_down.
            this->push_pointer(pointer_type::move, event.x, event.y, event.button,
                               static_cast<unsigned>(event.modifiers), hit_id(context->GetHoverElement(), context->GetRootElement()));
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
                this->push_pointer(pointer_type::down, this->pointer_x, this->pointer_y, event.button,
                                   static_cast<unsigned>(event.modifiers), hit_id(context->GetHoverElement(), context->GetRootElement()));
            } else {
                context->ProcessMouseButtonUp(event.button, event.modifiers);
                this->pressed_buttons &= ~(1u << event.button);
                this->push_pointer(pointer_type::up, this->pointer_x, this->pointer_y, event.button,
                                   static_cast<unsigned>(event.modifiers), hit_id(context->GetHoverElement(), context->GetRootElement()));
            }
            return mouse;
        case input_type::wheel:
            context->ProcessMouseWheel(Rml::Vector2f{0, -event.wheel}, event.modifiers);
            this->push_pointer(pointer_type::wheel, 0, 0, 0, static_cast<unsigned>(event.modifiers),
                               hit_id(context->GetHoverElement(), context->GetRootElement()), event.wheel);
            return mouse;
        case input_type::character:
            if (!keyboard) {
                return false;
            }
            if (event.codepoint <= 0x10ffff && !(event.codepoint >= 0xd800 && event.codepoint <= 0xdfff)) {
                context->ProcessTextInput(static_cast<Rml::Character>(event.codepoint));
            }
            return true;
        case input_type::key: {
            // Only text entry takes the keyboard. A focused button is not a reason to hide key
            // events from the game, and forwarding them would activate the button while the game
            // also handled the key.
            if (!keyboard) {
                return false;
            }
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
            return true;
        }
        case input_type::mouse_leave:
            context->ProcessMouseLeave();
            return this->has_mouse_capture();
        case input_type::focus_lost:
            this->push_pointer(pointer_type::cancel, 0, 0, 0, 0, {});
            if (auto* focus = context->GetFocusElement()) {
                focus->Blur();
            }
            context->ProcessMouseLeave();
            // RELEASE EVERY BUTTON THE DOCUMENT THINKS IS DOWN: button accepts 0..7 and pressed_buttons
            // tracks the same eight, so releasing only 0..2 left 3..7 stuck down after an OS focus loss.
            for (int button = 0; button < 8; ++button) {
                if (button < 3 || (this->pressed_buttons & (1u << button)) != 0) {
                    context->ProcessMouseButtonUp(button, 0);
                }
            }
            this->pressed_buttons = 0;
            return false;
        }
        return false;
    }
} // namespace deren::ui
