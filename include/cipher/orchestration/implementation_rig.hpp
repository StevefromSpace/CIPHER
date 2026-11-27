#ifndef CIPHER_ORCHESTRATION_IMPLEMENTATION_RIG_HPP
#define CIPHER_ORCHESTRATION_IMPLEMENTATION_RIG_HPP

#include "cipher/orchestration/git_worktree_backend.hpp"
#include "cipher/orchestration/gh_process_runner.hpp"
#include "cipher/orchestration/implementation_executor.hpp"
#include "cipher/orchestration/pipeline_roles.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace cipher::orchestration {

struct RigConfig {
    std::filesystem::path repository;      // repository the worktrees are created from (never edited directly)
    std::string base_sha;                  // full commit the worktrees start from
    std::filesystem::path worktree_root;   // folder holding job-<n> worktrees
    std::string model = "haiku";
    std::string max_budget_usd = "0.25";   // per claude call
};

// Production wiring for one issue: system process runner, git worktree backend, Claude-driven agent and the
// ImplementationExecutor. Roles other than Implementation escalate to the owner (RoleNotSupported).
// Throws if git or claude cannot be found on PATH. NOT verified against a live Claude; every agent step is paid.
class ImplementationRig final : public RoleExecutor {
public:
    ImplementationRig(const RigConfig& config, int issue, const std::string& task);
    RoleResult run(Role role, const JobRecord& job) override { return executor_->run(role, job); }

private:
    struct Escalator final : RoleExecutor {
        RoleResult run(Role, const JobRecord&) override;
    };
    SystemProcessRunner runner_;
    Escalator escalator_;
    std::unique_ptr<GitWorktreeBackend> backend_;
    std::unique_ptr<WorktreeAllocator> allocator_;
    std::unique_ptr<ClaudeToolAgent> agent_;
    std::unique_ptr<GhProcessRunner> gh_runner_;
    std::unique_ptr<GhAdapter> gh_;
    std::unique_ptr<PipelineExecutor> pipeline_;  // PR creation, QA, mechanical review; falls back to escalator_
    std::unique_ptr<ImplementationExecutor> executor_;
};

}  // namespace cipher::orchestration

#endif  // CIPHER_ORCHESTRATION_IMPLEMENTATION_RIG_HPP
