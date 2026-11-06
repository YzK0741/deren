#pragma once

struct GLFWwindow;

namespace deren::app {
    // Application startup decoration; failures never prevent the renderer from starting.
    class startup_splash {
      public:
        explicit startup_splash(bool enabled) noexcept;
        ~startup_splash();
        startup_splash(startup_splash const&) = delete("startup_splash is non-copyable: pass it by reference or construct a new instance");
        startup_splash& operator=(startup_splash const&) = delete("startup_splash is non-copyable: modify the existing instance instead");

        void attach_owner(GLFWwindow* window) noexcept;
        void close() noexcept;

      private:
#ifdef _WIN32
        static unsigned long __stdcall run(void* context) noexcept;
        void* thread_ = nullptr;
        void* ready_ = nullptr;
        void* stop_ = nullptr;
        unsigned long thread_id_ = 0;
#endif
    };
} // namespace deren::app
