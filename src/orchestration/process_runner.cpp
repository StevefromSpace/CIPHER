#include "cipher/orchestration/process_runner.hpp"

#include <filesystem>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <thread>
#endif

namespace cipher::orchestration {

#ifdef _WIN32
namespace {

// Owns one Win32 handle. Invalid and null values are never closed.
class Handle {
public:
    Handle() = default;
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    HANDLE get() const { return handle_; }
    void reset(HANDLE handle = nullptr) {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

std::wstring to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = static_cast<int>(text.size());
    const int wide_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
    if (wide_size <= 0) throw std::invalid_argument("process argument is not valid UTF-8");
    std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), size, wide.data(), wide_size);
    return wide;
}

// MSVC C runtime rules: quote when empty or containing space, tab or quote; a backslash run before a quote doubles.
std::wstring quote_argument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) return argument;
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

// Reads one stream to EOF. Past the cap, data is discarded but still drained so the child never blocks on a full pipe.
void drain_pipe(HANDLE pipe, std::size_t cap, std::string& out, bool& overflow) {
    char buffer[4096];
    DWORD bytes_read = 0;
    while (ReadFile(pipe, buffer, sizeof buffer, &bytes_read, nullptr) && bytes_read > 0) {
        if (out.size() + bytes_read > cap) {
            overflow = true;
            continue;
        }
        out.append(buffer, bytes_read);
    }
}

[[noreturn]] void throw_last_error(const char* what) {
    throw std::runtime_error(std::string(what) + " failed: " + std::to_string(GetLastError()));
}

}  // namespace

SystemProcessRunner::SystemProcessRunner(std::size_t max_output_bytes) : max_output_bytes_(max_output_bytes) {}

ProcessResult SystemProcessRunner::run(const std::vector<std::string>& argv, int timeout_ms) {
    if (argv.empty() || !std::filesystem::path(argv.front()).is_absolute()) {
        throw std::invalid_argument("process argv[0] must be an absolute path");
    }
    if (timeout_ms <= 0) throw std::invalid_argument("process timeout must be positive");

    std::wstring executable = to_wide(argv.front());
    std::wstring command_line;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) command_line += L' ';
        command_line += quote_argument(to_wide(argv[i]));
    }

    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle stdin_nul;
    stdin_nul.reset(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
    if (stdin_nul.get() == INVALID_HANDLE_VALUE) throw_last_error("open NUL");

    Handle stdout_read;
    Handle stdout_write;
    Handle stderr_read;
    Handle stderr_write;
    HANDLE pipe_read = nullptr;
    HANDLE pipe_write = nullptr;
    if (!CreatePipe(&pipe_read, &pipe_write, &inheritable, 0)) throw_last_error("CreatePipe");
    stdout_read.reset(pipe_read);
    stdout_write.reset(pipe_write);
    if (!CreatePipe(&pipe_read, &pipe_write, &inheritable, 0)) throw_last_error("CreatePipe");
    stderr_read.reset(pipe_read);
    stderr_write.reset(pipe_write);
    SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0);

    Handle job;
    job.reset(CreateJobObjectW(nullptr, nullptr));
    if (!job.get()) throw_last_error("CreateJobObject");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits)) {
        throw_last_error("SetInformationJobObject");
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_nul.get();
    startup.hStdOutput = stdout_write.get();
    startup.hStdError = stderr_write.get();
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        throw_last_error("CreateProcessW");
    }
    Handle process_handle;
    process_handle.reset(process.hProcess);
    Handle thread_handle;
    thread_handle.reset(process.hThread);

    // The parent's copies of the write ends must close, or the readers never see end-of-file.
    stdout_write.reset();
    stderr_write.reset();
    stdin_nul.reset();
    if (!AssignProcessToJobObject(job.get(), process.hProcess)) {
        TerminateProcess(process.hProcess, 1);
        throw_last_error("AssignProcessToJobObject");
    }

    ProcessResult result;
    bool stdout_overflow = false;
    bool stderr_overflow = false;
    std::thread stdout_reader([&] { drain_pipe(stdout_read.get(), max_output_bytes_, result.stdout_text, stdout_overflow); });
    std::thread stderr_reader([&] { drain_pipe(stderr_read.get(), max_output_bytes_, result.stderr_text, stderr_overflow); });

    ResumeThread(process.hThread);
    if (WaitForSingleObject(process.hProcess, static_cast<DWORD>(timeout_ms)) == WAIT_TIMEOUT) {
        result.timed_out = true;
    }
    if (!result.timed_out) WaitForSingleObject(process.hProcess, INFINITE);

    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    // Kill any grandchild still holding a pipe, so the readers reach end-of-file.
    TerminateJobObject(job.get(), 1);
    stdout_reader.join();
    stderr_reader.join();

    result.exit_code = result.timed_out ? -1 : static_cast<int>(exit_code);
    result.output_overflow = stdout_overflow || stderr_overflow;
    return result;
}

#else

SystemProcessRunner::SystemProcessRunner(std::size_t max_output_bytes) : max_output_bytes_(max_output_bytes) {}

ProcessResult SystemProcessRunner::run(const std::vector<std::string>& argv, int timeout_ms) {
    (void)argv;
    (void)timeout_ms;
    throw std::runtime_error("SystemProcessRunner is implemented for Windows only in this build");
}

#endif

}  // namespace cipher::orchestration
