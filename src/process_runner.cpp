#include "cipher/process_runner.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <functional>
#include <thread>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

namespace cipher {

namespace {

constexpr std::size_t READ_CHUNK_BYTES = 4096;
constexpr std::size_t WRITE_CHUNK_BYTES = 4096; // <= PIPE_BUF, so a POLLOUT-gated write never blocks
constexpr int POLL_MAX_WAIT_MS = 100;
constexpr int REAP_INTERVAL_MS = 10;

} // anonymous namespace

#if defined(_WIN32)

// ---------------------------------------------------------------------------
// Windows: CreateProcessW inside a Job Object, with threads for pipe I/O
// ---------------------------------------------------------------------------
namespace {

class ScopedHandle {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
    ~ScopedHandle() { reset(); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const { return handle_; }
    void reset(HANDLE handle = nullptr) {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = handle;
    }
    HANDLE release() {
        HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }

private:
    HANDLE handle_{nullptr};
};

struct Pipe {
    ScopedHandle read_end;
    ScopedHandle write_end;
};

std::wstring to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

// MSVCRT argument quoting: backslashes are doubled only before a quote or at the end.
std::wstring quote_argument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        quoted.append(backslashes * 2 + (character == L'"' ? 1 : 0), L'\\');
        quoted.push_back(character);
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring build_command_line(const ProcessRequest& request) {
    std::wstring line = quote_argument(to_wide(request.executable));
    for (const auto& argument : request.arguments) {
        line += L' ';
        line += quote_argument(to_wide(argument));
    }
    return line;
}

bool create_inheritable_pipe(Pipe& pipe) {
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if (!CreatePipe(&read_handle, &write_handle, &attributes, 0)) return false;
    pipe.read_end.reset(read_handle);
    pipe.write_end.reset(write_handle);
    return true;
}

void write_stdin(HANDLE pipe, const std::string& text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min(text.size() - offset, WRITE_CHUNK_BYTES));
        DWORD written = 0;
        if (!WriteFile(pipe, text.data() + offset, chunk, &written, nullptr) || written == 0) break;
        offset += written;
    }
    CloseHandle(pipe); // Signals end of input to the child
}

void read_stream(HANDLE pipe, std::size_t limit, HANDLE job, std::string& out, std::atomic<bool>& overflow) {
    char buffer[READ_CHUNK_BYTES];
    DWORD received = 0;
    while (ReadFile(pipe, buffer, sizeof(buffer), &received, nullptr) && received > 0) {
        if (out.size() + received > limit) {
            overflow = true;
            TerminateJobObject(job, 1); // Wakes the waiting caller
            return;
        }
        out.append(buffer, received);
    }
}

// Starts the child suspended inside the job so it cannot spawn outside it.
bool start_in_job(const ProcessRequest& request, Pipe& in, Pipe& out, Pipe& err,
                  HANDLE job, ScopedHandle& process, std::string& error) {
    // Child-side ends must stay inheritable; parent-side ends must not leak into the child.
    SetHandleInformation(in.write_end.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out.read_end.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err.read_end.get(), HANDLE_FLAG_INHERIT, 0);

    const std::wstring working_directory = to_wide(request.working_directory);
    std::wstring command_line = build_command_line(request);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = in.read_end.get();
    startup.hStdOutput = out.write_end.get();
    startup.hStdError = err.write_end.get();
    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(
        to_wide(request.executable).c_str(), command_line.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
        working_directory.empty() ? nullptr : working_directory.c_str(), &startup, &info);
    // The child owns the inheritable ends now; the parent keeps only its own.
    in.read_end.reset();
    out.write_end.reset();
    err.write_end.reset();
    if (!created) {
        error = "CreateProcess failed (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    ScopedHandle thread(info.hThread);
    process.reset(info.hProcess);
    if (!AssignProcessToJobObject(job, info.hProcess)) {
        TerminateProcess(info.hProcess, 1);
        error = "could not place child in job object";
        return false;
    }
    ResumeThread(info.hThread);
    return true;
}

} // anonymous namespace

ProcessResult SystemProcessRunner::run(const ProcessRequest& request) {
    ProcessResult result;
    Pipe in, out, err;
    if (!create_inheritable_pipe(in) || !create_inheritable_pipe(out) || !create_inheritable_pipe(err)) {
        result.error = "could not create process pipes";
        return result;
    }
    ScopedHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.get()) {
        result.error = "could not create job object";
        return result;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    ScopedHandle process;
    if (!start_in_job(request, in, out, err, job.get(), process, result.error)) {
        return result;
    }
    result.started = true;

    std::atomic<bool> overflow{false};
    std::thread stdin_thread(write_stdin, in.write_end.release(), std::cref(request.stdin_text));
    std::thread stdout_thread(read_stream, out.read_end.get(), request.max_stdout_bytes, job.get(),
                              std::ref(result.stdout_text), std::ref(overflow));
    std::thread stderr_thread(read_stream, err.read_end.get(), request.max_stderr_bytes, job.get(),
                              std::ref(result.stderr_text), std::ref(overflow));

    const DWORD timeout_ms = static_cast<DWORD>(request.timeout_seconds * 1000.0);
    if (WaitForSingleObject(process.get(), timeout_ms) == WAIT_TIMEOUT) {
        result.timed_out = true;
    }
    TerminateJobObject(job.get(), 1); // Ends any remaining descendants
    WaitForSingleObject(process.get(), INFINITE);

    DWORD exit_code = 0;
    GetExitCodeProcess(process.get(), &exit_code);
    result.exit_code = static_cast<int>(exit_code);
    result.output_overflow = overflow.load();

    stdin_thread.join();
    stdout_thread.join();
    stderr_thread.join();
    return result;
}

#else

// ---------------------------------------------------------------------------
// POSIX: fork/execv in a fresh process group, poll() for bounded I/O
// ---------------------------------------------------------------------------
namespace {

class ScopedFd {
public:
    ScopedFd() = default;
    explicit ScopedFd(int fd) : fd_(fd) {}
    ~ScopedFd() { reset(); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    int get() const { return fd_; }
    void reset(int fd = -1) {
        if (fd_ >= 0) close(fd_);
        fd_ = fd;
    }

private:
    int fd_{-1};
};

struct Pipe {
    ScopedFd read_end;
    ScopedFd write_end;
};

// Close-on-exec keeps parent-side ends out of the child; dup2 clears it for 0/1/2.
bool create_pipe(Pipe& pipe) {
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) return false;
    pipe.read_end.reset(fds[0]);
    pipe.write_end.reset(fds[1]);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
}

// Writing to a pipe whose reader exited raises SIGPIPE, which would kill CIPHER.
// The previous disposition is restored when the run finishes.
class IgnoreSigpipe {
public:
    IgnoreSigpipe() {
        struct sigaction ignore{};
        ignore.sa_handler = SIG_IGN;
        sigaction(SIGPIPE, &ignore, &previous_);
    }
    ~IgnoreSigpipe() { sigaction(SIGPIPE, &previous_, nullptr); }
    IgnoreSigpipe(const IgnoreSigpipe&) = delete;
    IgnoreSigpipe& operator=(const IgnoreSigpipe&) = delete;

private:
    struct sigaction previous_ {};
};

// Waits for the child until it exits or the run is over. Kills it when the run is over.
void reap_child(pid_t pid, std::chrono::steady_clock::time_point deadline, ProcessResult& result, int& status) {
    using Clock = std::chrono::steady_clock;
    while (waitpid(pid, &status, WNOHANG) != pid) {
        if (result.output_overflow) {
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            return;
        }
        if (result.timed_out || Clock::now() >= deadline) {
            result.timed_out = true;
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(REAP_INTERVAL_MS));
    }
}

// Runs in the forked child. Only async-signal-safe calls are allowed here.
[[noreturn]] void exec_child(const ProcessRequest& request, std::vector<char*>& argv,
                             Pipe& in, Pipe& out, Pipe& err, Pipe& exec_report) {
    setpgid(0, 0);
    int failure = 0;
    if (!request.working_directory.empty() && chdir(request.working_directory.c_str()) != 0) {
        failure = errno;
    } else {
        dup2(in.read_end.get(), STDIN_FILENO);
        dup2(out.write_end.get(), STDOUT_FILENO);
        dup2(err.write_end.get(), STDERR_FILENO);
        execv(request.executable.c_str(), argv.data());
        failure = errno;
    }
    ssize_t reported = write(exec_report.write_end.get(), &failure, sizeof(failure));
    static_cast<void>(reported);
    _exit(127);
}

// Returns true when the child exec'd. On failure, `error` says why.
bool await_exec(Pipe& exec_report, pid_t pid, std::string& error) {
    int exec_errno = 0;
    if (read(exec_report.read_end.get(), &exec_errno, sizeof(exec_errno)) != sizeof(exec_errno)) {
        return true; // EOF with no report: exec succeeded (close-on-exec fired)
    }
    int status = 0;
    waitpid(pid, &status, 0);
    error = std::string("could not start process: ") + std::strerror(exec_errno);
    return false;
}

struct Stream {
    int fd{-1};
    std::string* out{nullptr};
    std::size_t limit{0};
    bool open{true};
};

// Reads one chunk from a ready stream. Returns false when the stream overflowed.
bool read_chunk(Stream& stream) {
    char buffer[READ_CHUNK_BYTES];
    const ssize_t received = read(stream.fd, buffer, sizeof(buffer));
    if (received <= 0) {
        stream.open = false;
        return true;
    }
    if (stream.out->size() + static_cast<std::size_t>(received) > stream.limit) return false;
    stream.out->append(buffer, static_cast<std::size_t>(received));
    return true;
}

} // anonymous namespace

ProcessResult SystemProcessRunner::run(const ProcessRequest& request) {
    ProcessResult result;
    const IgnoreSigpipe sigpipe_guard;
    Pipe in, out, err, exec_report;
    if (!create_pipe(in) || !create_pipe(out) || !create_pipe(err) || !create_pipe(exec_report)) {
        result.error = "could not create process pipes";
        return result;
    }

    // Built before fork(): the child must not allocate.
    std::vector<std::string> argv_storage{request.executable};
    argv_storage.insert(argv_storage.end(), request.arguments.begin(), request.arguments.end());
    std::vector<char*> argv;
    for (auto& argument : argv_storage) argv.push_back(argument.data());
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        result.error = std::string("fork failed: ") + std::strerror(errno);
        return result;
    }
    if (pid == 0) exec_child(request, argv, in, out, err, exec_report);

    setpgid(pid, pid); // Mirrors the child's own call to close the race
    in.read_end.reset();
    out.write_end.reset();
    err.write_end.reset();
    exec_report.write_end.reset();
    if (!await_exec(exec_report, pid, result.error)) return result;
    result.started = true;

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(request.timeout_seconds));

    std::size_t stdin_offset = 0;
    if (request.stdin_text.empty()) in.write_end.reset();
    Stream stdout_stream{out.read_end.get(), &result.stdout_text, request.max_stdout_bytes, true};
    Stream stderr_stream{err.read_end.get(), &result.stderr_text, request.max_stderr_bytes, true};

    bool output_done = false;
    while (!output_done && !result.timed_out && !result.output_overflow) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0) {
            result.timed_out = true;
            break;
        }
        // Negative descriptors are ignored by poll(), so closed streams need no special case.
        pollfd fds[3] = {{in.write_end.get(), POLLOUT, 0},
                         {stdout_stream.open ? stdout_stream.fd : -1, POLLIN, 0},
                         {stderr_stream.open ? stderr_stream.fd : -1, POLLIN, 0}};
        const int wait_ms = static_cast<int>(std::min<long long>(remaining.count(), POLL_MAX_WAIT_MS));
        if (poll(fds, 3, wait_ms) < 0 && errno != EINTR) break;

        if (fds[0].fd >= 0 && (fds[0].revents & (POLLOUT | POLLERR | POLLHUP))) {
            const std::size_t left = request.stdin_text.size() - stdin_offset;
            const ssize_t written = write(in.write_end.get(), request.stdin_text.data() + stdin_offset,
                                          std::min(left, WRITE_CHUNK_BYTES));
            if (written > 0) stdin_offset += static_cast<std::size_t>(written);
            if (written < 0 || stdin_offset >= request.stdin_text.size()) in.write_end.reset();
        }
        for (Stream* stream : {&stdout_stream, &stderr_stream}) {
            if (stream->open && (fds[stream == &stdout_stream ? 1 : 2].revents & (POLLIN | POLLHUP | POLLERR))) {
                if (!read_chunk(*stream)) result.output_overflow = true;
            }
        }
        output_done = !stdout_stream.open && !stderr_stream.open;
    }

    int status = 0;
    reap_child(pid, deadline, result, status);
    kill(-pid, SIGKILL); // Descendants that outlived the child; ESRCH once none remain
    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    return result;
}

#endif

} // namespace cipher
