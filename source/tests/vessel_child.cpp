#include "vessel_child.hpp"

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <string>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#endif

import deren.utility.shared;

void vessel_prepare_child() noexcept {
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::signal(SIGABRT, +[](int) {
        constexpr char marker[] = "abort signal received\n";
#if defined(_WIN32)
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), marker, sizeof(marker) - 1, &written, nullptr);
#else
        static_cast<void>(::write(STDERR_FILENO, marker, sizeof(marker) - 1));
#endif
    });
}

vessel_child_result vessel_run_child(char const* executable, char const* mode, char const* log) noexcept {
#if defined(_WIN32)
    deren::utility::vessel::wstring path(std::filesystem::absolute(executable).wstring());
    deren::utility::vessel::string mode_text(mode);
    deren::utility::vessel::wstring command = L"\"" + path + L"\" " + deren::utility::vessel::wstring(mode_text.begin(), mode_text.end());
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE capture = CreateFileA(log, GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (capture == INVALID_HANDLE_VALUE)
        return {};
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = capture;
    startup.hStdError = capture;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION child{};
    BOOL created = CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
    CloseHandle(capture);
    if (!created)
        return {};
    vessel_child_result result;
    if (WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0) {
        DWORD status = 0;
        if (GetExitCodeProcess(child.hProcess, &status))
            result = {true, status == 3, status};
    } else {
        TerminateProcess(child.hProcess, 99);
        WaitForSingleObject(child.hProcess, 1000);
    }
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return result;
#else
    int capture = ::open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (capture < 0)
        return {};
    auto path = std::filesystem::absolute(executable).string();
    pid_t child = ::fork();
    if (child == 0) {
        ::dup2(capture, STDOUT_FILENO);
        ::dup2(capture, STDERR_FILENO);
        ::close(capture);
        ::execl(path.c_str(), path.c_str(), mode, static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::close(capture);
    if (child < 0)
        return {};
    int status = 0;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        pid_t waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child)
            return {true, WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, static_cast<unsigned>(status)};
        if (waited < 0)
            return {};
        ::usleep(10000);
    }
    ::kill(child, SIGKILL);
    ::waitpid(child, &status, 0);
    return {};
#endif
}
