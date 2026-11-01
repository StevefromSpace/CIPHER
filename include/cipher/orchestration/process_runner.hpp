#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace cipher::orchestration {

inline constexpr std::size_t kDefaultMaxProcessOutputBytes = 1024 * 1024;

// Captured result of one child process. stdout/stderr may contain model text or account details: never log them.
struct ProcessResult {
    int exit_code = 0;
    bool timed_out = false;
    bool output_overflow = false;   // a stream exceeded the cap; a failure, never a partial success
    std::string stdout_text;
    std::string stderr_text;
};

// Provider-neutral process seam. Callers pass a closed argv; there is no shell and no string command.
class ProcessRunner {
public:
    virtual ~ProcessRunner() = default;
    virtual ProcessResult run(const std::vector<std::string>& argv, int timeout_ms) = 0;
};

// Launches argv[0] directly. argv[0] must be an absolute path, so the OS never resolves an executable from the
// working directory (job worktrees hold agent-written files). stdin is NUL. A timeout or output overflow kills the
// whole process tree through a Job Object. Implemented for Windows; other platforms throw std::runtime_error.
class SystemProcessRunner final : public ProcessRunner {
public:
    explicit SystemProcessRunner(std::size_t max_output_bytes = kDefaultMaxProcessOutputBytes);
    ProcessResult run(const std::vector<std::string>& argv, int timeout_ms) override;

private:
    std::size_t max_output_bytes_;
};

}  // namespace cipher::orchestration
