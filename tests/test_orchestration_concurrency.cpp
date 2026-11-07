// Multi-job scheduling tests. These drive several jobs through a deterministic tick loop on a real FileJobStore,
// one tick at a time, in issue order. There are no threads: "concurrent" means several jobs hold slots at once,
// and a restart is simulated by building new store objects over the same directory. Fakes only; no model,
// process or GitHub call.

#include "cipher/orchestration/governed_merge.hpp"
#include "cipher/orchestration/job_store_file.hpp"
#include "cipher/orchestration/orchestrator.hpp"
#include "cipher/orchestration/scheduler.hpp"
#include "cipher/orchestration/worktree_allocator.hpp"

#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

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

fs::path fresh_dir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// Implementation completes on kHeadA. Every other role returns an empty result, which the state machine rejects.
class ImplementingExecutor final : public RoleExecutor {
public:
    RoleResult run(Role role, const JobRecord&) override {
        RoleResult result;
        result.model_family = kClaude;
        if (role == Role::Implementation) {
            result.event.kind = EventKind::ImplementationComplete;
            result.event.head_sha = kHeadA;
        }
        return result;
    }
};

// One tick: rebuild every job's scheduler input from the store (never from memory), plan, then start admitted jobs.
// deps maps each issue to its structured dependencies. Escalations are applied by the test, not by the tick.
std::vector<Decision> tick(const fs::path& dir, const std::map<int, std::vector<int>>& deps,
                           const std::function<RoleExecutor&(int)>& executor_for,
                           GlobalStop stop = GlobalStop::None) {
    FileJobStore store(dir);
    std::vector<SchedulerJob> jobs;
    for (const auto& [issue, depends_on] : deps) {
        SchedulerJob job;
        job.issue_number = issue;
        job.depends_on = depends_on;
        const auto saved = store.load(issue);
        job.state = saved ? saved->state : State::Issue;
        jobs.push_back(job);
    }
    const std::vector<Decision> decisions = plan_schedule(jobs, kDefaultMaxParallelJobs, stop);
    for (const Decision& decision : decisions) {
        if (decision.action != Action::Admit) continue;
        Orchestrator orch(decision.issue_number, IssueFacts{}, executor_for(decision.issue_number), store, Policy{});
        orch.start();
        orch.advance();  // implementation completes; the job now holds a slot in PullRequest
    }
    return decisions;
}

std::vector<Decision> tick(const fs::path& dir, const std::map<int, std::vector<int>>& deps, RoleExecutor& executor,
                           GlobalStop stop = GlobalStop::None) {
    return tick(dir, deps, [&executor](int) -> RoleExecutor& { return executor; }, stop);
}

const Decision* find(const std::vector<Decision>& decisions, int issue) {
    for (const Decision& decision : decisions) {
        if (decision.issue_number == issue) return &decision;
    }
    return nullptr;
}

void test_fourth_job_waits_and_restart_cannot_exceed_the_limit() {
    const fs::path dir = fresh_dir("cipher-concurrency-cap");
    const std::map<int, std::vector<int>> deps = {{1, {}}, {2, {}}, {3, {}}, {4, {}}};
    ImplementingExecutor executor;

    const auto first = tick(dir, deps, executor);
    check(find(first, 1) && find(first, 1)->action == Action::Admit &&
              find(first, 2)->action == Action::Admit && find(first, 3)->action == Action::Admit,
          "cap: the first three jobs are admitted in issue order");
    check(find(first, 4) && find(first, 4)->action == Action::Wait, "cap: the fourth job waits for a slot");

    // Restart: a new process rebuilds the scheduler input from disk. Three jobs still hold slots, so no admit.
    const auto after_restart = tick(dir, deps, executor);
    check(after_restart.size() == 1 && after_restart.front().issue_number == 4 &&
              after_restart.front().action == Action::Wait,
          "cap: after a restart the fourth job still waits, the limit is not exceeded");
    fs::remove_all(dir);
}

void test_needs_human_frees_a_slot_without_stopping_the_others() {
    const fs::path dir = fresh_dir("cipher-concurrency-human");
    const std::map<int, std::vector<int>> deps = {{1, {}}, {2, {}}, {3, {}}, {4, {}}};
    ImplementingExecutor executor;
    tick(dir, deps, executor);

    {
        FileJobStore store(dir);
        Orchestrator first(1, IssueFacts{}, executor, store, Policy{});
        Event escalate;
        escalate.kind = EventKind::Escalate;
        escalate.reason = EscalationReason::ExplicitRequest;
        check(first.apply(escalate) && first.job().state == State::NeedsHuman,
              "human: escalation moves job 1 to NeedsHuman");
    }

    const auto next = tick(dir, deps, executor);
    check(next.size() == 1 && next.front().issue_number == 4 && next.front().action == Action::Admit,
          "human: the freed slot admits the waiting fourth job");
    FileJobStore store(dir);
    check(store.load(2)->state == State::PullRequest && store.load(3)->state == State::PullRequest,
          "human: the other running jobs are unaffected by the escalation");
    fs::remove_all(dir);
}

void test_dependency_gating() {
    const fs::path dir = fresh_dir("cipher-concurrency-deps");
    ImplementingExecutor executor;

    // 11 depends on 10. 10 is still queued, so 11 waits; 10 is admitted.
    const std::map<int, std::vector<int>> waiting = {{10, {}}, {11, {10}}};
    const auto first = tick(dir, waiting, executor);
    check(find(first, 10) && find(first, 10)->action == Action::Admit && find(first, 11) &&
              find(first, 11)->action == Action::Wait,
          "deps: a job waits while its dependency has not reached NextIssue");

    // Pure checks for the escalation cases, on explicit states.
    std::vector<SchedulerJob> ready = {{20, State::NextIssue, {}, false, false}, {21, State::Issue, {20}, false, false}};
    const auto complete = plan_schedule(ready);
    check(find(complete, 21) && find(complete, 21)->action == Action::Admit,
          "deps: a dependency at NextIssue releases the dependent job");

    std::vector<SchedulerJob> unknown = {{30, State::Issue, {99}, false, false}};
    const auto unknown_decisions = plan_schedule(unknown);
    check(find(unknown_decisions, 30) && find(unknown_decisions, 30)->action == Action::Escalate &&
              find(unknown_decisions, 30)->reason == EscalationReason::Ambiguity,
          "deps: an unknown dependency escalates as ambiguity");

    std::vector<SchedulerJob> blocked = {{40, State::NeedsHuman, {}, false, false}, {41, State::Issue, {40}, false, false}};
    const auto human = plan_schedule(blocked);
    check(find(human, 41) && find(human, 41)->action == Action::Escalate && find(human, 41)->reason == EscalationReason::Ambiguity,
          "deps: a dependency in NeedsHuman escalates the dependent job");
    fs::remove_all(dir);
}

void test_unrecorded_worktree_conflict_stops_all_admission() {
    struct CountingBackend final : WorktreeBackend {
        int adds = 0;
        void add(const fs::path& path, int) override {
            ++adds;
            fs::create_directories(path);
        }
        void remove(const fs::path& path, int) override { fs::remove_all(path); }
    };

    const fs::path root = fresh_dir("cipher-concurrency-worktrees") / "worktrees";
    fs::create_directories(root / "job-9");  // a directory with no ownership record
    CountingBackend backend;
    WorktreeAllocator allocator(root, backend);

    check(allocator.allocate(9) == WorktreeOutcome::Conflict && backend.adds == 0,
          "worktree: an unrecorded directory is a conflict and is never reused");

    const std::vector<SchedulerJob> queued = {{1, State::Issue, {}, false, false}, {2, State::Issue, {}, false, false}};
    check(plan_schedule(queued, kDefaultMaxParallelJobs, GlobalStop::WorktreeConflict).empty(),
          "worktree: a global stop admits nothing");
    check(!plan_schedule(queued).empty(), "worktree: without the global stop the same queue is planned");

    check(allocator.allocate(8) == WorktreeOutcome::Created && backend.adds == 1,
          "worktree: a fresh job gets its own worktree once");
    check(allocator.allocate(8) == WorktreeOutcome::Reused && backend.adds == 1,
          "worktree: a recorded job reuses its worktree without adding another");
    fs::remove_all(root.parent_path());
}

void test_merge_lock_serializes_and_gates_are_reevaluated() {
    const fs::path dir = fresh_dir("cipher-concurrency-merge");
    MergeLock lock(dir);

    MergeGateInputs job_a;
    job_a.pr_open = true;
    job_a.head_matches_gated_sha = true;
    job_a.mergeable = true;
    job_a.ci_all_required_passed = true;
    job_a.qa_approved_or_not_applicable = true;
    job_a.reviewer_approved = true;
    job_a.security_not_applicable_or_approved = true;
    job_a.no_open_blocker_or_high = true;
    job_a.scope_accepted = true;
    job_a.no_protected_content = true;

    check(lock.try_acquire(1) && lock.holder() == 1, "merge: job A takes the merge lock");
    check(!lock.try_acquire(2) && lock.holder() == 1, "merge: job B cannot take the lock while A holds it");
    lock.release(2);
    check(lock.holder() == 1, "merge: a release by a non-holder leaves the lock in place");

    check(evaluate_merge_gates(job_a).allowed, "merge: A passes its gates on its gated head");
    lock.release(1);  // A has merged; the lock is free

    // B was gated before A merged. The base has moved, so B must be evaluated again on the new state.
    MergeGateInputs job_b = job_a;
    check(evaluate_merge_gates(job_b).allowed, "merge: B's stale gate result says it may merge");
    job_b.mergeable = false;  // re-read after A merged: now conflicts with the new base
    const MergeDecision reevaluated = evaluate_merge_gates(job_b);
    check(!reevaluated.allowed && reevaluated.block == MergeBlock::NotMergeable,
          "merge: B is blocked when re-evaluated after A merged");
    check(lock.try_acquire(2) && lock.holder() == 2, "merge: the lock passes to B only after A releases");
    lock.release(2);
    fs::remove_all(dir);
}

void test_stale_merge_lock_blocks_until_a_human_clears_it() {
    const fs::path dir = fresh_dir("cipher-concurrency-stale-lock");
    MergeLock lock(dir);
    check(lock.try_acquire(5), "stale lock: job 5 takes the lock and then 'crashes' without releasing");

    // Documented limitation: a stale merge.lock is never auto-deleted. Every later merge is blocked until a human removes it.
    check(!lock.try_acquire(6) && lock.holder() == 5,
          "stale lock: job 6 is blocked and the stale holder is still recorded");
    lock.release(6);
    check(lock.holder() == 5, "stale lock: a release by another job does not clear it");
    fs::remove_all(dir);
}

// One job's replies, in order per role. Each job gets its own instance, because JobRecord does not carry the issue number.
class ScriptedExecutor final : public RoleExecutor {
public:
    void queue(Role role, const RoleResult& result) { scripts_[role].push_back(result); }
    RoleResult run(Role role, const JobRecord&) override {
        auto& queue = scripts_[role];
        if (queue.empty()) return RoleResult{};
        RoleResult next = queue.front();
        queue.erase(queue.begin());
        return next;
    }

private:
    std::map<Role, std::vector<RoleResult>> scripts_;
};

RoleResult result_for(EventKind kind, const std::string& head) {
    RoleResult result;
    result.event.kind = kind;
    result.event.head_sha = head;
    result.model_family = kClaude;
    return result;
}

void test_store_backed_flow_releases_dependency_and_exhausts_budget() {
    const fs::path dir = fresh_dir("cipher-concurrency-flow");
    ScriptedExecutor job10_script;
    ScriptedExecutor job11_script;
    ScriptedExecutor job12_script;

    // Job 10 runs the standard path to NextIssue.
    job10_script.queue(Role::Implementation, result_for(EventKind::ImplementationComplete, kHeadA));
    job10_script.queue(Role::Implementation, result_for(EventKind::PullRequestOpened, kHeadA));
    RoleResult qa = result_for(EventKind::QaResult, kHeadA);
    qa.event.verdict = Verdict::Approve;
    job10_script.queue(Role::Qa, qa);
    RoleResult review = result_for(EventKind::ReviewVerdict, kHeadA);
    review.event.verdict = Verdict::Approve;
    job10_script.queue(Role::Reviewer, review);

    // Job 12 fails implementation on every attempt.
    job12_script.queue(Role::Implementation, result_for(EventKind::ImplementationFailed, ""));
    job12_script.queue(Role::Implementation, result_for(EventKind::ImplementationFailed, ""));

    FileJobStore store(dir);
    {
        Orchestrator job10(10, IssueFacts{}, job10_script, store, Policy{});
        check(job10.start() && job10.advance() && job10.advance() && job10.advance(),
              "flow: job 10 reaches Ci");
        Event ci;
        ci.kind = EventKind::CiResult;
        ci.ci_passed = true;
        ci.head_sha = kHeadA;
        check(job10.apply(ci) && job10.advance() && job10.job().state == State::Merge,
              "flow: job 10 passes CI and review to Merge");

        MergeLock lock(dir);
        check(lock.try_acquire(10), "flow: job 10 takes the merge lock");
        Event merge;
        merge.kind = EventKind::MergeRequest;
        merge.head_sha = kHeadA;
        merge.evidence.no_open_blocker_or_high = true;
        merge.evidence.scope_within_accepted = true;
        merge.evidence.protected_content_absent = true;
        check(job10.apply(merge) && job10.job().state == State::NextIssue, "flow: job 10 reaches NextIssue");
        lock.release(10);
    }

    // A fresh tick reads job 10 from disk. Job 11 depends on 10, so the dependency is released. Job 12 is admitted.
    const std::map<int, std::vector<int>> deps = {{10, {}}, {11, {10}}, {12, {}}};
    const auto decisions = tick(dir, deps, [&](int issue) -> RoleExecutor& {
        return issue == 11 ? static_cast<RoleExecutor&>(job11_script) : static_cast<RoleExecutor&>(job12_script);
    });
    check(find(decisions, 11) && find(decisions, 11)->action == Action::Admit,
          "flow: dependent job 11 is admitted once job 10 reaches NextIssue");
    check(find(decisions, 12) && find(decisions, 12)->action == Action::Admit, "flow: job 12 is admitted in the same tick");

    // Job 12 burns its two implementation executions and is escalated. Job 10 stays where it was.
    // The tick already ran job 12's first implementation attempt. One more advance runs the second and last one.
    FileJobStore reread(dir);
    Orchestrator job12(12, IssueFacts{}, job12_script, reread, Policy{});
    check(job12.advance() && job12.job().state == State::NeedsHuman &&
              job12.job().escalation == EscalationReason::RetryBudgetExhausted,
          "flow: job 12 ends in NeedsHuman with RetryBudgetExhausted");
    check(job12.job().implementation_executions == 2, "flow: job 12 used exactly two implementation executions");

    const auto job10 = FileJobStore(dir).load(10);
    check(job10 && job10->state == State::NextIssue && job10->implementation_executions == 1,
          "flow: job 10 is unchanged by job 12's escalation");
    fs::remove_all(dir);
}

}  // namespace

int main() {
    test_fourth_job_waits_and_restart_cannot_exceed_the_limit();
    test_needs_human_frees_a_slot_without_stopping_the_others();
    test_dependency_gating();
    test_unrecorded_worktree_conflict_stops_all_admission();
    test_merge_lock_serializes_and_gates_are_reevaluated();
    test_stale_merge_lock_blocks_until_a_human_clears_it();
    test_store_backed_flow_releases_dependency_and_exhausts_budget();
    std::cout << (g_failures == 0 ? "ALL PASSED" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
