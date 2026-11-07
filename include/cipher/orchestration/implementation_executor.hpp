#ifndef CIPHER_ORCHESTRATION_IMPLEMENTATION_EXECUTOR_HPP
#define CIPHER_ORCHESTRATION_IMPLEMENTATION_EXECUTOR_HPP

#include "cipher/orchestration/orchestrator.hpp"
#include "cipher/orchestration/process_runner.hpp"
#include "cipher/orchestration/worktree_allocator.hpp"
#include "cipher/orchestration/worktree_tools.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace cipher::orchestration {

struct ToolTurn {
    ToolCall call;
    ToolOutcome outcome;
};

// What the model wants next: one tool call, or "done" with a short summary.
struct AgentStep {
    bool done = false;
    ToolCall call;
    std::string summary;
};

// Model seam. Implementations must not execute anything; they only propose. May throw on a malformed reply,
// which the executor treats as a failed execution (no retry here).
class ToolAgent {
public:
    virtual ~ToolAgent() = default;
    virtual AgentStep next(const std::string& task, const std::vector<ToolTurn>& transcript) = 0;
};

// Claude-backed agent: one tool-less `claude -p` call per step; the reply must be a single JSON object
//   {"tool":"fs.read","path":"..."}  or  {"done":true,"summary":"..."}.
// Unverified against a live model (tests use a fake runner); every call is paid, so the step count is bounded by the
// executor.
class ClaudeToolAgent final : public ToolAgent {
public:
    ClaudeToolAgent(ProcessRunner& runner, std::string claude_executable, std::string model,
                    std::string max_budget_usd, int timeout_ms);
    AgentStep next(const std::string& task, const std::vector<ToolTurn>& transcript) override;

private:
    ProcessRunner& runner_;
    std::string claude_executable_, model_, max_budget_usd_;
    int timeout_ms_;
};

// Parses one agent reply (pure, throws std::runtime_error on anything malformed).
AgentStep parse_agent_reply(const std::string& text);

struct ImplementationSettings {
    int issue_number = 0;
    std::string task;                                   // structured task text from the owner
    std::string git_executable;                         // absolute path
    std::map<std::string, std::vector<std::string>> commands;  // process.run ids -> fixed argv
    std::string verify_command_id;                      // optional: must exit 0 before commit
    std::string model_family;                           // reported to the orchestrator (reviewer independence)
    int max_steps = 20;
    std::filesystem::path evidence_file;                // optional; written after every execution
};

// Implementation role: allocates the job's isolated worktree, lets the agent drive WorktreeTools until it says done,
// verifies, commits inside the worktree and reports ImplementationComplete with the new HEAD. Every other role (and
// the PullRequest step, which needs a GitHub write that does not exist yet) is delegated to `fallback`.
// One execution = at most max_steps agent calls and no internal retry; the orchestrator owns the 2-execution limit.
class ImplementationExecutor final : public RoleExecutor {
public:
    ImplementationExecutor(ImplementationSettings settings, WorktreeAllocator& allocator, ProcessRunner& runner,
                           ToolAgent& agent, RoleExecutor& fallback);
    RoleResult run(Role role, const JobRecord& job) override;

private:
    RoleResult execute_implementation();

    ImplementationSettings settings_;
    WorktreeAllocator& allocator_;
    ProcessRunner& runner_;
    ToolAgent& agent_;
    RoleExecutor& fallback_;
};

}  // namespace cipher::orchestration

#endif  // CIPHER_ORCHESTRATION_IMPLEMENTATION_EXECUTOR_HPP
