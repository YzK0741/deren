module;

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

export module deren.app.glfw;

import deren.input_actions;
import deren.utility;

export namespace deren::app {
    deren::input::key action_key(int code) {
        using namespace deren::input;
        if (code >= GLFW_KEY_A && code <= GLFW_KEY_Z) {
            return static_cast<key>('a' + code - GLFW_KEY_A);
        }
        if (code >= GLFW_KEY_0 && code <= GLFW_KEY_9) {
            return static_cast<key>('0' + code - GLFW_KEY_0);
        }
        switch (code) {
        case GLFW_KEY_SPACE:
            return space;
        case GLFW_KEY_LEFT:
            return left;
        case GLFW_KEY_RIGHT:
            return right;
        case GLFW_KEY_UP:
            return up;
        case GLFW_KEY_DOWN:
            return down_arrow;
        case GLFW_KEY_ENTER:
            return enter;
        case GLFW_KEY_TAB:
            return tab;
        case GLFW_KEY_LEFT_SHIFT:
            return left_shift;
        case GLFW_KEY_RIGHT_SHIFT:
            return right_shift;
        case GLFW_KEY_LEFT_CONTROL:
            return left_control;
        case GLFW_KEY_RIGHT_CONTROL:
            return right_control;
        default:
            return unknown;
        }
    }

    /// One window event, in the app's own vocabulary: GLFW's constants stay on this side of the seam
    /// and the scripting boundary converts to its flat mirror when a managed game asks for input.
    unsigned ui_key(int key) noexcept {
        if (key >= 32 && key <= 126) {
            return static_cast<unsigned>(key);
        }
        switch (key) {
        case GLFW_KEY_BACKSPACE:
            return 8;
        case GLFW_KEY_TAB:
            return 9;
        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER:
            return 13;
        case GLFW_KEY_ESCAPE:
            return 27;
        case GLFW_KEY_LEFT:
            return 256;
        case GLFW_KEY_RIGHT:
            return 257;
        case GLFW_KEY_UP:
            return 258;
        case GLFW_KEY_DOWN:
            return 259;
        case GLFW_KEY_DELETE:
            return 260;
        case GLFW_KEY_HOME:
            return 261;
        case GLFW_KEY_END:
            return 262;
        case GLFW_KEY_PAGE_UP:
            return 263;
        case GLFW_KEY_PAGE_DOWN:
            return 264;
        default:
            return 0;
        }
    }

    enum class window_event_type : unsigned {
        mouse_move = 1,
        mouse_button,
        scroll,
        key,
        character,
        focus_lost,
    };

    struct window_event {
        window_event_type type = window_event_type::focus_lost;
        /// The mouse button index, for `mouse_button`.
        int button = 0;
        /// Whether the button or key went down; `false` is an up event.
        bool down = false;
        /// The key code GLFW reported, for `key`.
        int key = 0;
        /// The cursor position in window pixels.
        float x = 0.0F;
        float y = 0.0F;
        /// The cursor delta since the previous move.
        float dx = 0.0F;
        float dy = 0.0F;
        /// The scroll amount, for `scroll`.
        float wheel = 0.0F;
        /// The typed character, for `character`.
        unsigned codepoint = 0;
        int modifiers = 0;
        bool repeated = false;
    };

    // The application's complete GLFW surface: window lifecycle, queries and input callbacks.
    // The engine only receives native_handle(); it never owns a window.
    struct glfw_window {
        glfw_window(int width, int height, char const* title, bool visible, bool offscreen = false)
            : window_(nullptr) {
            if (offscreen) {
                return;
            }
            if (glfwInit() != GLFW_TRUE) {
                deren::utility::panic("glfwInit failed");
            }
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
            glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);
            window_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
            if (window_ == nullptr) {
                deren::utility::panic("glfwCreateWindow failed");
            }
            if (visible) {
                glfwShowWindow(window_);
            } else {
                glfwHideWindow(window_);
            }
        }

        glfw_window(glfw_window const&) = delete ("glfw_window is non-copyable: pass it by reference or construct a new instance");
        glfw_window& operator=(glfw_window const&) = delete ("glfw_window is non-copyable: modify the existing instance instead");

        ~glfw_window() {
            if (window_ != nullptr) {
                glfwDestroyWindow(window_);
                glfwTerminate();
            }
        }

        GLFWwindow* native_handle() const noexcept {
            return window_;
        }

        void window_size(int& width, int& height) const noexcept {
            if (window_ == nullptr) {
                width = height = 0;
                return;
            }
            glfwGetWindowSize(window_, &width, &height);
        }

        void framebuffer_size(int& width, int& height) const noexcept {
            if (window_ == nullptr) {
                width = height = 0;
                return;
            }
            glfwGetFramebufferSize(window_, &width, &height);
        }

        bool left_mouse_button_down() const noexcept {
            return window_ != nullptr && glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        }

        void cursor_position(double& x, double& y) const noexcept {
            if (window_ == nullptr) {
                x = y = 0.0;
                return;
            }
            glfwGetCursorPos(window_, &x, &y);
        }

        bool should_close() const noexcept {
            return window_ != nullptr && glfwWindowShouldClose(window_) == GLFW_TRUE;
        }

        void poll_events() const noexcept {
            if (window_ != nullptr) {
                glfwPollEvents();
            }
        }

    private:
        GLFWwindow* window_;
    };

    // The app owns one GLFW window. The GUI chains these callbacks and restores them on teardown.
    //
    // THE ACTION MAP AND THE RAW SINK ARE TWO READERS OF ONE EVENT STREAM: the action map is the
    // engine's own vocabulary (named actions, held keys), while `set_event_sink` hands every event
    // unchanged to whoever wants the whole stream - the managed game, in this application. Neither
    // reader keeps state for the other.
    struct glfw_action_input {
        using event_sink = void (*)(void* context, window_event const& event);
        using event_filter = bool (*)(void* context, window_event const& event);
        void set_event_filter(event_filter filter, void* context) noexcept {
            this->filter = filter;
            this->filter_context = context;
        }

        glfw_action_input(glfw_window const* host, deren::input::action_state& state)
            : window_(host == nullptr ? nullptr : host->native_handle())
            , state(state) {
            if (window_ == nullptr) {
                return;
            }
            active = this;
            glfwSetKeyCallback(window_, [](GLFWwindow* event_window, int code, int, int action, int mods) {
                if (active->window_ != event_window) {
                    return;
                }
                active->modifiers = mods;
                if (action == GLFW_PRESS || action == GLFW_RELEASE || action == GLFW_REPEAT) {
                    window_event event{.type = window_event_type::key, .down = action != GLFW_RELEASE, .key = code, .modifiers = mods, .repeated = action == GLFW_REPEAT};
                    bool const consumed = active->publish(event);
                    if (action != GLFW_REPEAT) {
                        active->state.set_key(action_key(code), action == GLFW_PRESS && !consumed);
                    }
                }
            });
            glfwSetWindowFocusCallback(window_, [](GLFWwindow* event_window, int focused) {
                if (active->window_ != event_window) {
                    return;
                }
                if (focused == GLFW_FALSE) {
                    active->state.release_keys();
                    active->modifiers = 0;
                    window_event event{.type = window_event_type::focus_lost, .down = false};
                    active->publish(event);
                }
            });
            glfwSetCursorPosCallback(window_, [](GLFWwindow* event_window, double x, double y) {
                if (active->window_ != event_window) {
                    return;
                }
                auto const fx = static_cast<float>(x);
                auto const fy = static_cast<float>(y);
                // THE FIRST POSITION IS NOT A MOVE: GLFW reports one on the first poll, and a delta from
                // an origin the pointer was never at would start a mouse-look with a jump.
                auto const dx = active->has_cursor ? fx - active->cursor_x : 0.0F;
                auto const dy = active->has_cursor ? fy - active->cursor_y : 0.0F;
                active->cursor_x = fx;
                active->cursor_y = fy;
                active->has_cursor = true;
                window_event event{.type = window_event_type::mouse_move, .x = fx, .y = fy, .dx = dx, .dy = dy, .modifiers = active->modifiers};
                active->publish(event);
            });
            glfwSetMouseButtonCallback(window_, [](GLFWwindow* event_window, int button, int action, int mods) {
                if (active->window_ != event_window || (action != GLFW_PRESS && action != GLFW_RELEASE)) {
                    return;
                }
                window_event event{.type = window_event_type::mouse_button,
                                   .button = button,
                                   .down = action == GLFW_PRESS,
                                   .modifiers = mods};
                active->publish(event);
            });
            glfwSetScrollCallback(window_, [](GLFWwindow* event_window, double, double yoffset) {
                if (active->window_ != event_window) {
                    return;
                }
                window_event event{.type = window_event_type::scroll, .wheel = static_cast<float>(yoffset), .modifiers = active->modifiers};
                active->publish(event);
            });
            glfwSetCharCallback(window_, [](GLFWwindow* event_window, unsigned codepoint) {
                if (active->window_ != event_window) {
                    return;
                }
                window_event event{.type = window_event_type::character, .codepoint = codepoint};
                active->publish(event);
            });
        }

        ~glfw_action_input() {
            if (window_ != nullptr) {
                glfwSetKeyCallback(window_, nullptr);
                glfwSetWindowFocusCallback(window_, nullptr);
                glfwSetCursorPosCallback(window_, nullptr);
                glfwSetMouseButtonCallback(window_, nullptr);
                glfwSetScrollCallback(window_, nullptr);
                glfwSetCharCallback(window_, nullptr);
                active = nullptr;
            }
        }

        /// Hands every event to @p sink from now on; a null sink stops the forwarding.
        void set_event_sink(event_sink sink, void* context) noexcept {
            this->sink = sink;
            this->sink_context = context;
        }

        glfw_action_input(glfw_action_input const&) = delete;
        glfw_action_input& operator=(glfw_action_input const&) = delete;

    private:
        bool publish(window_event const& event) const noexcept {
            bool const consumed = this->filter && this->filter(this->filter_context, event);
            bool const release = (event.type == window_event_type::key || event.type == window_event_type::mouse_button) && !event.down;
            if (this->sink && !event.repeated && (!consumed || release || event.type == window_event_type::focus_lost)) {
                this->sink(this->sink_context, event);
            }
            return consumed;
        }

        GLFWwindow* window_;
        deren::input::action_state& state;
        // The raw stream's one reader, and where the cursor was on the previous move event.
        int modifiers = 0;
        event_filter filter = nullptr;
        void* filter_context = nullptr;
        event_sink sink = nullptr;
        void* sink_context = nullptr;
        float cursor_x = 0.0F;
        float cursor_y = 0.0F;
        bool has_cursor = false;
        static inline glfw_action_input* active = nullptr;
    };
} // namespace deren::app
