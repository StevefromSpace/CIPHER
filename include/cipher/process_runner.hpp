#ifndef CIPHER_PROCESS_RUNNER_HPP
#define CIPHER_PROCESS_RUNNER_HPP

// ---------------------------------------------------------------------------
// cipher/process_runner.hpp  —  bounded, shell-free child process execution
//
// Invariants:
//   - The executable is launched directly with a distinct argument vector.
//     No shell is involved, so arguments are never interpreted.
//   - Input text is delivered over stdin, never as an argument.
//   - stdout and stderr are bounded independently. Overflow is reported, and
//     the caller must discard the partial output.
//   - On timeout or overflow the child and its descendants are terminated
//     (Windows: Job Object; POSIX: process group).
// ---------------------------------------------------------------------------

#include <cstddef>
#include <string>
#include <vector>

namespace cipher {

struct ProcessRequest {
    std::string executable;             // Absolute path, resolved by the caller
    std::vector<std::string> arguments; // Fixed argv; never shell-interpreted
    std::string stdin_text;             // Delivered over stdin, not argv
    std::string working_directory;      // Empty: inherit the caller's directory
    double timeout_seconds{30.0};
    std::size_t max_stdout_bytes{262144};
    std::size_t max_stderr_bytes{16384};
};

struct ProcessResult {
    bool started{false};
    bool timed_out{false};
    bool output_overflow{false}; // stdout or stderr exceeded its limit
    int exit_code{-1};
    std::string stdout_text;
    std::string stderr_text;
    std::string error;           // Set only when started is false
};

class IProcessRunner {
public:
    virtual ~IProcessRunner() = default;
    virtual ProcessResult run(const ProcessRequest& request) = 0;
};

class SystemProcessRunner : public IProcessRunner {
public:
    ProcessResult run(const ProcessRequest& request) override;
};

} // namespace cipher

#endif // CIPHER_PROCESS_RUNNER_HPP
