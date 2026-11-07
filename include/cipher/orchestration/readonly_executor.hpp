#ifndef CIPHER_ORCHESTRATION_READONLY_EXECUTOR_HPP
#define CIPHER_ORCHESTRATION_READONLY_EXECUTOR_HPP

#include "cipher/orchestration/implementation_executor.hpp"

#include <filesystem>
#include <string>

namespace cipher::orchestration {

struct ReadOnlySettings {
    int issue_number = 0;
    std::string task;
    std::string git_executable;
    std::string model_family;
    int max_steps = 10;
    std::filesystem::path evidence_file;  // optional; findings summary is appended here
};

// Research / Architecture roles: the agent may only read the job's worktree (fs.read, fs.list, fs.search, git.status,
// git.diff, git.log). Any other tool request is refused by this executor before WorktreeTools is called, and no
// process commands are registered, so nothing can be written or run. Other roles go to `fallback`.
// A malformed reply, a refused tool or the step limit gives Blocked; there is no retry here.
// The findings summary is only written to the evidence file; the JobRecord has no field to carry it (known limit).
class ReadOnlyExecutor final : public RoleExecutor {
public:
    ReadOnlyExecutor(ReadOnlySettings settings, WorktreeAllocator& allocator, ProcessRunner& runner, ToolAgent& agent,
                     RoleExecutor& fallback);
    RoleResult run(Role role, const JobRecord& job) override;

private:
    ReadOnlySettings settings_;
    WorktreeAllocator& allocator_;
    ProcessRunner& runner_;
    ToolAgent& agent_;
    RoleExecutor& fallback_;
};

}  // namespace cipher::orchestration

#endif  // CIPHER_ORCHESTRATION_READONLY_EXECUTOR_HPP
