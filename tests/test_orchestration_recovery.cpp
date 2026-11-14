// Restart and recovery tests. Each "restart" builds a new Orchestrator on a new FileJobStore over the same
// directory, which is what a process restart looks like to the orchestrator. Scripted executors only; no model,
// process or GitHub call.

#include "cipher/orchestration/event_log.hpp"
#include "cipher/orchestration/job_store_file.hpp"
#include "cipher/orchestration/orchestrator.hpp"

#include <algorithm>
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
    check(restarted.job().state == State::Implementation && restarted.job().implementation_executions == 0,
          "restart queued: the saved state is reloaded and nothing is charged before dispatch");
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
    // The crashed run was charged at dispatch (attempt 1). Recovery adds no charge; the retry is charged when dispatched.
    check(recovered && restarted.job().implementation_executions == 1 && !restarted.job().execution_in_flight &&
              restarted.job().state == State::Implementation,
          "restart in flight: recovery keeps the single dispatch charge and clears the flag, with no new charge");
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
    check(restarted.job().state == State::Implementation && restarted.job().implementation_executions == 0,
          "leftover tmp: a stale .tmp is ignored and the last committed .state is used");
    fs::remove_all(dir);
}

// Real store that dies on the Nth save from now, the way a killed process leaves the previous file intact.
class DyingStore final : public JobStore {
public:
    explicit DyingStore(const fs::path& dir) : inner_(dir) {}
    int saves_until_death = -1;  // -1 = never
    void save(int issue, const JobRecord& job) override {
        if (saves_until_death == 0) throw std::runtime_error("simulated crash during save");
        if (saves_until_death > 0) --saves_until_death;
        inner_.save(issue, job);
    }
    std::optional<JobRecord> load(int issue) const override { return inner_.load(issue); }

private:
    FileJobStore inner_;
};

// Leaves a job crashed mid-execution on disk: Implementation, in flight, one counted run.
void crash_mid_run(const fs::path& dir, EventLog* log = nullptr, const std::string& run_id = "") {
    IssueFacts facts;
    CrashingExecutor executor;
    FileJobStore store(dir);
    Orchestrator orch(kIssue, facts, executor, store, Policy{});
    if (log != nullptr) orch.attach_event_log(*log);
    orch.assign_run_id(run_id);
    orch.start();
    try {
        orch.advance();
    } catch (const std::runtime_error&) {
    }
}

void test_recover_interrupted_before_completion_can_be_retried() {
    const fs::path dir = fresh_dir("cipher-recovery-interrupted");
    crash_mid_run(dir);
    IssueFacts facts;
    CrashingExecutor executor;
    {
        DyingStore store(dir);
        Orchestrator orch(kIssue, facts, executor, store, Policy{});
        store.saves_until_death = 0;  // the single atomic save in recover() dies
        try {
            orch.recover();
        } catch (const std::runtime_error&) {
        }
    }
    FileJobStore after_crash(dir);
    const JobRecord on_disk = *after_crash.load(kIssue);
    check(on_disk.execution_in_flight && on_disk.implementation_executions == 1,
          "recover interrupted: disk still shows the run in flight and uncharged (nothing half-applied)");
    Orchestrator again(kIssue, facts, executor, after_crash, Policy{});
    check(again.recover() && again.job().implementation_executions == 1 && !again.job().execution_in_flight,
          "recover interrupted: a second recover clears the flag and adds no second charge");
    fs::remove_all(dir);
}

void test_recover_repeated_changes_nothing_and_preserves_history() {
    const fs::path dir = fresh_dir("cipher-recovery-repeat");
    const fs::path log_file = dir / "events.log";
    EventLog log(log_file);
    crash_mid_run(dir, &log, "run-42-test");
    IssueFacts facts;
    CrashingExecutor executor;
    FileJobStore store(dir);
    Orchestrator orch(kIssue, facts, executor, store, Policy{});
    orch.attach_event_log(log);
    const std::vector<std::string> before = log.read_all();
    check(orch.recover(), "recover repeated: first recover succeeds");
    const std::vector<std::string> after_first = log.read_all();
    const JobRecord first = orch.job();
    check(after_first.size() == before.size() + 1 &&
              std::equal(before.begin(), before.end(), after_first.begin()),
          "recover repeated: exactly one new event appended, earlier events unchanged");

    check(!orch.recover(), "recover repeated: second recover reports nothing to do");
    FileJobStore reread(dir);
    Orchestrator restarted(kIssue, facts, executor, reread, Policy{});
    check(!restarted.recover(), "recover repeated: a restarted process also finds nothing to do");
    check(log.read_all() == after_first, "recover repeated: no events added by repeats");
    const JobRecord now = *reread.load(kIssue);
    check(serialize_job(now) == serialize_job(first), "recover repeated: persisted record is byte-identical");
    check(now.run_id == "run-42-test", "recover repeated: run_id preserved");
    fs::remove_all(dir);
}

void test_recover_leaves_existing_worktree_and_branch_dirs_alone() {
    const fs::path dir = fresh_dir("cipher-recovery-worktree");
    crash_mid_run(dir);
    const fs::path worktree = dir / "worktrees" / "job-42";
    fs::create_directories(worktree);
    write_text(worktree / "work.txt", "uncommitted work");
    IssueFacts facts;
    CrashingExecutor executor;
    FileJobStore store(dir);
    Orchestrator orch(kIssue, facts, executor, store, Policy{});
    orch.recover();
    orch.recover();
    check(read_text(worktree / "work.txt") == "uncommitted work" && fs::exists(worktree),
          "recover: an existing worktree directory and its files are untouched");
    check(executor.implementation_calls == 0, "recover: no executor is run, so no commit or branch can be created");
    fs::remove_all(dir);
}

void test_recover_inconsistent_record_is_left_unchanged() {
    const fs::path dir = fresh_dir("cipher-recovery-inconsistent");
    crash_mid_run(dir);
    {
        FileJobStore store(dir);
        JobRecord job = *store.load(kIssue);
        job.state = State::Qa;  // in flight, but not in Implementation: cannot come from a real crash
        store.save(kIssue, job);
    }
    IssueFacts facts;
    CrashingExecutor executor;
    FileJobStore store(dir);
    Orchestrator orch(kIssue, facts, executor, store, Policy{});
    const std::string before = serialize_job(*store.load(kIssue));
    check(!orch.recover(), "recover inconsistent: refused");
    check(serialize_job(*store.load(kIssue)) == before && store.load(kIssue)->execution_in_flight,
          "recover inconsistent: the stored record is unchanged and still flagged for the owner");
    check(orch.job().execution_in_flight && orch.job().state == State::Qa,
          "recover inconsistent: in-memory job matches disk");
    fs::remove_all(dir);
}

// The job record is authoritative and is saved before its event is logged. A failed append after a successful
// save must not undo the transition, repeat it, or hide the failure; the missing line is documented as lost.
void test_event_log_failure_after_save_is_reported_without_repeating_the_transition() {
    const fs::path dir = fresh_dir("cipher-recovery-logfail");
    const fs::path log_file = dir / "events.log";
    EventLog good(log_file);
    crash_mid_run(dir, &good, "run-42-test");
    const std::vector<std::string> history = good.read_all();

    // A log whose file path is a directory: the folder exists, but opening it for append fails.
    fs::create_directories(dir / "bad" / "events.log");
    EventLog bad(dir / "bad" / "events.log");

    IssueFacts facts;
    CrashingExecutor executor;
    {
        FileJobStore store(dir);
        Orchestrator orch(kIssue, facts, executor, store, Policy{});
        orch.attach_event_log(bad);
        check(orch.recover(), "log failure: the recovery itself succeeds, it is not turned into an error");
        check(orch.log_error().find("cannot append to") != std::string::npos,
              "log failure: the failed append is reported through log_error(), not swallowed");
        check(orch.records().size() == 1, "log failure: the in-memory execution record still exists");
    }
    FileJobStore reread(dir);
    const JobRecord saved = *reread.load(kIssue);
    check(saved.implementation_executions == 1 && !saved.execution_in_flight && saved.run_id == "run-42-test",
          "log failure: the saved record has the charge, the cleared flag and the run_id");
    check(good.read_all() == history, "log failure: the earlier event history is untouched");

    // A restarted process cannot see the lost line and must not repeat the transition to recreate it.
    const std::string before = serialize_job(saved);
    Orchestrator restarted(kIssue, facts, executor, reread, Policy{});
    restarted.attach_event_log(good);
    check(!restarted.recover(), "log failure restart: recover reports nothing in flight");
    check(restarted.log_error().empty(), "log failure restart: a healthy log reports no error");
    check(serialize_job(*reread.load(kIssue)) == before, "log failure restart: the record is unchanged, no second charge");
    check(good.read_all() == history, "log failure restart: no event was added or duplicated");
    check(executor.implementation_calls == 0, "log failure restart: the executor never ran");
    fs::remove_all(dir);
}

void test_crash_before_dispatch_then_retry_dispatches_once() {
    const fs::path dir = fresh_dir("cipher-recovery-predispatch");
    IssueFacts facts;
    CrashingExecutor executor;
    executor.crash_on_implementation = false;
    {
        DyingStore store(dir);
        Orchestrator orch(kIssue, facts, executor, store, Policy{});
        orch.start();
        store.saves_until_death = 0;  // the in-flight flag save never reaches disk
        bool crashed = false;
        try {
            orch.advance();
        } catch (const std::runtime_error&) {
            crashed = true;
        }
        check(crashed, "pre-dispatch: the flag save fails");
        check(executor.implementation_calls == 0, "pre-dispatch: the executor is not called without a persisted flag");
        check(!orch.job().execution_in_flight, "pre-dispatch: memory matches disk, no flag");
    }
    FileJobStore store(dir);
    const JobRecord saved = *store.load(kIssue);
    check(!saved.execution_in_flight && saved.implementation_executions == 0,
          "pre-dispatch: the failed flag save left no charge and no flag on disk");
    Orchestrator restarted(kIssue, facts, executor, store, Policy{});
    check(restarted.advance(), "pre-dispatch retry: the step runs");
    check(executor.implementation_calls == 1, "pre-dispatch retry: the executor runs exactly once");
    check(restarted.job().implementation_executions == 1, "pre-dispatch retry: exactly one charge, made at dispatch");
    fs::remove_all(dir);
}

void test_flagged_record_is_not_redispatched_without_recover() {
    const fs::path dir = fresh_dir("cipher-recovery-flagged-advance");
    IssueFacts facts;
    CrashingExecutor executor;
    {
        FileJobStore store(dir);
        Orchestrator orch(kIssue, facts, executor, store, Policy{});
        orch.start();
        try {
            orch.advance();  // dispatches, then crashes: the flag stays on disk
        } catch (const std::runtime_error&) {
        }
    }
    FileJobStore store(dir);
    CrashingExecutor after_restart;
    after_restart.crash_on_implementation = false;
    Orchestrator restarted(kIssue, facts, after_restart, store, Policy{});
    check(!restarted.advance(), "flagged advance: a flagged record takes no step");
    check(after_restart.implementation_calls == 0, "flagged advance: the executor is not called");
    check(restarted.job().execution_in_flight, "flagged advance: the flag is still set");
    fs::remove_all(dir);
}

// Writes a legacy (schema 2) record that was charged on entry and waits in Implementation, unflagged.
void write_legacy_waiting_record(const fs::path& dir, int count) {
    JobRecord job;
    job.state = State::Implementation;
    job.implementation_executions = count;
    job.run_id = "run-42-20261101T000000Z";
    std::string text = serialize_job(job);
    text.replace(text.find("schema=3"), 8, "schema=2");
    text.erase(text.find("accounting_unreconciled="));
    write_text(FileJobStore(dir).path_for(kIssue), text);
}

void test_legacy_record_refuses_dispatch_until_reconciled() {
    const fs::path dir = fresh_dir("cipher-recovery-legacy-refuse");
    write_legacy_waiting_record(dir, 1);
    FileJobStore store(dir);
    CrashingExecutor executor;
    executor.crash_on_implementation = false;
    IssueFacts facts;
    Orchestrator orch(kIssue, facts, executor, store, Policy{});
    check(orch.job().accounting_unreconciled && orch.job().implementation_executions == 1,
          "legacy: the load keeps the historical count and flags the record");
    check(!orch.advance(), "legacy: an unreconciled record takes no step");
    check(executor.implementation_calls == 0, "legacy: the executor is not called before reconcile");
    check(store.load(kIssue)->implementation_executions == 1, "legacy: refusing dispatch leaves the disk count unchanged");
    fs::remove_all(dir);
}

void test_reconcile_default_keeps_count_and_never_double_charges() {
    const fs::path dir = fresh_dir("cipher-recovery-reconcile-k");
    write_legacy_waiting_record(dir, 1);
    FileJobStore store(dir);
    CrashingExecutor executor;
    executor.crash_on_implementation = false;
    IssueFacts facts;
    Orchestrator orch(kIssue, facts, executor, store, Policy{});
    check(orch.reconcile(false, ""), "reconcile k: the marker is cleared");
    check(orch.job().implementation_executions == 1, "reconcile k: the historical count is kept");
    check(store.load(kIssue)->implementation_executions == 1 && !store.load(kIssue)->accounting_unreconciled,
          "reconcile k: disk agrees");
    check(!orch.reconcile(false, ""), "reconcile k: a second reconcile is a no-op");
    check(orch.advance(), "reconcile k: dispatch is allowed afterwards");
    check(orch.job().implementation_executions == 2, "reconcile k: the next dispatch is charged exactly once more");
    fs::remove_all(dir);
}

void test_reconcile_never_dispatched_needs_evidence_and_refunds_one() {
    const fs::path dir = fresh_dir("cipher-recovery-reconcile-km1");
    write_legacy_waiting_record(dir, 1);
    FileJobStore store(dir);
    CrashingExecutor executor;
    IssueFacts facts;
    Orchestrator orch(kIssue, facts, executor, store, Policy{});
    bool threw = false;
    try {
        orch.reconcile(true, "");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "reconcile k-1: refused without evidence");
    check(orch.job().accounting_unreconciled && orch.job().implementation_executions == 1,
          "reconcile k-1: a refused request changes nothing");
    check(orch.reconcile(true, "operator confirmed no execution ran"), "reconcile k-1: accepted with evidence");
    check(orch.job().implementation_executions == 0, "reconcile k-1: the count drops by exactly one");
    check(store.load(kIssue)->implementation_executions == 0, "reconcile k-1: disk agrees");

    write_legacy_waiting_record(dir, 0);  // count 0 is not ambiguous, so k-1 cannot go below zero
    FileJobStore again(dir);
    Orchestrator zero(kIssue, facts, executor, again, Policy{});
    check(!zero.reconcile(true, "evidence"), "reconcile k-1: nothing to settle at count zero");
    fs::remove_all(dir);
}

void test_failed_final_save_does_not_advance_memory() {
    const fs::path dir = fresh_dir("cipher-recovery-finalsave");
    IssueFacts facts;
    CrashingExecutor executor;
    executor.crash_on_implementation = false;
    DyingStore store(dir);
    Orchestrator orch(kIssue, facts, executor, store, Policy{});
    orch.assign_run_id("run-42-test");
    orch.start();
    store.saves_until_death = 1;  // flag save passes, the save after the executor dies
    bool crashed = false;
    try {
        orch.advance();
    } catch (const std::runtime_error&) {
        crashed = true;
    }
    check(crashed, "final save: the transition save fails");
    check(executor.implementation_calls == 1, "final save: the executor ran once before the failed save");
    check(orch.job().state == State::Implementation && orch.job().execution_in_flight,
          "final save: memory does not advance past a transition the disk lacks");
    check(orch.records().size() == 1 && orch.records().front().event == EventKind::Start,
          "final save: no transition record was added for the failed save");
    check(orch.job().run_id == "run-42-test", "final save: run_id is unchanged");

    store.saves_until_death = -1;  // the simulated crash is over; the process restarts with a working disk
    const bool recovered = orch.recover();  // the flag is still on disk: clears it, no new charge, no dispatch
    check(recovered && orch.job().implementation_executions == 1 && !orch.job().execution_in_flight,
          "final save: recovery clears the flag without a second charge");
    check(executor.implementation_calls == 1, "final save: recovery never re-runs the executor");
    fs::remove_all(dir);
}

// A save fault at a chosen point: the Nth save from now throws, either before the write or after it lands.
class FaultStore final : public JobStore {
public:
    explicit FaultStore(const fs::path& dir) : inner_(dir) {}
    int countdown = -1;          // -1 = never; 0 = the next save faults; n = that many saves pass first
    bool write_then_throw = false;
    void save(int issue, const JobRecord& job) override {
        if (countdown == 0) {
            countdown = -1;
            if (write_then_throw) inner_.save(issue, job);
            throw std::runtime_error("simulated fault during save");
        }
        if (countdown > 0) --countdown;
        inner_.save(issue, job);
    }
    std::optional<JobRecord> load(int issue) const override { return inner_.load(issue); }

private:
    FileJobStore inner_;
};

// Final save writes the transition and then throws. The same object must adopt the durable state, not overwrite it.
void test_final_save_write_then_throw_is_adopted_not_reverted() {
    const fs::path dir = fresh_dir("cipher-writethrow-final");
    IssueFacts facts;
    CrashingExecutor executor;
    executor.crash_on_implementation = false;
    FaultStore store(dir);
    Orchestrator orch(kIssue, facts, executor, store, Policy{});
    orch.assign_run_id("run-wt-final");
    orch.start();
    store.countdown = 1;  // reservation save passes, the final save faults after writing
    store.write_then_throw = true;
    bool advanced = false;
    try {
        advanced = orch.advance();
    } catch (const std::runtime_error&) {
    }
    check(advanced, "write-then-throw final: the durable transition is adopted and advance reports it");
    check(orch.job().state == State::PullRequest && !orch.job().execution_in_flight,
          "write-then-throw final: memory shows the landed PullRequest state, unflagged");
    check(orch.job().implementation_executions == 1 && orch.job().run_id == "run-wt-final",
          "write-then-throw final: one charge and run_id preserved");
    check(!orch.recover() && executor.implementation_calls == 1,
          "write-then-throw final: recover() on the same object is a no-op and never re-runs the executor");

    FileJobStore reopened(dir);
    Orchestrator restarted(kIssue, facts, executor, reopened, Policy{});
    check(restarted.job().state == State::PullRequest && restarted.job().implementation_executions == 1,
          "write-then-throw final: a restarted process sees the same durable PullRequest state");
    fs::remove_all(dir);
}

// Reservation save writes the flag and charge and then throws. Nothing may dispatch; the same object recovers.
void test_reservation_write_then_throw_blocks_dispatch_and_recovers() {
    const fs::path dir = fresh_dir("cipher-writethrow-reserve");
    IssueFacts facts;
    CrashingExecutor executor;
    executor.crash_on_implementation = false;
    FaultStore store(dir);
    Orchestrator orch(kIssue, facts, executor, store, Policy{});
    orch.assign_run_id("run-wt-reserve");
    orch.start();
    store.countdown = 0;
    store.write_then_throw = true;
    bool threw = false;
    try {
        orch.advance();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw && executor.implementation_calls == 0,
          "write-then-throw reservation: the executor never runs when the reservation save throws");
    check(orch.job().execution_in_flight && orch.job().implementation_executions == 1,
          "write-then-throw reservation: memory matches the landed flag and charge");
    check(!orch.advance() && executor.implementation_calls == 0,
          "write-then-throw reservation: a flagged record refuses dispatch");
    check(orch.recover() && !orch.job().execution_in_flight && orch.job().implementation_executions == 1,
          "write-then-throw reservation: recover() clears the flag without a second charge");
    fs::remove_all(dir);
}

// Final save throws before writing. Memory must match the disk (still flagged), and recovery must work.
void test_final_save_fault_before_write_keeps_memory_equal_to_disk() {
    const fs::path dir = fresh_dir("cipher-fault-before-final");
    IssueFacts facts;
    CrashingExecutor executor;
    executor.crash_on_implementation = false;
    FaultStore store(dir);
    Orchestrator orch(kIssue, facts, executor, store, Policy{});
    orch.assign_run_id("run-before-final");
    orch.start();
    store.countdown = 1;
    store.write_then_throw = false;
    bool threw = false;
    try {
        orch.advance();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw && executor.implementation_calls == 1,
          "fault before write: the executor ran once and the final save threw");
    check(orch.job().state == State::Implementation && orch.job().execution_in_flight,
          "fault before write: memory is the durable flagged Implementation state");
    check(orch.recover() && orch.job().implementation_executions == 1 && executor.implementation_calls == 1,
          "fault before write: recover() charges nothing extra and never re-runs the executor");
    fs::remove_all(dir);
}

}  // namespace

int main() {
    test_legacy_record_refuses_dispatch_until_reconciled();
    test_reconcile_default_keeps_count_and_never_double_charges();
    test_reconcile_never_dispatched_needs_evidence_and_refunds_one();
    test_final_save_write_then_throw_is_adopted_not_reverted();
    test_reservation_write_then_throw_blocks_dispatch_and_recovers();
    test_final_save_fault_before_write_keeps_memory_equal_to_disk();
    test_recover_interrupted_before_completion_can_be_retried();
    test_recover_repeated_changes_nothing_and_preserves_history();
    test_recover_leaves_existing_worktree_and_branch_dirs_alone();
    test_recover_inconsistent_record_is_left_unchanged();
    test_event_log_failure_after_save_is_reported_without_repeating_the_transition();
    test_restart_while_queued_keeps_the_saved_state();
    test_restart_during_in_flight_execution_counts_it_once();
    test_restart_after_pr_and_stale_ci_head_is_rejected();
    test_replayed_ci_after_restart_changes_no_counters();
    test_second_crash_after_restart_exhausts_the_budget();
    test_corrupt_state_fails_closed();
    test_leftover_tmp_is_ignored();
    test_crash_before_dispatch_then_retry_dispatches_once();
    test_flagged_record_is_not_redispatched_without_recover();
    test_failed_final_save_does_not_advance_memory();
    std::cout << (g_failures == 0 ? "ALL PASSED" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
