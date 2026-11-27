#ifndef CIPHER_ORCHESTRATION_PIPELINE_ROLES_HPP
#define CIPHER_ORCHESTRATION_PIPELINE_ROLES_HPP

#include "cipher/orchestration/github_adapter.hpp"
#include "cipher/orchestration/orchestrator.hpp"
#include "cipher/orchestration/process_runner.hpp"
#include "cipher/orchestration/worktree_allocator.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cipher::orchestration {

struct PipelineSettings {
    int issue_number = 0;
    std::string git_executable;                          // absolute path
    std::string base_branch = "main";
    std::string base_sha;                                // full commit the job started from
    std::string branch_prefix = "cipher/job-";           // PR head branch = prefix + issue number
    std::map<std::string, std::vector<std::string>> test_commands;  // approved QA commands: id -> fixed argv
    std::vector<std::string> qa_command_ids;             // run in order; all must exit 0
    std::filesystem::path evidence_file;                 // optional; findings are appended
    std::filesystem::path pr_body_file;                  // written by the executor before `gh pr create`
    int qa_timeout_ms = 600000;                          // per QA command; timeout or output overflow is BLOCKED
    std::size_t max_changed_files = 50;                  // reviewer: more than this is NEEDS_HUMAN
};

// Roles that follow implementation, all bounded and mechanical:
//   Implementation @ PullRequest : push the job's commit to <prefix><issue> and open (or reuse) exactly one PR.
//   Qa                           : check the diff, run the approved commands, report PASS/FAIL/BLOCKED as a verdict.
//   Reviewer                     : mechanical contract review (protected paths, secrets, test coverage, size).
// Every other role goes to `next`. Nothing here merges, edits files or trusts issue/PR text. The reviewer is NOT a
// semantic or independent security review: security-sensitive jobs always get NEEDS_HUMAN.
class PipelineExecutor final : public RoleExecutor {
public:
    PipelineExecutor(PipelineSettings settings, WorktreeAllocator& allocator, ProcessRunner& runner, GhAdapter& gh,
                     RoleExecutor& next);
    RoleResult run(Role role, const JobRecord& job) override;

private:
    RoleResult open_pull_request(const JobRecord& job);
    RoleResult run_qa(const JobRecord& job);
    RoleResult run_review(const JobRecord& job);

    ProcessResult git(const std::vector<std::string>& args);
    std::vector<std::string> changed_files();
    bool head_matches(const JobRecord& job);
    void note(const std::string& line) const;

    PipelineSettings settings_;
    WorktreeAllocator& allocator_;
    ProcessRunner& runner_;
    GhAdapter& gh_;
    RoleExecutor& next_;
};

// CI is observed on GitHub, not produced by a role. nullopt while checks are pending; otherwise a CiResult event
// (or Blocked when the PR is not open, or its head differs from the job's gated head).
std::optional<Event> observe_ci(const JobRecord& job, GhAdapter& gh, int pr_number);

}  // namespace cipher::orchestration

#endif  // CIPHER_ORCHESTRATION_PIPELINE_ROLES_HPP
