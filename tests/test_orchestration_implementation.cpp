// Tool layer + implementation executor. Tool tests use a fake runner; the integration test uses real git, a real
// worktree, real files and the real orchestrator/job store, with a scripted (non-paid) agent.

#include "cipher/orchestration/executable_resolver.hpp"
#include "cipher/orchestration/git_worktree_backend.hpp"
#include "cipher/orchestration/implementation_executor.hpp"
#include "cipher/orchestration/readonly_executor.hpp"
#include "cipher/orchestration/job_store_file.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

using namespace cipher::orchestration;
namespace fs = std::filesystem;

int failures = 0;

void check(bool ok, const char* what) {
    std::cout << (ok ? "PASS: " : "FAIL: ") << what << "\n";
    if (!ok) ++failures;
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

fs::path fresh_base(const std::string& name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path base = fs::temp_directory_path() / ("cipher impl test " + name + "-" + std::to_string(stamp));
    fs::create_directories(base);
    return base;
}

class FakeRunner final : public ProcessRunner {
public:
    ProcessResult next;
    int calls = 0;
    std::vector<std::string> last_argv;
    ProcessResult run(const std::vector<std::string>& argv, int) override {
        ++calls;
        last_argv = argv;
        return next;
    }
};

ToolCall call(const std::string& tool, const std::string& path = "", const std::string& pattern = "") {
    ToolCall c;
    c.tool = tool;
    c.path = path;
    c.pattern = pattern;
    return c;
}

// ---- tool layer -----------------------------------------------------------------------------------------------

void test_tools() {
    const fs::path base = fresh_base("tools");
    const fs::path root = base / "wt";
    fs::create_directories(root / "docs");
    fs::create_directories(root / ".git");
    std::ofstream(root / "docs" / "a.txt", std::ios::binary) << "hello\nneedle here\n";
    FakeRunner runner;
    runner.next.stdout_text = "ok-out";
    WorktreeTools tools(root, runner, "git", {{"build", {"cmake", "--build", "."}}}, default_worktree_policy());

    check(tools.call(call("fs.read", "docs/a.txt")).output == "hello\nneedle here\n", "tools: read returns file text");
    check(has(tools.call(call("fs.list", "docs")).output, "a.txt"), "tools: list shows entries");
    check(!has(tools.call(call("fs.list", ".")).output, ".git"), "tools: list hides .git");
    check(has(tools.call(call("fs.search", ".", "needle")).output, "docs/a.txt:2:"), "tools: search finds line");

    ToolCall write = call("fs.write", "docs/new/b.txt");
    write.content = "created";
    check(tools.call(write).ok && slurp(root / "docs" / "new" / "b.txt") == "created", "tools: write creates file");

    ToolCall edit = call("fs.edit", "docs/a.txt");
    edit.old_text = "hello";
    edit.new_text = "bye";
    check(tools.call(edit).ok && has(slurp(root / "docs" / "a.txt"), "bye\n"), "tools: edit replaces text");
    check(!tools.call(edit).ok, "tools: edit with missing old_text fails");
    ToolCall ambiguous = call("fs.edit", "docs/new/b.txt");
    ambiguous.old_text = "e";
    check(!tools.call(ambiguous).ok, "tools: ambiguous edit is refused");
    ToolCall empty_edit = call("fs.edit", "docs/a.txt");
    check(!tools.call(empty_edit).ok, "tools: empty old_text refused");

    check(!tools.call(call("fs.read", "../x")).ok, "tools: traversal refused");
    check(!tools.call(call("fs.read", "docs/../../x")).ok, "tools: nested traversal refused");
    check(!tools.call(call("fs.read", (base / "wt" / "docs" / "a.txt").string())).ok, "tools: absolute path refused");
    check(!tools.call(call("fs.read", "C:/Windows/win.ini")).ok, "tools: drive path refused");
    check(!tools.call(call("fs.read", ".git/config")).ok, ".git read refused");
    write.path = ".git/hooks/pre-commit";
    check(!tools.call(write).ok, "tools: write into .git refused");
    write.path = ".GIT/config";
    check(!tools.call(write).ok, "tools: case-variant .git refused");
    for (const char* protected_path : {".github/workflows/ci.yml", "tools/pipeline/x.py", "docs/AGENT_ROLE_CONTRACT.md"}) {
        write.path = protected_path;
        check(!tools.call(write).ok && !fs::exists(root / protected_path), "tools: protected path write refused");
    }
    ToolCall big = call("fs.write", "big.txt");
    big.content.assign(300 * 1024, 'x');
    check(!tools.call(big).ok, "tools: oversized write refused");
    check(!tools.call(call("shell.exec", "", "rm -rf /")).ok, "tools: unknown tool refused");
    check(!tools.call(call("fs.read", "docs")).ok, "tools: read of directory refused");

    // Output limit.
    std::ofstream(root / "large.txt", std::ios::binary) << std::string(100 * 1024, 'a');
    check(has(tools.call(call("fs.read", "large.txt")).output, "[file truncated]"), "tools: read output bounded");

    // process.run: only registered ids, results mapped.
    const int before = runner.calls;
    check(!tools.call(call("process.run", "", "rm -rf /")).ok && runner.calls == before, "tools: unknown command id never launches");
    check(tools.call(call("process.run", "", "build")).ok && runner.last_argv[0] == "cmake", "tools: registered command runs");
    runner.next.exit_code = 2;
    check(!tools.call(call("process.run", "", "build")).ok, "tools: non-zero exit is a failure");
    runner.next.exit_code = -1;
    runner.next.timed_out = true;
    check(has(tools.call(call("process.run", "", "build")).output, "timed_out"), "tools: timeout reported");
    runner.next = {};
    runner.next.output_overflow = true;
    check(!tools.call(call("process.run", "", "build")).ok, "tools: output overflow is a failure");
    runner.next = {};
    runner.next.stdout_text = std::string(40000, 'z');
    check(tools.call(call("process.run", "", "build")).output.size() < 20000, "tools: process output bounded");

    // git tools use a fixed argv; the model supplies no executable.
    tools.call(call("git.status"));
    check(runner.last_argv[0] == "git" && runner.last_argv[4] == "status", "tools: git.status argv is fixed");
    check(tools.audit().size() > 20 && has(tools.audit().front(), "fs.read"), "tools: every call is audited");

    // Denied-by-policy: an empty policy defaults to Ask which has no owner here.
    cipher::PermissionPolicy strict;
    strict.set_default_tier(cipher::RiskTier::Ask);
    WorktreeTools locked(root, runner, "git", {}, strict);
    check(has(locked.call(call("fs.read", "docs/a.txt")).output, "denied"), "tools: policy denial is enforced");
    fs::remove_all(base);
}

// ---- agent reply parsing -----------------------------------------------------------------------------------------

template <typename F>
bool throws_runtime(F&& action) {
    try {
        action();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

void test_parse() {
    check(parse_agent_reply("{\"done\":true,\"summary\":\"ok\"}").done, "parse: done");
    const AgentStep step = parse_agent_reply("Sure! {\"tool\":\"fs.read\",\"path\":\"a\"} thanks");
    check(!step.done && step.call.tool == "fs.read" && step.call.path == "a", "parse: tool call amid prose");
    check(throws_runtime([] { parse_agent_reply("no json"); }), "parse: no JSON rejected");
    check(throws_runtime([] { parse_agent_reply("{bad json}"); }), "parse: invalid JSON rejected");
    check(throws_runtime([] { parse_agent_reply("{\"path\":\"a\"}"); }), "parse: missing tool rejected");
    check(throws_runtime([] { parse_agent_reply("{\"tool\":5}"); }), "parse: non-string tool rejected");
    check(throws_runtime([] { parse_agent_reply("{\"done\":\"yes\"}"); }), "parse: non-boolean done rejected");
    check(throws_runtime([] { parse_agent_reply("[1,2]"); }), "parse: non-object rejected");
}

class FakeClaudeRunner final : public ProcessRunner {
public:
    std::string reply;
    int calls = 0;
    ProcessResult run(const std::vector<std::string>&, int) override {
        ++calls;
        ProcessResult r;
        r.stdout_text = "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"result\":" + reply + "}";
        return r;
    }
};

void test_claude_agent() {
    FakeClaudeRunner runner;
    runner.reply = "\"{\\\"done\\\":true,\\\"summary\\\":\\\"fin\\\"}\"";
    ClaudeToolAgent agent(runner, "claude", "haiku", "0.50", 1000);
    const AgentStep step = agent.next("task", {});
    check(step.done && step.summary == "fin" && runner.calls == 1, "claude agent: parses reply, one call per step");
    runner.reply = "\"garbage\"";
    check(throws_runtime([&] { agent.next("task", {}); }), "claude agent: malformed reply throws");
    FakeRunner failing;
    failing.next.exit_code = 1;
    ClaudeToolAgent broken(failing, "claude", "haiku", "0.50", 1000);
    check(throws_runtime([&] { broken.next("task", {}); }) && failing.calls == 1, "claude agent: failed claude throws, no retry");
}

// ---- executor integration (real git, real files) ----------------------------------------------------------------

class ScriptedAgent final : public ToolAgent {
public:
    std::vector<AgentStep> script;
    bool loop_forever = false;
    bool throw_malformed = false;
    int calls = 0;
    std::size_t index = 0;
    AgentStep next(const std::string&, const std::vector<ToolTurn>&) override {
        ++calls;
        if (throw_malformed) throw std::runtime_error("malformed");
        if (loop_forever) return {false, call("fs.list", "."), ""};
        return script.at(index++);
    }
};

std::string git_out(SystemProcessRunner& runner, const std::string& git, const fs::path& dir,
                    const std::vector<std::string>& args) {
    std::vector<std::string> argv{git, "-C", dir.string()};
    argv.insert(argv.end(), args.begin(), args.end());
    ProcessResult result = runner.run(argv, 30000);
    std::string text = result.stdout_text;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

std::string make_repo(SystemProcessRunner& runner, const std::string& git, const fs::path& repo) {
    fs::create_directories(repo);
    runner.run({git, "-C", repo.string(), "init", "-q"}, 30000);
    std::ofstream(repo / "README.txt") << "probe\n";
    runner.run({git, "-C", repo.string(), "add", "README.txt"}, 30000);
    runner.run({git, "-C", repo.string(), "-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-q",
                "-m", "base"},
               30000);
    return git_out(runner, git, repo, {"rev-parse", "HEAD"});
}

struct Fixture {
    SystemProcessRunner runner{1 << 20};
    std::string git = resolve_executable_on_path("git");
    fs::path base;
    std::string sha;
    std::unique_ptr<GitWorktreeBackend> backend;
    std::unique_ptr<WorktreeAllocator> allocator;
    explicit Fixture(const std::string& name) : base(fresh_base(name)) {
        sha = make_repo(runner, git, base / "repo");
        backend = std::make_unique<GitWorktreeBackend>(git, base / "repo", sha, base / "worktrees", runner);
        allocator = std::make_unique<WorktreeAllocator>(base / "worktrees", *backend);
    }
};

class Escalator final : public RoleExecutor {
public:
    RoleResult run(Role, const JobRecord&) override {
        Event e;
        e.kind = EventKind::Escalate;
        e.reason = EscalationReason::RoleNotSupported;
        return {e, ""};
    }
};

ImplementationSettings settings(const Fixture& f, int issue) {
    ImplementationSettings s;
    s.issue_number = issue;
    s.task = "add a docs note";
    s.git_executable = f.git;
    s.model_family = "fake";
    s.max_steps = 5;
    s.commands["check"] = {f.git, "-C", (f.base / "worktrees" / ("job-" + std::to_string(issue))).string(), "diff", "--check"};
    s.verify_command_id = "check";
    s.evidence_file = f.base / "evidence.log";
    return s;
}

ScriptedAgent doc_agent() {
    ScriptedAgent agent;
    ToolCall write = call("fs.write", "docs/NOTE.md");
    write.content = "# Note\nwritten by the tool layer\n";
    agent.script = {{false, call("fs.list", "."), ""}, {false, write, ""}, {true, {}, "added note"}};
    return agent;
}

void test_end_to_end() {
    Fixture f("e2e");
    Escalator fallback;
    ScriptedAgent agent = doc_agent();
    ImplementationExecutor executor(settings(f, 21), *f.allocator, f.runner, agent, fallback);
    FileJobStore store(f.base / "jobs");
    {
        Orchestrator job(21, IssueFacts{}, executor, store, Policy{});
        job.start();
        while (job.job().state != State::PullRequest && !is_terminal(job.job().state) && job.advance()) {}
        check(job.job().state == State::PullRequest, "e2e: implementation advances job to PullRequest");
        check(job.job().implementation_executions == 1, "e2e: one execution counted");
        const fs::path wt = f.base / "worktrees" / "job-21";
        check(slurp(wt / "docs" / "NOTE.md") == "# Note\nwritten by the tool layer\n", "e2e: file really written in worktree");
        const std::string head = git_out(f.runner, f.git, wt, {"rev-parse", "HEAD"});
        check(job.job().head_sha == head && head != f.sha, "e2e: recorded head is the new commit");
        check(has(git_out(f.runner, f.git, wt, {"show", "--stat", "--format=%s", "HEAD"}), "docs/NOTE.md"),
              "e2e: commit contains the change");
        check(git_out(f.runner, f.git, wt, {"status", "--porcelain"}).empty(), "e2e: worktree clean after commit");
        check(!fs::exists(f.base / "repo" / "docs" / "NOTE.md"), "e2e: owner checkout untouched");
        check(has(slurp(f.base / "evidence.log"), "COMPLETE head=" + head), "e2e: evidence recorded");
        // PR step must not be faked: fallback escalates.
        job.advance();
        check(job.job().state == State::NeedsHuman && job.job().escalation == EscalationReason::RoleNotSupported,
              "e2e: PullRequest step escalates honestly");
    }
    {
        Orchestrator reloaded(21, IssueFacts{}, executor, store, Policy{});
        check(reloaded.job().state == State::NeedsHuman && reloaded.job().implementation_executions == 1,
              "e2e: state persists across restart");
    }
    fs::remove_all(f.base);
}

void test_failures() {
    {
        Fixture f("loop");
        Escalator fallback;
        ScriptedAgent agent;
        agent.loop_forever = true;
        ImplementationExecutor executor(settings(f, 22), *f.allocator, f.runner, agent, fallback);
        FileJobStore store(f.base / "jobs");
        Orchestrator job(22, IssueFacts{}, executor, store, Policy{});
        job.start();
        while (!is_terminal(job.job().state) && job.job().state != State::PullRequest && job.advance()) {}
        check(agent.calls == 10, "fail: exactly 2 executions x max_steps agent calls, no hidden retry");
        check(job.job().state == State::NeedsHuman && job.job().escalation == EscalationReason::RetryBudgetExhausted,
              "fail: two failed executions exhaust the budget");
        check(job.job().implementation_executions == 2, "fail: execution limit is two");
        fs::remove_all(f.base);
    }
    {
        Fixture f("malformed");
        Escalator fallback;
        ScriptedAgent agent;
        agent.throw_malformed = true;
        ImplementationExecutor executor(settings(f, 23), *f.allocator, f.runner, agent, fallback);
        JobRecord record;
        record.state = State::Implementation;
        check(executor.run(Role::Implementation, record).event.kind == EventKind::ImplementationFailed && agent.calls == 1,
              "fail: agent exception becomes ImplementationFailed after one call");
        fs::remove_all(f.base);
    }
    {
        Fixture f("nochange");
        Escalator fallback;
        ScriptedAgent agent;
        agent.script = {{true, {}, "nothing"}};
        ImplementationExecutor executor(settings(f, 24), *f.allocator, f.runner, agent, fallback);
        JobRecord record;
        record.state = State::Implementation;
        check(executor.run(Role::Implementation, record).event.kind == EventKind::ImplementationFailed,
              "fail: no changes means failed, not complete");
        fs::remove_all(f.base);
    }
    {
        Fixture f("verify");
        Escalator fallback;
        ScriptedAgent agent = doc_agent();
        ImplementationSettings s = settings(f, 25);
        s.commands["check"] = {f.git, "--version-nonexistent-flag"};
        ImplementationExecutor executor(s, *f.allocator, f.runner, agent, fallback);
        JobRecord record;
        record.state = State::Implementation;
        const RoleResult result = executor.run(Role::Implementation, record);
        check(result.event.kind == EventKind::ImplementationFailed, "fail: failing verification blocks the commit");
        check(has(git_out(f.runner, f.git, f.base / "worktrees" / "job-25", {"status", "--porcelain", "-uall"}), "NOTE.md"),
              "fail: change stays uncommitted when verification fails");
        fs::remove_all(f.base);
    }
    {
        Fixture f("other-roles");
        Escalator fallback;
        ScriptedAgent agent;
        ImplementationExecutor executor(settings(f, 26), *f.allocator, f.runner, agent, fallback);
        JobRecord record;
        record.state = State::Qa;
        check(executor.run(Role::Qa, record).event.kind == EventKind::Escalate && agent.calls == 0,
              "roles: non-implementation roles keep escalating");
        record.state = State::Implementation;
        check(executor.run(Role::Security, record).event.kind == EventKind::Escalate && agent.calls == 0,
              "roles: security stays escalated");
        fs::remove_all(f.base);
    }
}

void test_readonly() {
    Fixture f("readonly");
    Escalator fallback;
    ReadOnlySettings s;
    s.issue_number = 31;
    s.git_executable = f.git;
    s.model_family = "fake";
    s.max_steps = 3;
    s.evidence_file = f.base / "ro.log";
    JobRecord record;
    record.state = State::Research;
    {
        ScriptedAgent agent;
        agent.script = {{false, call("fs.read", "README.txt"), ""}, {true, {}, "found readme"}};
        ReadOnlyExecutor ro(s, *f.allocator, f.runner, agent, fallback);
        check(ro.run(Role::Research, record).event.kind == EventKind::ResearchComplete && agent.calls == 2,
              "readonly: research reads then completes");
        check(has(slurp(f.base / "ro.log"), "found readme"), "readonly: summary recorded");
        record.state = State::Architecture;
        ScriptedAgent a2;
        a2.script = {{true, {}, "design"}};
        ReadOnlyExecutor ro2(s, *f.allocator, f.runner, a2, fallback);
        check(ro2.run(Role::Architecture, record).event.kind == EventKind::ArchitectureComplete,
              "readonly: architecture completes");
    }
    record.state = State::Research;
    for (const char* tool : {"fs.write", "fs.edit", "process.run"}) {
        ScriptedAgent agent;
        agent.script = {{false, call(tool, "docs/X.md", "check"), ""}};
        ReadOnlyExecutor ro(s, *f.allocator, f.runner, agent, fallback);
        check(ro.run(Role::Research, record).event.kind == EventKind::Blocked, "readonly: refuses write/edit/process tools");
    }
    check(!fs::exists(f.base / "worktrees" / "job-31" / "docs" / "X.md"), "readonly: nothing written");
    {
        ScriptedAgent agent;
        agent.loop_forever = true;
        ReadOnlyExecutor ro(s, *f.allocator, f.runner, agent, fallback);
        check(ro.run(Role::Research, record).event.kind == EventKind::Blocked && agent.calls == 3,
              "readonly: step limit blocks with no retry");
        ScriptedAgent bad;
        bad.throw_malformed = true;
        ReadOnlyExecutor ro2(s, *f.allocator, f.runner, bad, fallback);
        check(ro2.run(Role::Research, record).event.kind == EventKind::Blocked && bad.calls == 1,
              "readonly: malformed reply blocks");
        ScriptedAgent idle;
        ReadOnlyExecutor ro3(s, *f.allocator, f.runner, idle, fallback);
        check(ro3.run(Role::Qa, record).event.kind == EventKind::Escalate && idle.calls == 0,
              "readonly: other roles fall through");
    }
    fs::remove_all(f.base);
}

}  // namespace

int main() {
    test_readonly();
    test_tools();
    test_parse();
    test_claude_agent();
    test_end_to_end();
    test_failures();
    std::cout << (failures == 0 ? "ALL PASSED\n" : "FAILURES\n");
    return failures == 0 ? 0 : 1;
}
