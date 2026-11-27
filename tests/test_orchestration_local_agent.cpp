// LocalToolAgent with a scripted fake IAssistantBackend. No model, no network. The e2e cases use real git.

#include "cipher/orchestration/executable_resolver.hpp"
#include "cipher/orchestration/git_worktree_backend.hpp"
#include "cipher/orchestration/implementation_executor.hpp"
#include "cipher/orchestration/implementation_rig.hpp"
#include "cipher/orchestration/local_tool_agent.hpp"

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace {

using namespace cipher;
using namespace cipher::orchestration;
namespace fs = std::filesystem;

int failures = 0;

void check(bool ok, const char* what) {
    std::cout << (ok ? "PASS: " : "FAIL: ") << what << "\n";
    if (!ok) ++failures;
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

class FakeBackend final : public IAssistantBackend {
public:
    std::deque<AssistantResponse> replies;
    ProcessingRoute route = ProcessingRoute::VerifiedLocal;
    bool available = true;
    std::vector<std::string> prompts;
    std::string backend_id() const override { return "fake-local"; }
    bool is_available() override { return available; }
    AssistantCapabilities capabilities() const override {
        AssistantCapabilities c;
        c.processing_route = route;
        return c;
    }
    AssistantResponse execute_request(const AssistantRequest& request) override {
        prompts.push_back(request.prompt);
        if (replies.empty()) return {};
        AssistantResponse r = replies.front();
        replies.pop_front();
        return r;
    }
    void say(const std::string& text) {
        AssistantResponse r;
        r.status = AssistantStatus::Ok;
        r.text = text;
        replies.push_back(r);
    }
    void fail(AssistantStatus status) {
        AssistantResponse r;
        r.status = status;
        replies.push_back(r);
    }
};

constexpr std::size_t kBudget = 4000;

// Returns the exception text, or "" when next() succeeded.
std::string error_of(FakeBackend& backend, const std::vector<ToolTurn>& transcript = {}) {
    LocalToolAgent agent(backend, 30.0, kBudget);
    try {
        agent.next("task", transcript);
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

void test_parsing() {
    FakeBackend b;
    b.say("```json\n{\"tool\":\"fs.read\",\"path\":\"a.md\"}\n```");
    b.say("Sure! Here you go: {\"done\":true,\"summary\":\"ok\"} hope that helps");
    LocalToolAgent agent(b, 30.0, kBudget);
    AgentStep fenced = agent.next("task", {});
    check(!fenced.done && fenced.call.tool == "fs.read" && fenced.call.path == "a.md", "fenced JSON parsed");
    AgentStep prose = agent.next("task", {});
    check(prose.done && prose.summary == "ok", "prose around JSON tolerated");
    check(has(b.prompts[0], "TASK: task") && has(b.prompts[0], "fs.edit"), "prompt carries protocol and task");

    FakeBackend m;
    m.say("{\"tool\": ");
    check(!error_of(m).empty(), "malformed JSON throws");
    FakeBackend n;
    n.say("{\"tool\":\"fs.read\",\"path\":7}");
    check(!error_of(n).empty(), "non-string field throws");
    FakeBackend u;
    u.say("{\"tool\":\"rm.everything\"}");
    LocalToolAgent ua(u, 30.0, kBudget);
    check(ua.next("task", {}).call.tool == "rm.everything", "unknown tool passes through; the tool layer rejects it");
}

void test_backend_failures() {
    FakeBackend t;
    t.fail(AssistantStatus::Timeout);
    check(has(error_of(t), "local agent:"), "backend Timeout throws");
    FakeBackend u;
    u.available = false;
    check(!error_of(u).empty() && u.prompts.empty(), "unavailable backend throws without a request");
    FakeBackend r;
    r.route = ProcessingRoute::Unknown;
    r.say("{\"done\":true}");
    check(!error_of(r).empty() && r.prompts.empty(), "route not VerifiedLocal is refused");
    FakeBackend o;
    o.say("{\"done\":true}");
    LocalToolAgent tiny(o, 30.0, 50);
    bool threw = false;
    try {
        tiny.next("task", {});
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw && o.prompts.empty(), "prompt budget too small throws before any request");
}

void test_budget_and_repeat() {
    FakeBackend b;
    std::vector<ToolTurn> transcript;
    for (int i = 0; i < 50; ++i) {
        ToolTurn turn;
        turn.call.tool = "fs.read";
        turn.call.path = "f" + std::to_string(i);
        turn.outcome.output = std::string(500, 'x');
        transcript.push_back(turn);
    }
    b.say("{\"done\":true}");
    LocalToolAgent agent(b, 30.0, kBudget);
    agent.next("task", transcript);
    check(b.prompts[0].size() <= kBudget && has(b.prompts[0], "f49") && !has(b.prompts[0], "f0\n"),
          "history trimmed oldest-first to fit the budget");

    FakeBackend r;
    r.say("{\"tool\":\"fs.read\",\"path\":\"f49\"}");
    check(has(error_of(r, transcript), "repeated"), "repeat guard fails the same call twice in a row");
}

// ---- executor integration (real git, scripted backend) -------------------------------------------------------

std::string git_out(SystemProcessRunner& runner, const std::string& git, const fs::path& dir,
                    const std::vector<std::string>& args) {
    std::vector<std::string> argv{git, "-C", dir.string()};
    argv.insert(argv.end(), args.begin(), args.end());
    std::string text = runner.run(argv, 30000).stdout_text;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

class NoFallback final : public RoleExecutor {
public:
    RoleResult run(Role, const JobRecord&) override { return {Event{EventKind::Escalate}, ""}; }
};

struct Fixture {
    SystemProcessRunner runner{1 << 20};
    std::string git = resolve_executable_on_path("git");
    fs::path base;
    std::unique_ptr<GitWorktreeBackend> backend;
    std::unique_ptr<WorktreeAllocator> allocator;
    Fixture() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        base = fs::temp_directory_path() / ("cipher local agent test-" + std::to_string(stamp));
        const fs::path repo = base / "repo";
        fs::create_directories(repo);
        runner.run({git, "-C", repo.string(), "init", "-q"}, 30000);
        std::ofstream(repo / "README.txt") << "probe\n";
        runner.run({git, "-C", repo.string(), "add", "README.txt"}, 30000);
        runner.run({git, "-C", repo.string(), "-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit",
                    "-q", "-m", "base"},
                   30000);
        const std::string sha = git_out(runner, git, repo, {"rev-parse", "HEAD"});
        backend = std::make_unique<GitWorktreeBackend>(git, repo, sha, base / "worktrees", runner);
        allocator = std::make_unique<WorktreeAllocator>(base / "worktrees", *backend);
    }
    ImplementationSettings settings(int issue, int max_steps) const {
        ImplementationSettings s;
        s.issue_number = issue;
        s.task = "add a docs note";
        s.git_executable = git;
        s.model_family = "fake";
        s.max_steps = max_steps;
        s.commands["check"] = {git, "-C", (base / "worktrees" / ("job-" + std::to_string(issue))).string(), "diff",
                               "--check"};
        s.verify_command_id = "check";
        s.evidence_file = base / "evidence.log";
        return s;
    }
};

EventKind run_executor(Fixture& f, FakeBackend& b, int issue, int max_steps) {
    NoFallback fallback;
    LocalToolAgent agent(b, 30.0, kBudget);
    ImplementationExecutor executor(f.settings(issue, max_steps), *f.allocator, f.runner, agent, fallback);
    JobRecord record;
    record.state = State::Implementation;
    return executor.run(Role::Implementation, record).event.kind;
}

void test_executor() {
    {
        Fixture f;
        FakeBackend b;
        b.say("{\"tool\":\"fs.list\",\"path\":\".\"}");
        b.say("```json\n{\"tool\":\"fs.write\",\"path\":\"docs/NOTE.md\",\"content\":\"# Note\\nhi\\n\"}\n```");
        b.say("{\"tool\":\"fs.edit\",\"path\":\"docs/NOTE.md\",\"old_text\":\"hi\",\"new_text\":\"hello\"}");
        b.say("{\"done\":true,\"summary\":\"added note\"}");
        check(run_executor(f, b, 31, 6) == EventKind::ImplementationComplete, "e2e: read-edit-done completes");
        const fs::path wt = f.base / "worktrees" / "job-31";
        check(has(git_out(f.runner, f.git, wt, {"show", "--stat", "--format=%s", "HEAD"}), "docs/NOTE.md"),
              "e2e: commit contains the change");
        check(has(b.prompts.back(), "fs.edit docs/NOTE.md"), "e2e: later prompts carry the history");
        fs::remove_all(f.base);
    }
    {
        Fixture f;
        FakeBackend b;
        b.say("not json at all");
        check(run_executor(f, b, 32, 6) == EventKind::ImplementationFailed, "e2e: malformed reply fails the execution");
        fs::remove_all(f.base);
    }
    {
        Fixture f;
        FakeBackend b;
        b.say("{\"tool\":\"fs.list\",\"path\":\".\"}");
        b.say("{\"tool\":\"fs.list\",\"path\":\"docs\"}");
        b.say("{\"tool\":\"fs.list\",\"path\":\"src\"}");
        check(run_executor(f, b, 33, 2) == EventKind::ImplementationFailed && b.prompts.size() == 2,
              "e2e: step limit stops after max_steps requests");
        fs::remove_all(f.base);
    }
}

// C4: the production rig in local mode over a scripted backend (docs-only writes, family, retry history, availability).
void test_rig_local_mode() {
    Fixture f;
    FakeBackend b;
    RigConfig config;
    config.repository = f.base / "repo";
    config.base_sha = git_out(f.runner, f.git, f.base / "repo", {"rev-parse", "HEAD"});
    config.worktree_root = f.base / "worktrees";
    config.agent = "local";
    config.local_backend = &b;
    config.local_family = "ollama";

    b.say("{\"tool\":\"fs.write\",\"path\":\"src/x.cpp\",\"content\":\"x\"}");
    {
        ImplementationRig rig(config, 41, "add a docs note");
        JobRecord record;
        record.state = State::Implementation;
        const RoleResult failed = rig.run(Role::Implementation, record);
        check(failed.event.kind == EventKind::ImplementationFailed && failed.model_family == "ollama",
              "rig: non-docs write is denied; family comes from config");
        b.available = false;
        check(!rig.dispatchable(Role::Implementation, record), "rig: unavailable backend is not dispatchable");
        b.available = true;
        check(rig.dispatchable(Role::Implementation, record), "rig: available backend is dispatchable");
    }
    b.say("{\"tool\":\"fs.write\",\"path\":\"docs/NOTE.md\",\"content\":\"hi\\n\"}");
    b.say("{\"done\":true,\"summary\":\"ok\"}");
    const std::size_t first_retry_prompt = b.prompts.size();
    ImplementationRig retry(config, 41, "add a docs note");
    JobRecord record;
    record.state = State::Implementation;
    check(retry.run(Role::Implementation, record).event.kind == EventKind::ImplementationComplete,
          "rig: scripted docs-only run completes");
    check(has(b.prompts[first_retry_prompt], "PREVIOUS ATTEMPT") && has(b.prompts[first_retry_prompt], "FAILED"),
          "rig: retry prompt carries the prior evidence");
    fs::remove_all(f.base);
}

}  // namespace

int main() {
    test_parsing();
    test_backend_failures();
    test_budget_and_repeat();
    test_executor();
    test_rig_local_mode();
    std::cout << (failures == 0 ? "ALL PASSED\n" : "FAILURES\n");
    return failures == 0 ? 0 : 1;
}
