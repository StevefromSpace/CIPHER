// Manual validation of the real QA path: real git, cmake and ctest through SystemProcessRunner in a disposable
// repository. NOT registered with CTest (it runs a nested CMake build); build the target and run it by hand:
//   cipher_qa_real_validation <git> <cmake> <ctest> <empty-scratch-dir>
// Exit 0 only if every case behaves as expected.

#include "cipher/orchestration/git_worktree_backend.hpp"
#include "cipher/orchestration/pipeline_roles.hpp"
#include "cipher/orchestration/qa_profile.hpp"

#include <chrono>
#include <functional>
#include <fstream>
#include <iostream>

namespace {

using namespace cipher::orchestration;
namespace fs = std::filesystem;

int failures = 0;
void check(bool ok, const std::string& what) {
    std::cout << (ok ? "PASS: " : "FAIL: ") << what << "\n";
    if (!ok) ++failures;
}

struct NoGh final : GhRunner {
    GhResult run(const std::vector<std::string>&) override { throw std::runtime_error("gh must not be used by QA"); }
};
struct NoNext final : RoleExecutor {
    RoleResult run(Role, const JobRecord&) override { throw std::runtime_error("unexpected role"); }
};

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) { std::cerr << "usage: git cmake ctest scratch-dir\n"; return 2; }
    const QaTools tools{argv[1], argv[2], argv[3], ""};
    const fs::path work = fs::absolute(argv[4]);
    if (fs::exists(work) && !fs::is_empty(work)) { std::cerr << "scratch dir must be empty\n"; return 2; }
    const fs::path repo = work / "repo";
    SystemProcessRunner sys;
    auto git_in = [&](const fs::path& dir, std::vector<std::string> args) {
        std::vector<std::string> a = {tools.git, "-C", dir.string(), "-c", "user.name=qa", "-c", "user.email=qa@example.invalid",
                                      "-c", "commit.gpgsign=false"};
        a.insert(a.end(), args.begin(), args.end());
        const ProcessResult r = sys.run(a, 60000);
        if (r.exit_code != 0) throw std::runtime_error("git failed: " + r.stderr_text);
        std::string out = r.stdout_text;
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        return out;
    };

    fs::create_directories(repo);
    git_in(repo, {"init", "-q"});
    write(repo / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.16)\nproject(qa_probe CXX)\nenable_testing()\n"
                                   "add_executable(probe src/main.cpp)\nadd_test(NAME probe COMMAND probe)\n");
    write(repo / "src/main.cpp", "int main() { return 0; }\n");
    write(repo / "README.md", "base\n");
    git_in(repo, {"add", "-A"});
    git_in(repo, {"commit", "-q", "-m", "base"});
    const std::string base = git_in(repo, {"rev-parse", "HEAD"});

    GitWorktreeBackend backend(tools.git, repo, base, work / "wt", sys);
    WorktreeAllocator allocator(work / "wt", backend);
    NoGh no_gh_runner;
    GhAdapter gh(no_gh_runner);
    NoNext next;

    // Creates job worktree <issue>, applies @p change, commits, and returns the head SHA.
    auto job_worktree = [&](int issue, const std::function<void(const fs::path&)>& change) {
        allocator.allocate(issue);
        const fs::path wt = allocator.path_for(issue);
        change(wt);
        git_in(wt, {"add", "-A"});
        git_in(wt, {"commit", "-q", "-m", "job"});
        return git_in(wt, {"rev-parse", "HEAD"});
    };
    auto settings_for = [&](int issue, const QaPlan& plan) {
        PipelineSettings s;
        s.issue_number = issue;
        s.git_executable = tools.git;
        s.base_sha = base;
        s.evidence_file = work / ("evidence-" + std::to_string(issue) + ".log");
        s.test_commands = plan.commands;
        s.qa_command_ids = plan.ids;
        return s;
    };
    auto record = [](const std::string& head) { JobRecord j; j.head_sha = head; return j; };

    try {
        // 1. PASS: docs-only change, real check + cmake configure/build + ctest.
        const std::string head1 = job_worktree(1, [](const fs::path& wt) { write(wt / "README.md", "base\nchange\n"); });
        const fs::path wt1 = allocator.path_for(1);
        const QaPlan plan1 = build_qa_plan("check,build,ctest", wt1, work / "qa-build-1", tools);
        PipelineSettings s1 = settings_for(1, plan1);
        RoleResult r = PipelineExecutor(s1, allocator, sys, gh, next).run(Role::Qa, record(head1));
        check(r.event.kind == EventKind::QaResult && r.event.verdict == Verdict::Approve && r.event.head_sha == head1,
              "real check+build+ctest -> PASS on exact head");
        check(fs::exists(work / "qa-build-1" / "CMakeCache.txt"), "build happened out of tree (CMakeCache in qa-build-1)");
        check(!fs::exists(wt1 / "build") && !fs::exists(wt1 / "CMakeCache.txt"), "nothing built inside the worktree");
        const std::string log1 = slurp(s1.evidence_file);
        for (const auto& id : plan1.ids)
            check(log1.find("QA " + id + " @" + head1 + " exit=0") != std::string::npos, "evidence for " + id + " names exact SHA");
        check(git_in(wt1, {"status", "--porcelain", "--untracked-files=no"}).empty() && git_in(wt1, {"rev-parse", "HEAD"}) == head1,
              "tracked files unmodified, HEAD unchanged after QA");

        // 2. FAIL: change makes the test executable return 1, so real ctest exits non-zero.
        const std::string head2 = job_worktree(2, [](const fs::path& wt) {
            write(wt / "src/main.cpp", "int main() { return 1; }\n");
            write(wt / "tests/note.txt", "covers the change\n");
        });
        const QaPlan plan2 = build_qa_plan("build,ctest", allocator.path_for(2), work / "qa-build-2", tools);
        r = PipelineExecutor(settings_for(2, plan2), allocator, sys, gh, next).run(Role::Qa, record(head2));
        check(r.event.kind == EventKind::QaResult && r.event.verdict == Verdict::ChangesRequested, "failing real ctest -> FAIL");

        // 3. BLOCKED: timeout kills a real sleeping process and maps to Blocked quickly.
        const std::string head3 = job_worktree(3, [](const fs::path& wt) { write(wt / "README.md", "base\nthree\n"); });
        QaPlan slow;
        slow.commands["slow"] = {tools.cmake, "-E", "sleep", "30"};
        slow.ids = {"slow"};
        PipelineSettings s3 = settings_for(3, slow);
        s3.qa_timeout_ms = 1500;
        const auto t0 = std::chrono::steady_clock::now();
        r = PipelineExecutor(s3, allocator, sys, gh, next).run(Role::Qa, record(head3));
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
        check(r.event.kind == EventKind::Blocked && secs < 20, "real timeout -> BLOCKED in " + std::to_string(secs) + "s (limit 1.5s, sleep 30s)");

        // 4. BLOCKED: output past the runner cap is a failure, never a partial pass.
        QaPlan loud;
        loud.commands["loud"] = {tools.cmake, "--help"};
        loud.ids = {"loud"};
        SystemProcessRunner tiny(64);
        r = PipelineExecutor(settings_for(3, loud), allocator, tiny, gh, next).run(Role::Qa, record(head3));
        check(r.event.kind == EventKind::Blocked, "real output overflow -> BLOCKED");

        // 5. BLOCKED: a QA command that rewrites a tracked file invalidates the run.
        const std::string head4 = job_worktree(4, [](const fs::path& wt) { write(wt / "README.md", "base\nfour\n"); });
        write(work / "other.txt", "different\n");
        QaPlan mutate;
        mutate.commands["mutate"] = {tools.cmake, "-E", "copy", (work / "other.txt").string(), (allocator.path_for(4) / "README.md").string()};
        mutate.ids = {"mutate"};
        r = PipelineExecutor(settings_for(4, mutate), allocator, sys, gh, next).run(Role::Qa, record(head4));
        check(r.event.kind == EventKind::Blocked, "QA that modified a tracked file -> BLOCKED");

        // 6. BLOCKED: the job's recorded head is not what the worktree holds.
        r = PipelineExecutor(settings_for(1, plan1), allocator, sys, gh, next).run(Role::Qa, record(head2));
        check(r.event.kind == EventKind::Blocked, "head mismatch -> BLOCKED");
    } catch (const std::exception& error) {
        check(false, std::string("unexpected exception: ") + error.what());
    }
    std::cout << (failures == 0 ? "ALL PASSED" : "FAILURES") << " (scratch: " << work.string() << ")\n";
    return failures == 0 ? 0 : 1;
}
