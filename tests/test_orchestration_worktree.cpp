// Git worktree backend tests against a real temporary repository. Never points at the CIPHER checkout.

#include "cipher/orchestration/executable_resolver.hpp"
#include "cipher/orchestration/git_worktree_backend.hpp"
#include "cipher/orchestration/process_runner.hpp"
#include "cipher/orchestration/worktree_allocator.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace cipher::orchestration;
namespace fs = std::filesystem;

int failures = 0;

void check(bool ok, const char* what) {
    std::cout << (ok ? "PASS: " : "FAIL: ") << what << "\n";
    if (!ok) ++failures;
}

template <typename Error = std::exception>
bool throws(const std::function<void()>& action) {
    try {
        action();
    } catch (const Error&) {
        return true;
    }
    return false;
}

std::string git_executable() { return resolve_executable_on_path("git"); }

// Runs git against a directory and returns stdout with trailing newlines removed.
std::string git_output(SystemProcessRunner& runner, const std::string& git, const fs::path& directory,
                       const std::vector<std::string>& args) {
    std::vector<std::string> argv{git, "-C", directory.string()};
    argv.insert(argv.end(), args.begin(), args.end());
    ProcessResult result = runner.run(argv, 30000);
    std::string text = result.stdout_text;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return result.exit_code == 0 ? text : std::string("<git failed>");
}

// Creates a repository with one commit. Returns the base commit SHA.
std::string make_repository(SystemProcessRunner& runner, const std::string& git, const fs::path& repo) {
    fs::create_directories(repo);
    runner.run({git, "-C", repo.string(), "init", "-q"}, 30000);
    std::ofstream(repo / "README.txt") << "probe\n";
    runner.run({git, "-C", repo.string(), "add", "README.txt"}, 30000);
    runner.run({git, "-C", repo.string(), "-c", "user.name=cipher-test", "-c", "user.email=test@example.invalid",
                "commit", "-q", "-m", "base"},
               30000);
    return git_output(runner, git, repo, {"rev-parse", "HEAD"});
}

fs::path fresh_base(const std::string& name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path base = fs::temp_directory_path() / ("cipher wt test " + name + "-" + std::to_string(stamp));
    fs::create_directories(base);
    return base;
}

void test_creates_detached_worktree_at_base(SystemProcessRunner& runner, const std::string& git) {
    const fs::path base = fresh_base("create");
    const std::string sha = make_repository(runner, git, base / "repo");
    GitWorktreeBackend backend(git, base / "repo", sha, base / "worktrees", runner);
    WorktreeAllocator allocator(base / "worktrees", backend);

    check(allocator.allocate(7) == WorktreeOutcome::Created, "worktree: allocate creates a managed worktree");
    check(git_output(runner, git, base / "worktrees" / "job-7", {"rev-parse", "HEAD"}) == sha,
          "worktree: created tree is detached at the base SHA");
    check(allocator.allocate(7) == WorktreeOutcome::Reused, "worktree: the same job reuses its recorded worktree");
    fs::remove_all(base);
}

void test_rejects_invalid_job_ids(SystemProcessRunner& runner, const std::string& git) {
    const fs::path base = fresh_base("ids");
    const std::string sha = make_repository(runner, git, base / "repo");
    GitWorktreeBackend backend(git, base / "repo", sha, base / "worktrees", runner);
    WorktreeAllocator allocator(base / "worktrees", backend);

    check(throws<std::invalid_argument>([&] { allocator.allocate(0); }), "worktree: job id 0 is rejected");
    check(throws<std::invalid_argument>([&] { allocator.allocate(-3); }), "worktree: a negative job id is rejected");
    fs::remove_all(base);
}

void test_rejects_traversal_and_root_escape(SystemProcessRunner& runner, const std::string& git) {
    const fs::path base = fresh_base("escape");
    const std::string sha = make_repository(runner, git, base / "repo");
    const fs::path root = base / "worktrees";
    GitWorktreeBackend backend(git, base / "repo", sha, root, runner);
    fs::create_directories(root);

    check(throws<std::runtime_error>([&] { backend.add(root / ".." / "job-9", 9); }),
          "worktree: a traversal path that escapes the root is rejected");
    check(throws<std::runtime_error>([&] { backend.add(base / "elsewhere" / "job-9", 9); }),
          "worktree: a sibling path outside the root is rejected");
    check(throws<std::runtime_error>([&] { backend.add(root / "notes", 9); }),
          "worktree: a non-job name inside the root is rejected");
    fs::remove_all(base);
}

void test_refuses_protected_roots(SystemProcessRunner& runner, const std::string& git) {
    const fs::path base = fresh_base("protected");
    const std::string sha = make_repository(runner, git, base / "repo");

    check(throws<std::runtime_error>([&] {
              GitWorktreeBackend bad(git, base / "repo", sha, base / "repo" / "worktrees", runner);
          }),
          "worktree: a root inside the repository (owner checkout) is refused");
    check(throws<std::runtime_error>([&] {
              GitWorktreeBackend bad(git, base / "repo", sha, base / "repo", runner);
          }),
          "worktree: the repository root itself is refused");
    check(throws<std::runtime_error>([&] {
              GitWorktreeBackend bad(git, base / "repo", sha, fs::path("relative") / "worktrees", runner);
          }),
          "worktree: a relative root is refused");
    check(throws<std::invalid_argument>([&] {
              GitWorktreeBackend bad(git, base / "repo", "abc", base / "worktrees", runner);
          }),
          "worktree: a short base SHA is refused");
    fs::remove_all(base);
}

void test_removes_managed_worktree(SystemProcessRunner& runner, const std::string& git) {
    const fs::path base = fresh_base("remove");
    const std::string sha = make_repository(runner, git, base / "repo");
    GitWorktreeBackend backend(git, base / "repo", sha, base / "worktrees", runner);
    WorktreeAllocator allocator(base / "worktrees", backend);
    allocator.allocate(3);

    allocator.release(3);
    check(!fs::exists(base / "worktrees" / "job-3") && !fs::exists(base / "worktrees" / "job-3.owner"),
          "worktree: release removes the directory and the owner record");
    check(git_output(runner, git, base / "repo", {"worktree", "list", "--porcelain"}).find("job-3") ==
              std::string::npos,
          "worktree: git no longer lists the removed worktree");
    check(throws<std::runtime_error>([&] { allocator.release(3); }),
          "worktree: repeated cleanup of the same job is refused, not silently repeated");
    fs::remove_all(base);
}

void test_refuses_unmanaged_paths(SystemProcessRunner& runner, const std::string& git) {
    const fs::path base = fresh_base("unmanaged");
    const std::string sha = make_repository(runner, git, base / "repo");
    const fs::path root = base / "worktrees";
    GitWorktreeBackend backend(git, base / "repo", sha, root, runner);
    WorktreeAllocator allocator(root, backend);
    fs::create_directories(root / "job-4");  // a bare directory with no record

    check(throws<std::runtime_error>([&] { allocator.release(4); }),
          "worktree: a bare directory without an owner record is never removed");
    check(fs::exists(root / "job-4"), "worktree: the refused bare directory is left in place");
    fs::remove_all(base);
}

void test_dirty_worktree_is_refused_and_stays_owned(SystemProcessRunner& runner, const std::string& git) {
    const fs::path base = fresh_base("dirty");
    const std::string sha = make_repository(runner, git, base / "repo");
    GitWorktreeBackend backend(git, base / "repo", sha, base / "worktrees", runner);
    WorktreeAllocator allocator(base / "worktrees", backend);
    allocator.allocate(5);
    std::ofstream(base / "worktrees" / "job-5" / "uncommitted.txt") << "work in progress\n";

    check(throws<std::runtime_error>([&] { allocator.release(5); }),
          "worktree: git refuses to remove a dirty worktree and the release fails");
    check(fs::exists(base / "worktrees" / "job-5") && fs::exists(base / "worktrees" / "job-5.owner"),
          "worktree: a refused removal keeps both the work and the owner record");
    fs::remove_all(base);
}

void test_stale_record_is_not_guessed(SystemProcessRunner& runner, const std::string& git) {
    const fs::path base = fresh_base("stale");
    const std::string sha = make_repository(runner, git, base / "repo");
    const fs::path root = base / "worktrees";
    GitWorktreeBackend backend(git, base / "repo", sha, root, runner);
    WorktreeAllocator allocator(root, backend);
    allocator.allocate(6);
    fs::remove_all(root / "job-6");  // directory lost, record survives

    check(throws<std::runtime_error>([&] { allocator.release(6); }),
          "worktree: release with a missing directory fails closed and does not guess");
    check(allocator.allocate(6) == WorktreeOutcome::Conflict,
          "worktree: a record with a missing directory is a conflict, not a silent recreate");
    fs::remove_all(base);
}

}  // namespace

int main() {
    SystemProcessRunner runner;
    const std::string git = git_executable();
    test_creates_detached_worktree_at_base(runner, git);
    test_rejects_invalid_job_ids(runner, git);
    test_rejects_traversal_and_root_escape(runner, git);
    test_refuses_protected_roots(runner, git);
    test_removes_managed_worktree(runner, git);
    test_refuses_unmanaged_paths(runner, git);
    test_dirty_worktree_is_refused_and_stays_owned(runner, git);
    test_stale_record_is_not_guessed(runner, git);
    std::cout << (failures == 0 ? "ALL PASSED" : "FAILURES") << "\n";
    return failures == 0 ? 0 : 1;
}
