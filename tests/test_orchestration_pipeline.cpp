// PR creation, QA, mechanical review and CI observation. All fakes: scripted git/test runner and scripted gh runner.

#include "cipher/orchestration/pipeline_roles.hpp"
#include "cipher/orchestration/qa_profile.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using namespace cipher::orchestration;
namespace fs = std::filesystem;

int failures = 0;

void check(bool ok, const char* what) {
    std::cout << (ok ? "PASS: " : "FAIL: ") << what << "\n";
    if (!ok) ++failures;
}

const std::string kHead(40, 'a');
const std::string kBase(40, 'b');

std::string pr_json(const std::string& head, const std::string& checks_status) {
    return R"({"number":7,"state":"OPEN","headRefOid":")" + head + R"(","baseRefOid":")" + kBase +
           R"(","mergeable":"MERGEABLE","reviews":[],"statusCheckRollup":[{"__typename":"CheckRun","name":"x",)"
           R"("status":"COMPLETED","conclusion":")" + checks_status + R"("}]})";
}

class FakeBackend final : public WorktreeBackend {
public:
    void add(const fs::path& path, int) override { fs::create_directories(path); }
    void remove(const fs::path&, int) override {}
};

// Scripted by the git subcommand found in argv.
class FakeRunner final : public ProcessRunner {
public:
    std::string head = kHead;
    std::string names;           // diff --name-only output
    std::string diff;            // diff output
    std::string status_out;      // git status --porcelain output (non-empty = tracked files modified)
    int push_exit = 0;
    int test_exit = 0;
    int test_calls = 0, push_calls = 0;
    std::vector<std::vector<std::string>> argvs;
    ProcessResult run(const std::vector<std::string>& argv, int) override {
        argvs.push_back(argv);
        ProcessResult r;
        if (argv.size() < 5 || argv[0] == "TEST") {  // approved test command
            ++test_calls;
            r.exit_code = test_exit;
            return r;
        }
        const std::string& sub = argv[4];
        if (sub == "rev-parse") r.stdout_text = head + "\n";
        else if (sub == "status") r.stdout_text = status_out;
        else if (sub == "push") { ++push_calls; r.exit_code = push_exit; }
        else if (sub == "diff" && argv.size() > 5 && argv[5] == "--name-only") r.stdout_text = names;
        else if (sub == "diff") r.stdout_text = diff;
        return r;
    }
};

class FakeGh final : public GhRunner {
public:
    std::string list_output;     // existing PR numbers
    std::string view_output = pr_json(kHead, "SUCCESS");
    int create_exit = 0;
    bool pr_exists_after_failed_create = false;  // models a timeout after GitHub already made the PR
    int creates = 0;
    std::string create_output = "https://github.com/o/r/pull/7\n";
    GhResult run(const std::vector<std::string>& argv) override {
        if (argv[1] == "pr" && argv[2] == "list") return {0, list_output};
        if (argv[2] == "create") {
            ++creates;
            if (create_exit != 0 && pr_exists_after_failed_create) list_output = "7\n";
            return {create_exit, create_output};
        }
        return {0, view_output};
    }
};

class Next final : public RoleExecutor {
public:
    int calls = 0;
    RoleResult run(Role, const JobRecord&) override { ++calls; return {Event{EventKind::Blocked}, "next"}; }
};

struct Rig {
    fs::path base;
    FakeBackend backend;
    std::unique_ptr<WorktreeAllocator> allocator;
    FakeRunner runner;
    FakeGh gh_runner;
    GhAdapter gh{gh_runner};
    Next next;
    PipelineSettings settings;
    std::unique_ptr<PipelineExecutor> exec;
    JobRecord job;

    explicit Rig(const std::string& name) {
        base = fs::temp_directory_path() / ("cipher pipeline " + name + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(base);
        allocator = std::make_unique<WorktreeAllocator>(base / "wt", backend);
        allocator->allocate(5);
        settings.issue_number = 5;
        settings.git_executable = "git";
        settings.base_sha = kBase;
        settings.evidence_file = base / "evidence.log";
        settings.pr_body_file = base / "body.md";
        settings.test_commands["t"] = {"TEST", "run"};
        settings.qa_command_ids = {"t"};
        job.head_sha = kHead;
        exec = std::make_unique<PipelineExecutor>(settings, *allocator, runner, gh, next);
    }
};

bool is(const RoleResult& r, EventKind kind) { return r.event.kind == kind; }

void test_pr_creation() {
    Rig a("pr1");
    a.job.state = State::PullRequest;
    RoleResult r = a.exec->run(Role::Implementation, a.job);
    check(is(r, EventKind::PullRequestOpened) && r.event.head_sha == kHead, "PR created and head verified");
    check(a.gh_runner.creates == 1 && a.runner.push_calls == 1, "one push and one create");

    Rig dup("pr2");
    dup.job.state = State::PullRequest;
    dup.gh_runner.list_output = "7\n";
    r = dup.exec->run(Role::Implementation, dup.job);
    check(is(r, EventKind::PullRequestOpened) && dup.gh_runner.creates == 0, "existing PR reused, no duplicate");

    Rig timed("pr-timeout");
    timed.job.state = State::PullRequest;
    timed.gh_runner.create_exit = 1;
    timed.gh_runner.pr_exists_after_failed_create = true;
    r = timed.exec->run(Role::Implementation, timed.job);
    check(is(r, EventKind::PullRequestOpened) && timed.gh_runner.creates == 1,
          "create failed after GitHub made the PR: the PR is found and reused, not blocked");
    r = timed.exec->run(Role::Implementation, timed.job);
    check(is(r, EventKind::PullRequestOpened) && timed.gh_runner.creates == 1,
          "a retry after that reuses the PR and never creates a second one");

    Rig failed("pr-failed");
    failed.job.state = State::PullRequest;
    failed.gh_runner.create_exit = 1;
    check(is(failed.exec->run(Role::Implementation, failed.job), EventKind::Blocked),
          "create failed and no PR exists: blocked");

    Rig two("pr3");
    two.job.state = State::PullRequest;
    two.gh_runner.list_output = "7\n8\n";
    check(is(two.exec->run(Role::Implementation, two.job), EventKind::Blocked) && two.runner.push_calls == 0,
          "two open PRs -> blocked before push");

    Rig moved("pr4");
    moved.job.state = State::PullRequest;
    moved.runner.head = std::string(40, 'c');
    check(is(moved.exec->run(Role::Implementation, moved.job), EventKind::Blocked) && moved.runner.push_calls == 0,
          "worktree head mismatch -> blocked");

    Rig stale("pr5");
    stale.job.state = State::PullRequest;
    stale.gh_runner.view_output = pr_json(std::string(40, 'd'), "SUCCESS");
    check(is(stale.exec->run(Role::Implementation, stale.job), EventKind::Blocked), "PR head differs -> blocked");

    Rig pushfail("pr6");
    pushfail.job.state = State::PullRequest;
    pushfail.runner.push_exit = 1;
    check(is(pushfail.exec->run(Role::Implementation, pushfail.job), EventKind::Blocked) && pushfail.gh_runner.creates == 0,
          "push failure -> blocked, no PR");

    Rig ambiguous("pr7");
    ambiguous.job.state = State::PullRequest;
    ambiguous.gh_runner.create_output = "created something\n";
    check(is(ambiguous.exec->run(Role::Implementation, ambiguous.job), EventKind::Blocked), "ambiguous gh output -> blocked");

    Rig other("pr8");
    other.job.state = State::Implementation;
    other.exec->run(Role::Implementation, other.job);
    check(other.next.calls == 1, "implementation state delegated to next");

    GhRunner* none = nullptr;
    (void)none;
    bool threw = false;
    try { Rig(std::string("pr9")).gh.create_pull_request("--evil", "main", "t", "f"); } catch (const GhRequestError&) { threw = true; }
    check(threw, "unsafe branch rejected");
    threw = false;
    try { Rig(std::string("pr10")).gh.create_pull_request("a", "a", "t", "f"); } catch (const GhRequestError&) { threw = true; }
    check(threw, "head == base rejected");
}

void test_qa() {
    Rig a("qa1");
    a.runner.names = "src/a.cpp\ntests/a_test.cpp\n";
    RoleResult r = a.exec->run(Role::Qa, a.job);
    check(is(r, EventKind::QaResult) && r.event.verdict == Verdict::Approve && a.runner.test_calls == 1, "QA passes");

    Rig cov("qa2");
    cov.runner.names = "src/a.cpp\n";
    r = cov.exec->run(Role::Qa, cov.job);
    check(r.event.verdict == Verdict::ChangesRequested && cov.runner.test_calls == 0, "missing test coverage fails QA");

    Rig fail("qa3");
    fail.runner.names = "docs/x.md\n";
    fail.runner.test_exit = 1;
    check(fail.exec->run(Role::Qa, fail.job).event.verdict == Verdict::ChangesRequested, "failing test fails QA");

    Rig empty("qa4");
    check(empty.exec->run(Role::Qa, empty.job).event.verdict == Verdict::ChangesRequested, "empty diff fails QA");

    Rig head("qa5");
    head.runner.names = "docs/x.md\n";
    head.runner.head = std::string(40, 'c');
    check(is(head.exec->run(Role::Qa, head.job), EventKind::Blocked), "QA blocked on head mismatch");

    Rig nocmd("qa6");
    nocmd.settings.qa_command_ids.clear();
    PipelineExecutor bare(nocmd.settings, *nocmd.allocator, nocmd.runner, nocmd.gh, nocmd.next);
    nocmd.runner.names = "docs/x.md\n";
    check(is(bare.run(Role::Qa, nocmd.job), EventKind::Blocked), "QA without approved commands is blocked, not passed");
}

void test_qa_dirty_and_profiles() {
    Rig dirty("qa7");
    dirty.runner.names = "docs/x.md\n";
    dirty.runner.status_out = " M src/a.cpp\n";
    check(is(dirty.exec->run(Role::Qa, dirty.job), EventKind::Blocked), "QA that modified tracked files is blocked");

    Rig ok("qa8");
    ok.runner.names = "docs/x.md\n";
    ok.exec->run(Role::Qa, ok.job);
    std::ifstream in(ok.settings.evidence_file);
    const std::string log((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check(log.find("QA t @" + kHead + " exit=0") != std::string::npos, "evidence records command, exact SHA and exit");

    const QaTools tools{"/g", "/cm", "/ct", "/py"};
    const QaPlan plan = build_qa_plan("check, build,ctest,python", "/wt", "/bd", tools);
    const std::vector<std::string> ids = {"check", "cmake-configure", "cmake-build", "ctest", "python-integrations",
                                          "python-pipeline"};
    check(plan.ids == ids, "profiles expand to ordered approved commands");
    check(plan.commands.at("cmake-build")[0] == "/cm" && plan.commands.at("ctest")[2] == "/bd",
          "commands use explicit executables and out-of-tree build dir");
    for (const char* bad : {"", "bogus", "check,check", "ctest", "build,ctest,ctest"}) {
        bool threw = false;
        try { build_qa_plan(bad, "/wt", "/bd", tools); } catch (const std::invalid_argument&) { threw = true; }
        check(threw, (std::string("rejected profile list '") + bad + "'").c_str());
    }
    bool missing = false;
    try { build_qa_plan("build", "/wt", "/bd", QaTools{"/g", "", "", ""}); } catch (const std::invalid_argument&) { missing = true; }
    check(missing, "missing tool is an error, not a skipped check");
}

void test_review() {
    Rig a("rv1");
    a.runner.names = "src/a.cpp\ntests/a_test.cpp\n";
    RoleResult r = a.exec->run(Role::Reviewer, a.job);
    check(is(r, EventKind::ReviewVerdict) && r.event.verdict == Verdict::Approve, "clean diff approved");

    Rig sec("rv2");
    sec.runner.names = "src/a.cpp\ntests/a_test.cpp\n";
    sec.job.facts.security_sensitive = true;
    check(sec.exec->run(Role::Reviewer, sec.job).event.verdict == Verdict::NeedsHuman, "security-sensitive -> NEEDS_HUMAN");

    for (const char* path : {"tools/pipeline/x.py", ".github/workflows/ci.yml", "docs/AGENT_ROLE_CONTRACT.md",
                             "CMakeLists.txt", "src/secret_store.cpp"}) {
        Rig p("rv3");
        p.runner.names = std::string(path) + "\ntests/a.cpp\n";
        check(p.exec->run(Role::Reviewer, p.job).event.verdict == Verdict::NeedsHuman, path);
    }

    Rig key("rv4");
    key.runner.names = "docs/x.md\n";
    key.runner.diff = "+token ghp_abcdef\n";
    check(key.exec->run(Role::Reviewer, key.job).event.verdict == Verdict::ChangesRequested, "secret marker rejected");

    Rig nt("rv5");
    nt.runner.names = "src/a.cpp\n";
    check(nt.exec->run(Role::Reviewer, nt.job).event.verdict == Verdict::ChangesRequested, "no tests -> changes requested");

    Rig docs("rv6");
    docs.runner.names = "docs/x.md\n";
    check(docs.exec->run(Role::Reviewer, docs.job).event.verdict == Verdict::Approve, "docs-only approved");
}

void test_ci() {
    JobRecord job;
    job.head_sha = kHead;
    FakeGh g;
    GhAdapter gh(g);
    auto e = observe_ci(job, gh, 7);
    check(e && e->kind == EventKind::CiResult && e->ci_passed && e->head_sha == kHead, "CI passed observed");
    g.view_output = pr_json(kHead, "FAILURE");
    e = observe_ci(job, gh, 7);
    check(e && e->kind == EventKind::CiResult && !e->ci_passed, "CI failure observed");
    g.view_output = R"({"number":7,"state":"OPEN","headRefOid":")" + kHead + R"(","baseRefOid":")" + kBase +
                    R"(","mergeable":"MERGEABLE","reviews":[],"statusCheckRollup":[]})";
    check(!observe_ci(job, gh, 7).has_value(), "no checks yet -> still pending");
    g.view_output = pr_json(std::string(40, 'd'), "SUCCESS");
    e = observe_ci(job, gh, 7);
    check(e && e->kind == EventKind::Blocked, "different head -> blocked");
    g.view_output = "not json";
    e = observe_ci(job, gh, 7);
    check(e && e->kind == EventKind::Blocked, "malformed output -> blocked");
}

}  // namespace

int main() {
    test_pr_creation();
    test_qa();
    test_qa_dirty_and_profiles();
    test_review();
    test_ci();
    std::cout << (failures == 0 ? "ALL PASSED\n" : "FAILURES\n");
    return failures == 0 ? 0 : 1;
}
