#pragma once

#include "cipher/orchestration/process_runner.hpp"
#include "cipher/orchestration/worktree_allocator.hpp"

#include <filesystem>
#include <string>

namespace cipher::orchestration {

// Returns true when root is absolute and no existing ancestor of it lies inside any git working tree. Runs
// `git rev-parse --show-toplevel` on the nearest existing ancestor; a worktree root inside a repository could
// overwrite that repository's checkouts, so it is refused. Fails closed: a git error counts as inside a repo.
bool is_safe_worktree_root(const std::filesystem::path& root, const std::string& git_executable, ProcessRunner& runner);

// Real `git worktree` backend. Every call is an argv vector with an absolute git path; there is no shell and no
// `--force`. Creation is detached at a full base SHA so no branch is created or later owed deletion.
class GitWorktreeBackend final : public WorktreeBackend {
public:
    GitWorktreeBackend(std::string git_executable, std::filesystem::path repository, std::string base_sha,
                       std::filesystem::path root, ProcessRunner& runner);

    void add(const std::filesystem::path& path, int issue_number) override;
    void remove(const std::filesystem::path& path, int issue_number) override;

private:
    void require_managed_path(const std::filesystem::path& path) const;

    std::string git_executable_;
    std::filesystem::path repository_;
    std::string base_sha_;
    std::filesystem::path root_;
    ProcessRunner& runner_;
};

}  // namespace cipher::orchestration
