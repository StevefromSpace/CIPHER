#ifndef CIPHER_ORCHESTRATION_GH_PROCESS_RUNNER_HPP
#define CIPHER_ORCHESTRATION_GH_PROCESS_RUNNER_HPP

#include "cipher/orchestration/github_adapter.hpp"
#include "cipher/orchestration/process_runner.hpp"

#include <string>

namespace cipher::orchestration {

// Runs the typed `gh` argv from build_gh_argv through the provider-neutral ProcessRunner. The leading "gh" is
// replaced by one absolute path that was resolved before construction, so a planted gh in a job worktree never runs.
// A timeout or output overflow is reported as a non-zero exit, and only stdout is returned to the adapter.
class GhProcessRunner final : public GhRunner {
public:
    GhProcessRunner(ProcessRunner& runner, std::string gh_executable, int timeout_ms);
    GhResult run(const std::vector<std::string>& argv) override;

private:
    ProcessRunner& runner_;
    std::string gh_executable_;
    int timeout_ms_;
};

}  // namespace cipher::orchestration

#endif  // CIPHER_ORCHESTRATION_GH_PROCESS_RUNNER_HPP
