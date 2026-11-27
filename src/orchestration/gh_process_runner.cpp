#include "cipher/orchestration/gh_process_runner.hpp"

#include <filesystem>
#include <stdexcept>
#include <utility>

namespace cipher::orchestration {

GhProcessRunner::GhProcessRunner(ProcessRunner& runner, std::string gh_executable, int timeout_ms,
                               std::filesystem::path working_dir)
    : runner_(runner), gh_executable_(std::move(gh_executable)), timeout_ms_(timeout_ms), working_dir_(std::move(working_dir)) {
    if (!std::filesystem::path(gh_executable_).is_absolute()) {
        throw std::invalid_argument("gh executable must be an absolute path");
    }
    if (timeout_ms_ <= 0) throw std::invalid_argument("gh timeout must be positive");
}

GhResult GhProcessRunner::run(const std::vector<std::string>& argv) {
    if (argv.empty() || argv.front() != "gh") throw std::invalid_argument("gh argv must start with \"gh\"");

    std::vector<std::string> resolved = argv;
    resolved.front() = gh_executable_;

    // Restores the previous cwd on every exit path, including a throwing runner.
    struct CwdGuard {
        std::filesystem::path previous;
        bool active = false;
        ~CwdGuard() {
            std::error_code ignored;
            if (active) std::filesystem::current_path(previous, ignored);
        }
    } guard;
    if (!working_dir_.empty()) {
        guard.previous = std::filesystem::current_path();
        std::filesystem::current_path(working_dir_);
        guard.active = true;
    }

    const ProcessResult process = runner_.run(resolved, timeout_ms_);
    GhResult result;
    // Timeout or overflow is never a success. The stderr text is dropped unread: it can echo request details.
    if (process.timed_out || process.output_overflow) {
        result.exit_code = -1;
        return result;
    }
    result.exit_code = process.exit_code;
    result.output = process.stdout_text;
    return result;
}

}  // namespace cipher::orchestration
