#include "startup_splash.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace {
    constexpr wchar_t splash_class[] = L"DerenStartupSplash";
    constexpr UINT attach_owner_message = WM_APP + 1;

    LRESULT CALLBACK splash_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto const* creation = reinterpret_cast<CREATESTRUCTW const*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
        } else if (message == WM_ERASEBKGND || message == WM_CLOSE) {
            return 1; // Lifetime belongs to startup_splash, not an independent user-close action.
        } else if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            HDC const dc = BeginPaint(window, &paint);
            auto const bitmap = reinterpret_cast<HBITMAP>(GetWindowLongPtrW(window, GWLP_USERDATA));
            BITMAP dimensions{};
            RECT area{};
            GetClientRect(window, &area);
            GetObjectW(bitmap, sizeof(dimensions), &dimensions);
            HDC const source = CreateCompatibleDC(dc);
            if (source) {
                HGDIOBJ const previous = SelectObject(source, bitmap);
                SetStretchBltMode(dc, HALFTONE);
                SetBrushOrgEx(dc, 0, 0, nullptr);
                StretchBlt(dc, 0, 0, area.right, area.bottom, source, 0, 0,
                           dimensions.bmWidth, dimensions.bmHeight, SRCCOPY);
                SelectObject(source, previous);
                DeleteDC(source);
            }
            EndPaint(window, &paint);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
} // namespace

namespace deren::app {
    startup_splash::startup_splash(bool const enabled) noexcept {
        if (!enabled) {
            return;
        }
        ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ready_ && stop_) {
            thread_ = CreateThread(nullptr, 0, run, this, 0, &thread_id_);
        }
        if (thread_) {
            // Only wait for native window creation, never model/device loading.
            WaitForSingleObject(ready_, INFINITE);
        } else {
            close();
        }
    }

    unsigned long __stdcall startup_splash::run(void* const context) noexcept {
        auto& self = *static_cast<startup_splash*>(context);
        HINSTANCE const instance = GetModuleHandleW(nullptr);
        auto const bitmap = static_cast<HBITMAP>(LoadImageW(instance, MAKEINTRESOURCEW(102), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.hInstance = instance;
        description.lpfnWndProc = splash_proc;
        description.lpszClassName = splash_class;
        description.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW, independent of UNICODE
        ATOM const registered = bitmap ? RegisterClassExW(&description) : 0;
        HWND window = nullptr;
        if (registered) {
            RECT work{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            BITMAP dimensions{};
            GetObjectW(bitmap, sizeof(dimensions), &dimensions);
            int const available = work.right - work.left - 32;
            int const width = available > 0 && available < 720 ? available : 720;
            int const height = dimensions.bmHeight * width / dimensions.bmWidth;
            window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, splash_class, L"deren — starting",
                                     WS_POPUP, work.left + (work.right - work.left - width) / 2,
                                     work.top + (work.bottom - work.top - height) / 2, width, height,
                                     nullptr, nullptr, instance, bitmap);
        }
        if (window) {
            ShowWindow(window, SW_SHOWNOACTIVATE);
            UpdateWindow(window);
        }
        // CreateWindow created the message queue before the caller may post an owner.
        SetEvent(self.ready_);
        if (window) {
            HANDLE const stop = self.stop_;
            for (;;) {
                DWORD const wake = MsgWaitForMultipleObjects(1, &stop, FALSE, INFINITE, QS_ALLINPUT);
                if (wake != WAIT_OBJECT_0 + 1) {
                    break;
                }
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (message.message == attach_owner_message && message.hwnd == nullptr) {
                        SetWindowLongPtrW(window, GWLP_HWNDPARENT, static_cast<LONG_PTR>(message.wParam));
                        SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    } else {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                }
            }
            DestroyWindow(window);
        }
        if (registered) {
            UnregisterClassW(splash_class, instance);
        }
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return 0;
    }

    void startup_splash::attach_owner(GLFWwindow* const window) noexcept {
        if (thread_ && window) {
            PostThreadMessageW(thread_id_, attach_owner_message, reinterpret_cast<WPARAM>(glfwGetWin32Window(window)), 0);
        }
    }

    void startup_splash::close() noexcept {
        if (thread_) {
            SetEvent(stop_);
            WaitForSingleObject(thread_, INFINITE);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        if (ready_) {
            CloseHandle(ready_);
            ready_ = nullptr;
        }
        if (stop_) {
            CloseHandle(stop_);
            stop_ = nullptr;
        }
    }

    startup_splash::~startup_splash() {
        close();
    }
} // namespace deren::app
#else
namespace deren::app {
    startup_splash::startup_splash(bool) noexcept {}
    startup_splash::~startup_splash() = default;
    void startup_splash::attach_owner(GLFWwindow*) noexcept {}
    void startup_splash::close() noexcept {}
} // namespace deren::app
#endif
