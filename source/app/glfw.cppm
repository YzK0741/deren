module;

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <dwmapi.h>
#endif

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
        glfw_window(int width, int height, char const* title, bool visible, bool offscreen = false, [[maybe_unused]] bool dark_frame = false,
                    [[maybe_unused]] bool borderless = false)
            : window_(nullptr) {
            if (offscreen) {
                return;
            }
            if (!acquire_platform()) {
                deren::utility::panic("glfwInit failed");
            }
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
            if (borderless) {
                // An undecorated window has no system caption or resize frame; the subclass installed
                // below gives it back the resize band and lets the managed UI draw its own caption.
                glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
            }
            window_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
            if (window_ == nullptr) {
                deren::utility::panic("glfwCreateWindow failed");
            }
#ifdef _WIN32
            if (borderless) {
                // FAILING LOUDLY IS THE POINT: a window whose frame could not be installed stays an
                // undecorated popup - no resize band, no taskbar entry, no maximize - while the
                // managed UI would still draw caption buttons over it. The editor's whole shell is
                // that caption, so a silent downgrade is worse than refusing to start.
                if (!install_borderless_frame()) {
                    deren::utility::panic("the borderless window frame could not be installed");
                }
            } else if (dark_frame) {
                auto const hwnd = glfwGetWin32Window(window_);
                BOOL const dark = TRUE;
                COLORREF const caption = RGB(21, 21, 21);
                COLORREF const border = RGB(47, 47, 47);
                COLORREF const title_text = RGB(224, 224, 224);
                DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
                DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
                DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
                DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &title_text, sizeof(title_text));
            }
#endif
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
#ifdef _WIN32
                // NOT GATED ON A "BORDERLESS" FLAG: the lookup is by HWND, so a slot an earlier
                // window left behind (a failed install, or a subclass that outlived its owner) is
                // cleared here instead of being "restored" onto an unrelated window later.
                remove_borderless_frame();
#endif
                glfwDestroyWindow(window_);
                release_platform();
            }
        }

        GLFWwindow* native_handle() const noexcept {
            return window_;
        }

        /// Hides the window in the taskbar the way the system minimize button does.
        void minimize_window() const noexcept {
            if (window_ != nullptr) {
                glfwIconifyWindow(window_);
            }
        }

        /// Maximizes a restored window and restores a maximized one.
        void toggle_maximize_window() const noexcept {
            if (window_ == nullptr) {
                return;
            }
            if (glfwGetWindowAttrib(window_, GLFW_MAXIMIZED) == GLFW_TRUE) {
                glfwRestoreWindow(window_);
            } else {
                glfwMaximizeWindow(window_);
            }
        }

        bool is_maximized_window() const noexcept {
            return window_ != nullptr && glfwGetWindowAttrib(window_, GLFW_MAXIMIZED) == GLFW_TRUE;
        }

        /// Asks the main loop to stop, exactly like the window's own close request.
        void request_close() const noexcept {
            if (window_ != nullptr) {
                glfwSetWindowShouldClose(window_, GLFW_TRUE);
            }
        }

        /// Hands a caption-area press to the system move loop, so the window follows the pointer
        /// until the button comes up. This call blocks for the duration of the drag.
        bool begin_window_drag() const noexcept {
#ifdef _WIN32
            if (window_ == nullptr) {
                return false;
            }
            auto const hwnd = glfwGetWin32Window(window_);
            if (hwnd == nullptr) {
                return false;
            }
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return true;
#else
            return false;
#endif
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

        // GLFW'S PLATFORM STATE IS PROCESS WIDE. A second glfwInit() registers another copy of the
        // Win32 window class and re-runs the platform's global setup, and any glfwTerminate() tears
        // the platform down under every window still open - so initialization is counted and the
        // platform lives exactly as long as the first window to the last one.
        inline static unsigned live_windows = 0;

        static bool acquire_platform() {
            if (live_windows != 0) {
                ++live_windows;
                return true;
            }
            if (glfwInit() != GLFW_TRUE) {
                return false;
            }
            live_windows = 1;
            return true;
        }

        static void release_platform() {
            if (live_windows == 0) {
                return;
            }
            if (--live_windows == 0) {
                glfwTerminate();
            }
        }

#ifdef _WIN32
        // The subclassed window procedure and the procedure it replaced. A subclass keeps working
        // after its GLFWwindow is destroyed (the HWND is gone, so in practice it never runs again),
        // which is why the slot is only cleared when the state moves out of the table here.
        struct frame_state {
            HWND hwnd;
            WNDPROC previous;
        };

        static constexpr unsigned frame_capacity = 4;

        static frame_state* find_frame(HWND hwnd) noexcept {
            for (unsigned index = 0; index < frame_count; ++index) {
                if (frames[index].hwnd == hwnd) {
                    return &frames[index];
                }
            }
            return nullptr;
        }

        // THE CLIENT RECT THIS PROCEDURE ANSWERS WITH, decided by the PROPOSED WINDOW RECT alone.
        // A maximized window with a resize frame is inflated past the screen edges, so its client stops
        // at the monitor work area and keeps the content fully visible. Anything smaller is the proposed
        // rect itself: the client covers the whole window. THE ZOOM STATE IS DELIBERATELY NOT CONSULTED
        // (no IsZoomed): ShowWindow(SW_RESTORE) sends WM_NCCALCSIZE while the window still answers
        // IsZoomed() == true, and trusting that answer left a restored window with a MAXIMIZED-SIZE
        // CLIENT - bigger than the window it belonged to - so the UI stayed laid out for the old extent
        // and the window showed only its top-left corner, with no resize ever reported to the swapchain.
        static RECT borderless_client_rect(RECT const& proposed) noexcept {
            MONITORINFO monitor_info{};
            monitor_info.cbSize = sizeof(monitor_info);
            HMONITOR const monitor = MonitorFromRect(&proposed, MONITOR_DEFAULTTONEAREST);
            if (GetMonitorInfoW(monitor, &monitor_info) == 0) {
                return proposed;
            }
            RECT const work = monitor_info.rcWork;
            bool const covers_work_area =
                proposed.left <= work.left && proposed.top <= work.top && proposed.right >= work.right && proposed.bottom >= work.bottom;
            return covers_work_area ? work : proposed;
        }

        // THE FRAME THE SYSTEM CANNOT DRAW: an undecorated window has no caption, no resize border
        // and no maximize animation, so this procedure hands those behaviours back. The client covers
        // the whole window (WM_NCCALCSIZE), the window keeps WS_THICKFRAME for resizing and the
        // taskbar, and WM_NCHITTEST defines the 6 dp band the user can grab.
        static LRESULT CALLBACK frame_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
            frame_state* state = find_frame(hwnd);
            if (state == nullptr || state->previous == nullptr) {
                return DefWindowProcW(hwnd, message, wparam, lparam);
            }
            switch (message) {
            case WM_NCCALCSIZE: {
                if (wparam == FALSE) {
                    // lParam is a plain RECT holding the proposed window rect: turn it into the client.
                    RECT window_rect{};
                    GetWindowRect(hwnd, &window_rect);
                    *reinterpret_cast<RECT*>(lparam) = borderless_client_rect(window_rect);
                    return 0;
                }
                auto* parameters = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
                // THE PROPOSED RECT IS rgrc[0], NOT rgrc[1]. rgrc[0] is the NEW window rectangle,
                // rgrc[1] is the OLD one and rgrc[2] the old client. Answering with rgrc[1] left the
                // client one resize behind, which looked harmless while the user kept resizing and
                // became the reported bug on a restore: the client stayed at the maximized size, the
                // document kept the old extent, and the window showed only its top-left corner with no
                // out-of-date ever reported to the swapchain.
                RECT const proposed = parameters->rgrc[0];
                parameters->rgrc[0] = borderless_client_rect(proposed);
                return 0;
            }
            case WM_NCHITTEST: {
                LRESULT const result = CallWindowProcW(state->previous, hwnd, message, wparam, lparam);
                if (result != HTCLIENT || IsZoomed(hwnd) != 0) {
                    return result;
                }
                int const border = 6;
                int const x = static_cast<short>(static_cast<unsigned short>(lparam & 0xFFFFU));
                int const y = static_cast<short>(static_cast<unsigned short>((lparam >> 16) & 0xFFFFU));
                RECT rect{};
                GetWindowRect(hwnd, &rect);
                bool const left = x >= rect.left && x < rect.left + border;
                bool const right = x < rect.right && x >= rect.right - border;
                bool const top = y >= rect.top && y < rect.top + border;
                bool const bottom = y < rect.bottom && y >= rect.bottom - border;
                if (top && left) {
                    return HTTOPLEFT;
                }
                if (top && right) {
                    return HTTOPRIGHT;
                }
                if (bottom && left) {
                    return HTBOTTOMLEFT;
                }
                if (bottom && right) {
                    return HTBOTTOMRIGHT;
                }
                if (left) {
                    return HTLEFT;
                }
                if (right) {
                    return HTRIGHT;
                }
                if (top) {
                    return HTTOP;
                }
                if (bottom) {
                    return HTBOTTOM;
                }
                return HTCLIENT;
            }
            default:
                return CallWindowProcW(state->previous, hwnd, message, wparam, lparam);
            }
        }

        // TRUE when the frame is installed and the window can be resized, moved and maximized like a
        // decorated one. FALSE leaves the window as GLFW created it: an undecorated popup with no
        // resize band and no taskbar entry. The caller must not pretend otherwise.
        bool install_borderless_frame() noexcept {
            auto const hwnd = glfwGetWin32Window(window_);
            if (hwnd == nullptr || frame_count >= frame_capacity || find_frame(hwnd) != nullptr) {
                return false;
            }
            // An undecorated GLFW window is WS_POPUP: it has no resize frame and no taskbar entry.
            // The thick frame supplies both; the subclass above hides what it would otherwise draw.
            auto const style = GetWindowLongPtrW(hwnd, GWL_STYLE);
            SetWindowLongPtrW(hwnd, GWL_STYLE,
                              style | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
            auto const previous = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&glfw_window::frame_proc)));
            if (previous == nullptr) {
                // THE SUBCLASS DID NOT TAKE: putting the thick frame back on a window whose procedure
                // is still GLFW's own would fight our own frame_proc assumptions (no NCCALCSIZE
                // answer, no hit-test band), so restore the popup style instead of reporting a frame
                // the window does not have.
                SetWindowLongPtrW(hwnd, GWL_STYLE, style);
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                             SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return false;
            }
            frames[frame_count] = frame_state{.hwnd = hwnd, .previous = previous};
            ++frame_count;
            // The style change invalidates the frame; recompute it now so the first frame is correct.
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return true;
        }

        void remove_borderless_frame() noexcept {
            auto const hwnd = glfwGetWin32Window(window_);
            frame_state* state = find_frame(hwnd);
            if (state == nullptr) {
                return;
            }
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(state->previous));
            // Compact the table: the last slot takes this one's place and the tail is cleared.
            auto const slot = static_cast<unsigned>(state - frames);
            for (unsigned index = slot; index + 1 < frame_count; ++index) {
                frames[index] = frames[index + 1];
            }
            --frame_count;
            frames[frame_count] = frame_state{};
        }

        inline static frame_state frames[frame_capacity] = {};
        inline static unsigned frame_count = 0;
#endif
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
            // THE SEAM POINTER IS SHARED BY EVERY WINDOW, so it is registered rather than assigned:
            // a seam installed later must not be forgotten when an earlier one is destroyed, and a
            // destroyed seam must not leave the callbacks routing through freed memory. The newest
            // live seam receives the events; the application owns one window, and a second seam is
            // expected to outlive the first only when it replaces it.
            next_live = live_head;
            live_head = this;
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
                    active->camera_drag = false;
                    window_event event{.type = window_event_type::focus_lost, .down = false};
                    active->publish(event);
                }
            });
            previous_cursor = glfwSetCursorPosCallback(window_, [](GLFWwindow* event_window, double x, double y) {
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
                // CHAINED, NOT REPLACED - AND ONLY WHEN THE UI DID NOT TAKE THE EVENT: the runtime's
                // orbit camera installed its cursor callback before this seam existed. A move the
                // document consumed (over a panel) must not also fly the camera; a move the camera
                // owns, even one that has wandered over a panel mid-drag, still must.
                bool const consumed = active->publish(event);
                if ((!consumed || active->camera_drag) && active->previous_cursor != nullptr) {
                    active->previous_cursor(event_window, x, y);
                }
            });
            previous_button = glfwSetMouseButtonCallback(window_, [](GLFWwindow* event_window, int button, int action, int mods) {
                if (active->window_ != event_window || (action != GLFW_PRESS && action != GLFW_RELEASE)) {
                    return;
                }
                window_event event{.type = window_event_type::mouse_button,
                                   .button = button,
                                   .down = action == GLFW_PRESS,
                                   .modifiers = mods};
                bool const consumed = active->publish(event);
                if (event.down) {
                    // THE PRESS DECIDES WHO DRAGS: consumed by the document means a panel owns the
                    // pointer; otherwise it is the camera's until the button comes back up.
                    active->camera_drag = !consumed;
                } else {
                    active->camera_drag = false;
                }
                // A RELEASE IS ALWAYS CHAINED. The camera clears its drag state on it, and a press
                // that the document consumed must not leave the camera believing the button is down.
                if ((!consumed || !event.down) && active->previous_button != nullptr) {
                    active->previous_button(event_window, button, action, mods);
                }
            });
            previous_scroll = glfwSetScrollCallback(window_, [](GLFWwindow* event_window, double xoffset, double yoffset) {
                if (active->window_ != event_window) {
                    return;
                }
                window_event event{.type = window_event_type::scroll, .wheel = static_cast<float>(yoffset), .modifiers = active->modifiers};
                if (!active->publish(event) && active->previous_scroll != nullptr) {
                    active->previous_scroll(event_window, xoffset, yoffset);
                }
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
                // THE THREE CHAINED CALLBACKS GO BACK TO THEIR PREVIOUS OWNER, not to null: the
                // runtime installed the orbit camera's handlers before this seam and they must
                // survive this object (teardown order has the runtime outlive us).
                glfwSetCursorPosCallback(window_, previous_cursor);
                glfwSetMouseButtonCallback(window_, previous_button);
                glfwSetScrollCallback(window_, previous_scroll);
                glfwSetCharCallback(window_, nullptr);
                // Unlink first: `active` falls back to the newest seam still alive, never to a
                // pointer into the windows that was just torn down and never to null while another
                // seam is installed.
                for (glfw_action_input** link = &live_head; *link != nullptr; link = &(*link)->next_live) {
                    if (*link == this) {
                        *link = next_live;
                        break;
                    }
                }
                if (active == this) {
                    active = live_head;
                }
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
        // THE CALLBACKS THIS SEAM REPLACED: the runtime's orbit camera installs its cursor/button/
        // scroll handlers in its constructor, and the ACTION SEAM IS INSTALLED AFTER IT so it can
        // capture them here and forward every event back (a plain overwrite silently dropped every
        // mouse event, so the managed UI never saw a hover). Key/focus/char are not chained because
        // the runtime installs none of them.
        GLFWcursorposfun previous_cursor = nullptr;
        GLFWmousebuttonfun previous_button = nullptr;
        GLFWscrollfun previous_scroll = nullptr;
        // THE CAMERA'S HALF OF THE ROUTING LATCH: a press the document did not consume belongs to the
        // orbit camera, and the moves that follow keep flowing to it even when the pointer wanders
        // over a panel, so a drag that started in the 3D view never freezes half way. A press the
        // document consumed never starts it. Cleared on release and on focus loss.
        bool camera_drag = false;
        // The raw stream's one reader, and where the cursor was on the previous move event.
        int modifiers = 0;
        event_filter filter = nullptr;
        void* filter_context = nullptr;
        event_sink sink = nullptr;
        void* sink_context = nullptr;
        float cursor_x = 0.0F;
        float cursor_y = 0.0F;
        bool has_cursor = false;
        // The registry of live seams, newest first: see the constructor and destructor for why a single
        // shared `active` pointer is not enough.
        glfw_action_input* next_live = nullptr;
        static inline glfw_action_input* active = nullptr;
        static inline glfw_action_input* live_head = nullptr;
    };
} // namespace deren::app
