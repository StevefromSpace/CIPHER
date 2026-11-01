// Restart and recovery tests. Each "restart" builds a new Orchestrator on a new FileJobStore over the same
// directory, which is what a process restart looks like to the orchestrator. Scripted executors only; no model,
// process or GitHub call.

#include "cipher/orchestration/job_store_file.hpp"
#include "cipher/orchestration/orchestrator.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

using namespace cipher::orchestration;
namespace fs = std::filesystem;

int g_failures = 0;

void check(bool condition, const std::string& name) {
    if (condition) {
        std::cout << "PASS: " << name << "\n";
    } else {
        std::cout << "FAIL: " << name << "\n";
        ++g_failures;
    }
}

const std::string kHeadA = "1111111111111111111111111111111111111111";
const std::string kHeadB = "2222222222222222222222222222222222222222";
const std::string kClaude = "claude";
const int kIssue = 42;

// Crashes (throws) on the first implementation run, the way a killed process would leave the job mid-run.
// Once armed to succeed, it returns ImplementationComplete on the given head.
class CrashingExecutor final : public RoleExecutor {
public:
    bool crash_on_implementation = true;
    int implementation_calls = 0;

    RoleResult run(Role role, const JobRecord&) override {
        if (role == Role::Implementation) {
            ++implementation_calls;
            if (crash_on_implementation) throw std::runtime_error("simulated crash during implementation");
            RoleResult result;
            result.event.kind = EventKind::ImplementationComplete;
            result.event.head_sha = kHeadA;
            result.model_family = kClaude;
            return result;
        }
        return RoleResult{};
    }
};

Event event(EventKind kind, const std::string& head = "", Verdict verdict = Verdict::None, bool ci = false) {
    Event e;
    e.kind = kind;
    e.head_sha = head;
    e.verdict = verdict;
    e.ci_passed = ci;
    return e;
}

fs::path fresh_dir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

void test_restart_while_queued_keeps_the_saved_state() {
    const fs::path dir = fresh_dir("cipher-recovery-queued");
    IssueFacts facts;
    CrashingExecutor executor;
    {
        FileJobStore store(dir);
        Orchestrator orch(kIssue, facts, executor, store, Policy{});
        orch.start();
    }
    FileJobStore store(dir);
    Orchestrator restarted(kIssue, facts, executor, store, Policy{});
    check(restarted.job().state == State::Implementation && restarted.job().implementation_executions == 1,
          "restart queued: the saved state and the first counted execution are reloaded");
    fs::remove_all(dir);
}

void test_restart_during_in_flight_execution_counts_it_once() {
    const fs::path dir = fresh_dir("cipher-recovery-inflight");
    IssueFacts facts;
    CrashingExecutor executor;
    {
        FileJobStore store(dir);
        Orchestrator orch(kIssue, facts, executor, store, Policy{});
        orch.start();
        try {
            orch.advance();
        } catch (const std::runtime_error&) {
            // the process "dies" here; the in-flight marker was saved before the run
        }
    }
    FileJobStore store(dir);
    CrashingExecutor after_restart;
    Orchestrator restarted(kIssue, facts, after_restart, store, Policy{});
    check(restarted.job().execution_in_flight, "restart in flight: the in-flight marker survives the crash");

    const bool recovered = restarted.recover();
    // The crashed run is counted once (attempt 1). Recovery opens the single allowed retry as attempt 2.
    check(recovered && restarted.job().implementation_executions == 2 && !restarted.job().execution_in_flight &&
              restarted.job().state == State::Implementation,
          "restart in flight: recovery counts the crashed run once and opens only the retry as attempt 2");
    check(after_restart.implementation_calls == 0,
          "restart in flight: recovery never re-runs the implementation executor");
    fs::remove_all(dir);
}

void test_restart_after_pr_and_stale_ci_head_is_rejected() {
    const fs::path dir = fresh_dir("cipher-recovery-pr");
    IssueFacts facts;
    CrashingExecutor executor;
    executor.crash_on_implementation = false;
    {
        FileJobStore store(dir);
        Orchestrator orch(kIssue, facts, executor, store, Policy{});
        orch.start();
        orch.advance();
        orch.apply(event(EventKind::PullRequestOpened, kHeadA));
        orch.apply(event(EventKind::QaResult, kHeadA, Verdict::Approve));
    }
    FileJobStore store(dir);
    Orchestrator restarted(kIssue, facts, executor, store, Policy{});
    check(restarted.job().state == State::Ci && restarted.job().head_sha == kHeadA,
          "restart after QA: the PR head and CI state are reloaded");
    check(!restarted.apply(event(EventKind::CiResult, kHeadB, Verdict::None, true)) &&
              restarted.job().state == State::Ci,
          "restart after QA: a CI result on a stale head is rejected after reload");
    fs::remove_all(dir);
}

void test_replayed_ci_after_restart_changes_no_counters() {
    const fs::path dir = fresh_dir("cipher-recovery-ci-replay");
    IssueFacts facts;
    CrashingExecutor executor;
    executor.crash_on_implementation = false;
    {
        FileJobStore store(dir);
        Orchestrator orch(kIssue, facts, executor, store, Policy{});
        orch.start();
        orch.advance();
        orch.apply(event(EventKind::PullRequestOpened, kHeadA));
        orch.apply(event(EventKind::QaResult, kHeadA, Verdict::Approve));
        orch.apply(event(EventKind::CiResult, kHeadA, Verdict::None, true));
    }
    FileJobStore store(dir);
    Orchestrator restarted(kIssue, facts, executor, store, Policy{});
    const int executions_before = restarted.job().implementation_executions;
    const int rounds_before = restarted.job().review_rounds;
    const bool replayed = restarted.apply(event(EventKind::CiResult, kHeadA, Verdict::None, true));
    check(!replayed && restarted.job().implementation_executions == executions_before &&
              restarted.job().review_rounds == rounds_before,
          "restart replay: a CI result replayed after restart changes no counters");
    fs::remove_all(dir);
}

void test_second_crash_after_restart_exhausts_the_budget() {
    const fs::path dir = fresh_dir("cipher-recovery-exhaust");
    IssueFacts facts;
    CrashingExecutor executor;
    {
        FileJobStore store(dir);
        Orchestrator orch(kIssue, facts, executor, store, Policy{});
        orch.start();
        try {
            orch.advance();
        } catch (const std::runtime_error&) {
        }
    }
    {
        FileJobStore store(dir);
        Orchestrator restarted(kIssue, facts, executor, store, Policy{});
        restarted.recover();
        try {
            restarted.advance();  // second and last implementation execution, crashes again
        } catch (const std::runtime_error&) {
        }
    }
    FileJobStore store(dir);
    Orchestrator restarted(kIssue, facts, executor, store, Policy{});
    restarted.recover();
    check(restarted.job().state == State::NeedsHuman && restarted.job().escalation == EscalationReason::RetryBudgetExhausted,
          "restart exhaustion: a crash on the last attempt escalates after reload");
    check(restarted.job().implementation_executions == 2, "restart exhaustion: exactly two implementation executions");
    fs::remove_all(dir);
}

void test_corrupt_state_fails_closed() {
    const fs::path dir = fresh_dir("cipher-recovery-corrupt");
    IssueFacts facts;
    CrashingExecutor executor;
    {
        FileJobStore store(dir);
        Orchestrator orch(kIssue, facts, executor, store, Policy{});
        orch.start();
    }
    const fs::path state = FileJobStore(dir).path_for(kIssue);
    const std::string original = read_text(state);
    write_text(state, original.substr(0, original.size() / 2));  // partial write

    FileJobStore store(dir);
    bool threw = false;
    try {
        Orchestrator restarted(kIssue, facts, executor, store, Policy{});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "corrupt state: a partial .state makes construction throw, not reset");
    check(read_text(state) == original.substr(0, original.size() / 2),
          "corrupt state: the damaged file is left untouched, no fresh record is written over it");
    fs::remove_all(dir);
}

void test_leftover_tmp_is_ignored() {
    const fs::path dir = fresh_dir("cipher-recovery-tmp");
    IssueFacts facts;
    CrashingExecutor executor;
    {
        FileJobStore store(dir);
        Orchestrator orch(kIssue, facts, executor, store, Policy{});
        orch.start();
    }
    const fs::path state = FileJobStore(dir).path_for(kIssue);
    fs::path leftover = state;
    leftover += ".tmp";
    write_text(leftover, "garbage from a crashed write");

    FileJobStore store(dir);
    Orchestrator restarted(kIssue, facts, executor, store, Policy{});
    check(restarted.job().state == State::Implementation && restarted.job().implementation_executions == 1,
          "leftover tmp: a stale .tmp is ignored and the last committed .state is used");
    fs::remove_all(dir);
}

}  // namespace

int main() {
    test_restart_while_queued_keeps_the_saved_state();
    test_restart_during_in_flight_execution_counts_it_once();
    test_restart_after_pr_and_stale_ci_head_is_rejected();
    test_replayed_ci_after_restart_changes_no_counters();
    test_second_crash_after_restart_exhausts_the_budget();
    test_corrupt_state_fails_closed();
    test_leftover_tmp_is_ignored();
    std::cout << (g_failures == 0 ? "ALL PASSED" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
