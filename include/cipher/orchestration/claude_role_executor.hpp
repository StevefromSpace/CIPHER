#ifndef CIPHER_ORCHESTRATION_CLAUDE_ROLE_EXECUTOR_HPP
#define CIPHER_ORCHESTRATION_CLAUDE_ROLE_EXECUTOR_HPP

#include "cipher/orchestration/claude_executor.hpp"
#include "cipher/orchestration/orchestrator.hpp"
#include "cipher/orchestration/process_runner.hpp"

#include <cstddef>
#include <string>

namespace cipher::orchestration {

// Stage 4: binds the provider-neutral RoleExecutor to one tool-less Claude call. Only the text roles Research and
// Architecture run here, because the call cannot read the repository, edit files, push, or see a diff. Every other
// role escalates without a process call: no fabricated implementation, QA, review or security verdict.
class ClaudeRoleExecutor final : public RoleExecutor {
public:
    ClaudeRoleExecutor(ProcessRunner& runner, int issue_number, std::string claude_executable, std::string model,
                       std::string max_budget_usd, int timeout_ms);
    RoleResult run(Role role, const JobRecord& job) override;

private:
    ProcessRunner& runner_;
    int issue_number_;
    std::string claude_executable_;
    std::string model_;
    std::string max_budget_usd_;
    int timeout_ms_;
};

// Fixed text for a role. Only the issue number is interpolated, so the prompt length is bounded by construction.
std::string build_role_prompt(Role role, int issue_number);

// Windows CreateProcess command line limit (32,767 characters). The prompt is one argv element, so it must fit.
inline constexpr std::size_t kMaxRolePromptChars = 4000;

}  // namespace cipher::orchestration

#endif  // CIPHER_ORCHESTRATION_CLAUDE_ROLE_EXECUTOR_HPP
