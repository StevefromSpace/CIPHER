#include "cipher/orchestration/gh_process_runner.hpp"

#include <filesystem>
#include <stdexcept>
#include <utility>

namespace cipher::orchestration {

GhProcessRunner::GhProcessRunner(ProcessRunner& runner, std::string gh_executable, int timeout_ms)
    : runner_(runner), gh_executable_(std::move(gh_executable)), timeout_ms_(timeout_ms) {
    if (!std::filesystem::path(gh_executable_).is_absolute()) {
        throw std::invalid_argument("gh executable must be an absolute path");
    }
    if (timeout_ms_ <= 0) throw std::invalid_argument("gh timeout must be positive");
}

GhResult GhProcessRunner::run(const std::vector<std::string>& argv) {
    if (argv.empty() || argv.front() != "gh") throw std::invalid_argument("gh argv must start with \"gh\"");

    std::vector<std::string> resolved = argv;
    resolved.front() = gh_executable_;

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
