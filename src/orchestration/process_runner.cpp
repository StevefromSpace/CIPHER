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

#ifndef _WIN32
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>

// Passed to posix_spawn unchanged, so the child sees the same environment as this process.
extern char** environ;
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

namespace {

constexpr int kPollSliceMs = 10;      // longest single wait before the deadline and child exit are checked again
constexpr int kDrainGraceMs = 1000;   // pipes still open after the group is killed count as a failure

[[noreturn]] void throw_error(const char* what, int code) {
    throw std::runtime_error(std::string(what) + " failed: " + std::strerror(code));
}

// Owns one file descriptor. Negative values are never closed.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Fd& operator=(Fd&& other) noexcept {
        reset(other.fd_);
        other.fd_ = -1;
        return *this;
    }
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    int get() const { return fd_; }
    void reset(int fd = -1) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

// One captured stream: the read end, where its bytes go, and whether anything was discarded past the cap.
struct Stream {
    Fd fd;
    std::string* text = nullptr;
    bool overflow = false;
};

// Appends what fits under the cap. Past the cap the bytes are discarded, but the stream keeps draining.
void absorb(Stream& stream, const char* data, std::size_t size, std::size_t cap) {
    const std::size_t room = stream.text->size() < cap ? cap - stream.text->size() : 0;
    stream.text->append(data, std::min(size, room));
    if (size > room) stream.overflow = true;
}

// Reads one chunk from a stream that poll reported ready. EOF closes the stream.
void read_chunk(Stream& stream, std::size_t cap) {
    char buffer[4096];
    const ssize_t n = ::read(stream.fd.get(), buffer, sizeof buffer);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) return;
        throw_error("process output read", errno);
    }
    if (n == 0) {
        stream.fd.reset();
        return;
    }
    absorb(stream, buffer, static_cast<std::size_t>(n), cap);
}

// Waits up to timeout_ms for output on either open stream, then reads what is ready. Returns true while one is open.
bool pump(Stream (&streams)[2], std::size_t cap, int timeout_ms) {
    pollfd fds[2];
    bool any_open = false;
    for (std::size_t i = 0; i < 2; ++i) {
        fds[i].fd = streams[i].fd.get();   // poll ignores negative fds, so closed streams drop out
        fds[i].events = POLLIN;
        fds[i].revents = 0;
        any_open = any_open || streams[i].fd.get() >= 0;
    }
    if (!any_open) {
        poll(nullptr, 0, timeout_ms);   // plain sleep
        return false;
    }
    if (poll(fds, 2, timeout_ms) < 0) {
        if (errno != EINTR) throw_error("process poll", errno);
        return true;
    }
    for (std::size_t i = 0; i < 2; ++i) {
        if (fds[i].revents != 0 && streams[i].fd.get() >= 0) read_chunk(streams[i], cap);
    }
    return streams[0].fd.get() >= 0 || streams[1].fd.get() >= 0;
}

// Starts argv with stdin on /dev/null and stdout/stderr on the given pipes. The child leads a new process group, so a
// timeout can kill grandchildren that inherited the pipes. Exec failure is reported here, not as a silent exit.
pid_t spawn_child(const std::vector<std::string>& argv, int stdout_fd, int stderr_fd) {
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const std::string& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if (const int rc = posix_spawn_file_actions_init(&actions)) throw_error("process file actions", rc);
    posix_spawnattr_t attributes;
    if (const int rc = posix_spawnattr_init(&attributes)) {
        posix_spawn_file_actions_destroy(&actions);
        throw_error("process attributes", rc);
    }

    pid_t pid = -1;
    int rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (rc == 0) rc = posix_spawn_file_actions_adddup2(&actions, stdout_fd, STDOUT_FILENO);
    if (rc == 0) rc = posix_spawn_file_actions_adddup2(&actions, stderr_fd, STDERR_FILENO);
    if (rc == 0) rc = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    if (rc == 0) rc = posix_spawnattr_setpgroup(&attributes, 0);   // 0 makes the child its own group leader
    if (rc == 0) rc = posix_spawn(&pid, argv.front().c_str(), &actions, &attributes, args.data(), environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) throw_error("process start", rc);
    return pid;
}

// Owns the child's process group. The destructor kills and reaps it on any early exit, so no path leaves an orphan.
class ChildProcess {
public:
    explicit ChildProcess(pid_t pid) : pid_(pid) {}
    ~ChildProcess() {
        if (pid_ > 0) {
            kill_group();
            wait_status();
        }
    }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    pid_t pid() const { return pid_; }

    // SIGKILL to the whole group. A zombie leader keeps its pid and group id reserved until it is reaped.
    void kill_group() const { killpg(pid_, SIGKILL); }   // ESRCH after a normal exit is expected

    int wait_status() {
        int status = 0;
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
        return status;
    }

private:
    pid_t pid_;
};

// True once the child has exited but before it is reaped, so its pid and group id stay reserved for the kill.
bool child_exited(pid_t pid) {
    siginfo_t info{};
    if (waitid(P_PID, pid, &info, WEXITED | WNOHANG | WNOWAIT) != 0) {
        if (errno == EINTR) return false;
        throw_error("process wait", errno);
    }
    return info.si_pid == pid;
}

int exit_code_from_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);   // shell convention: a signal death is non-zero
    return -1;
}

}  // namespace

SystemProcessRunner::SystemProcessRunner(std::size_t max_output_bytes) : max_output_bytes_(max_output_bytes) {}

ProcessResult SystemProcessRunner::run(const std::vector<std::string>& argv, int timeout_ms) {
    if (argv.empty() || !std::filesystem::path(argv.front()).is_absolute())
        throw std::invalid_argument("process argv[0] must be an absolute path");
    if (timeout_ms <= 0) throw std::invalid_argument("process timeout must be positive");

    int out_pipe[2];
    if (pipe2(out_pipe, O_CLOEXEC) != 0) throw_error("process stdout pipe", errno);
    Stream streams[2];
    streams[0].fd = Fd(out_pipe[0]);
    Fd out_write(out_pipe[1]);
    int err_pipe[2];
    if (pipe2(err_pipe, O_CLOEXEC) != 0) throw_error("process stderr pipe", errno);
    streams[1].fd = Fd(err_pipe[0]);
    Fd err_write(err_pipe[1]);

    ChildProcess child(spawn_child(argv, out_write.get(), err_write.get()));
    // The parent keeps only read ends, so EOF arrives once every write end in the group has closed.
    out_write.reset();
    err_write.reset();

    ProcessResult result;
    streams[0].text = &result.stdout_text;
    streams[1].text = &result.stderr_text;
    const std::size_t cap = max_output_bytes_;

    // Output is drained while waiting, so a child that fills a pipe cannot block forever and look like a hang.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!child_exited(child.pid())) {
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            break;
        }
        pump(streams, cap, kPollSliceMs);
    }

    child.kill_group();   // grandchildren that inherited the pipes die with the leader
    const auto drain_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(kDrainGraceMs);
    while (pump(streams, cap, kPollSliceMs) && std::chrono::steady_clock::now() < drain_end) {
    }
    // Pipes still open after the group is dead means something escaped it. Treat that as a failure, not a partial success.
    if (streams[0].fd.get() >= 0 || streams[1].fd.get() >= 0) result.timed_out = true;

    const int status = child.wait_status();
    result.exit_code = result.timed_out ? -1 : exit_code_from_status(status);
    result.output_overflow = streams[0].overflow || streams[1].overflow;
    return result;
}

#endif

}  // namespace cipher::orchestration
