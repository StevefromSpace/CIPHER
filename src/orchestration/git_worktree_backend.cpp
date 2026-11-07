#include "cipher/orchestration/git_worktree_backend.hpp"

#include "cipher/orchestration/commit_sha.hpp"

#include <stdexcept>

namespace cipher::orchestration {
namespace fs = std::filesystem;
namespace {

constexpr int kGitQueryTimeoutMs = 15000;
constexpr int kGitWorktreeTimeoutMs = 120000;

// git prints this for a directory outside every repository. Any other failure is treated as inside one.
constexpr const char* kNotARepositoryMessage = "not a git repository";

// Nearest ancestor of path (or path itself) that exists on disk; empty when none does.
fs::path nearest_existing(fs::path path) {
    while (!path.empty() && !fs::exists(path)) {
        const fs::path parent = path.parent_path();
        if (parent == path) return {};
        path = parent;
    }
    return path;
}

}  // namespace

bool is_safe_worktree_root(const fs::path& root, const std::string& git_executable, ProcessRunner& runner) {
    if (!root.is_absolute()) return false;
    const fs::path ancestor = nearest_existing(root);
    if (ancestor.empty()) return false;

    const ProcessResult result =
        runner.run({git_executable, "-C", ancestor.string(), "rev-parse", "--show-toplevel"}, kGitQueryTimeoutMs);
    if (result.timed_out || result.output_overflow) return false;
    if (result.exit_code == 0) return false;  // inside a working tree: refused
    // Only an explicit "not a repository" answer is safe. stderr is read here for classification and never logged.
    return result.stderr_text.find(kNotARepositoryMessage) != std::string::npos;
}

GitWorktreeBackend::GitWorktreeBackend(std::string git_executable, fs::path repository, std::string base_sha,
                                       fs::path root, ProcessRunner& runner)
    : git_executable_(std::move(git_executable)),
      repository_(std::move(repository)),
      base_sha_(std::move(base_sha)),
      root_(std::move(root)),
      runner_(runner) {
    if (!is_full_sha(base_sha_)) throw std::invalid_argument("worktree base must be a full lowercase commit SHA");
    if (!is_safe_worktree_root(root_, git_executable_, runner_)) {
        throw std::runtime_error("worktree root is not allowed: it must be absolute and outside every git working tree");
    }
}

// Accepts only <root>/job-<n> with the canonical root. Rejects traversal, sibling paths and other names.
void GitWorktreeBackend::require_managed_path(const fs::path& path) const {
    const fs::path canonical_root = fs::weakly_canonical(root_);
    const fs::path canonical_parent = fs::weakly_canonical(path).parent_path();
    const std::string name = path.filename().string();
    if (canonical_parent != canonical_root || name.rfind("job-", 0) != 0 || name.size() <= 4) {
        throw std::runtime_error("worktree path is outside the managed root");
    }
}

void GitWorktreeBackend::add(const fs::path& path, int issue_number) {
    require_managed_path(path);
    const ProcessResult result = runner_.run(
        {git_executable_, "-C", repository_.string(), "worktree", "add", "--detach", path.string(), base_sha_},
        kGitWorktreeTimeoutMs);
    if (result.timed_out || result.output_overflow || result.exit_code != 0) {
        throw std::runtime_error("git worktree add failed for job-" + std::to_string(issue_number));
    }
}

// No --force: git refuses a dirty or unregistered tree, so a managed record never deletes uncommitted work.
void GitWorktreeBackend::remove(const fs::path& path, int issue_number) {
    require_managed_path(path);
    const ProcessResult result = runner_.run(
        {git_executable_, "-C", repository_.string(), "worktree", "remove", path.string()}, kGitWorktreeTimeoutMs);
    if (result.timed_out || result.output_overflow || result.exit_code != 0) {
        throw std::runtime_error("git worktree remove refused for job-" + std::to_string(issue_number));
    }
}

}  // namespace cipher::orchestration
