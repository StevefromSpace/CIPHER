// Tests for the Stage 9 scheduler (pure) and the worktree allocator (filesystem + fake git backend).
// Uses a scratch directory under the system temp path only. No network, model, GitHub or real git calls.

#include "cipher/orchestration/scheduler.hpp"
#include "cipher/orchestration/worktree_allocator.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace cipher::orchestration;
namespace fs = std::filesystem;

int g_failures = 0;
int g_passes = 0;

void check(bool condition, const std::string& name) {
    if (condition) {
        ++g_passes;
        std::cout << "PASS: " << name << "\n";
    } else {
        ++g_failures;
        std::cout << "FAIL: " << name << "\n";
    }
}

SchedulerJob queued(int issue, std::vector<int> depends_on = {}) {
    SchedulerJob job;
    job.issue_number = issue;
    job.state = State::Issue;
    job.depends_on = std::move(depends_on);
    return job;
}

SchedulerJob with_state(int issue, State state) {
    SchedulerJob job = queued(issue);
    job.state = state;
    return job;
}

int count_action(const std::vector<Decision>& decisions, Action action) {
    int count = 0;
    for (const auto& decision : decisions) {
        if (decision.action == action) ++count;
    }
    return count;
}

const Decision* find_decision(const std::vector<Decision>& decisions, int issue) {
    for (const auto& decision : decisions) {
        if (decision.issue_number == issue) return &decision;
    }
    return nullptr;
}

bool admitted(const std::vector<Decision>& decisions, int issue) {
    const Decision* decision = find_decision(decisions, issue);
    return decision && decision->action == Action::Admit;
}

void test_capacity_admits_up_to_limit_then_queues() {
    check(count_action(plan_schedule({queued(1)}), Action::Admit) == 1, "capacity: one queued job is admitted");

    auto two = plan_schedule({queued(1), queued(2)});
    check(count_action(two, Action::Admit) == 2, "capacity: two queued jobs are both admitted");

    auto three = plan_schedule({queued(1), queued(2), queued(3)});
    check(count_action(three, Action::Admit) == 3, "capacity: three queued jobs are all admitted at the default limit");

    auto four = plan_schedule({queued(1), queued(2), queued(3), queued(4)});
    check(count_action(four, Action::Admit) == 3, "capacity: a fourth queued job is not admitted at the default limit");
    check(find_decision(four, 4) && find_decision(four, 4)->action == Action::Wait, "capacity: the fourth job waits");
    check(admitted(four, 1) && admitted(four, 2) && admitted(four, 3), "capacity: the first three in issue order are admitted");
}

void test_active_jobs_hold_slots() {
    const auto decisions = plan_schedule(
        {with_state(10, State::Implementation), with_state(11, State::Qa), queued(12), queued(13)});
    check(count_action(decisions, Action::Admit) == 1, "slots: two running jobs leave one slot for the queue");
    check(admitted(decisions, 12) && !admitted(decisions, 13), "slots: the lower issue number takes the free slot");
}

void test_blocked_job_holds_a_slot() {
    const auto decisions = plan_schedule(
        {with_state(20, State::Blocked), with_state(21, State::Review), with_state(22, State::Ci), queued(23)});
    check(count_action(decisions, Action::Admit) == 0, "blocked: a paused-by-BLOCKED job still holds a slot");
}

void test_terminal_jobs_release_slots() {
    const auto decisions = plan_schedule(
        {with_state(30, State::NextIssue), with_state(31, State::NeedsHuman), with_state(32, State::Implementation),
         with_state(33, State::Qa), queued(34), queued(35)});
    check(count_action(decisions, Action::Admit) == 1, "release: NextIssue and NeedsHuman free their slots");
    check(admitted(decisions, 34), "release: the next queued job takes the freed slot");
}

void test_limit_is_configurable_and_restart_safe() {
    const auto one = plan_schedule({queued(1), queued(2), queued(3)}, 1);
    check(count_action(one, Action::Admit) == 1, "config: max_parallel_jobs = 1 admits only one job");

    // A re-poll with the same persisted states must not admit anything new.
    const std::vector<SchedulerJob> running = {with_state(40, State::Implementation), with_state(41, State::Implementation),
                                               with_state(42, State::Implementation), queued(43)};
    check(count_action(plan_schedule(running), Action::Admit) == 0, "restart: a re-poll with three running jobs admits none");
    check(count_action(plan_schedule(running), Action::Admit) == 0, "restart: a second re-poll is still stable");
    check(count_action(plan_schedule(running, 4), Action::Admit) == 1, "config: a larger limit admits the queued job");
}

void test_dependency_waits_until_next_issue() {
    const auto waiting = plan_schedule({with_state(50, State::Implementation), queued(51, {50})});
    check(!admitted(waiting, 51), "dependency: a job waits while its dependency is still running");
    check(find_decision(waiting, 51) && find_decision(waiting, 51)->action == Action::Wait, "dependency: the waiting job is Wait");

    const auto ready = plan_schedule({with_state(52, State::NextIssue), queued(53, {52})});
    check(admitted(ready, 53), "dependency: a job is admitted once its dependency reaches NextIssue");
}

void test_dependency_needs_human_escalates_dependent() {
    const auto decisions = plan_schedule({with_state(60, State::NeedsHuman), queued(61, {60})});
    const Decision* decision = find_decision(decisions, 61);
    check(decision && decision->action == Action::Escalate, "dependency: a dependent job escalates when its dependency needs a human");
    check(decision && decision->reason == EscalationReason::Ambiguity, "dependency: the escalation reason is recorded");
}

void test_unknown_dependency_escalates() {
    const auto decisions = plan_schedule({queued(70, {999})});
    const Decision* decision = find_decision(decisions, 70);
    check(decision && decision->action == Action::Escalate, "dependency: an unknown dependency escalates the job");
}

void test_cycle_escalates_every_member() {
    const auto decisions = plan_schedule({queued(80, {81}), queued(81, {82}), queued(82, {80}), queued(83)});
    check(find_decision(decisions, 80) && find_decision(decisions, 80)->action == Action::Escalate, "cycle: first member escalates");
    check(find_decision(decisions, 81) && find_decision(decisions, 81)->action == Action::Escalate, "cycle: second member escalates");
    check(find_decision(decisions, 82) && find_decision(decisions, 82)->action == Action::Escalate, "cycle: third member escalates");
    check(admitted(decisions, 83), "cycle: an unrelated job is still admitted");
}

void test_failure_isolation_keeps_unrelated_jobs_running() {
    // Job 90 needs a human. Job 91 is independent and must still be admitted; 92 depends only on 91.
    const auto decisions = plan_schedule(
        {with_state(90, State::NeedsHuman), queued(91), queued(92, {91})});
    check(admitted(decisions, 91), "isolation: an independent job is admitted despite another job needing a human");
    check(!admitted(decisions, 92), "isolation: the dependent job waits for its upstream job");
}

void test_pause_and_cancel() {
    SchedulerJob paused = queued(100);
    paused.paused = true;
    const auto with_pause = plan_schedule({paused, queued(101)});
    check(!admitted(with_pause, 100), "pause: a paused job is not admitted");
    check(admitted(with_pause, 101), "pause: a paused job does not hold a slot from the queue");

    SchedulerJob cancelled = queued(102);
    cancelled.cancelled = true;
    const auto with_cancel = plan_schedule({cancelled});
    const Decision* decision = find_decision(with_cancel, 102);
    check(decision && decision->action == Action::Escalate, "cancel: a cancelled job escalates instead of starting");
    check(decision && decision->reason == EscalationReason::ExplicitRequest, "cancel: the reason is an explicit owner request");
}

void test_global_stop_admits_nothing() {
    const std::vector<SchedulerJob> jobs = {queued(110), queued(111)};
    check(plan_schedule(jobs, kDefaultMaxParallelJobs, GlobalStop::WorktreeConflict).empty(),
          "global stop: a worktree conflict admits nothing");
    check(plan_schedule(jobs, kDefaultMaxParallelJobs, GlobalStop::AuthAnomaly).empty(),
          "global stop: an auth anomaly admits nothing");
    check(plan_schedule(jobs, kDefaultMaxParallelJobs, GlobalStop::RulesetChange).empty(),
          "global stop: a ruleset change admits nothing");
}

void test_duplicate_issue_throws() {
    bool threw = false;
    try {
        plan_schedule({queued(120), queued(120)});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "duplicate: the same issue twice is refused, not guessed");
}

// Fake git backend: creates the directory and records the call. Never runs a process.
class FakeWorktreeBackend final : public WorktreeBackend {
public:
    void add(const fs::path& path, int issue_number) override {
        ++calls;
        last_issue = issue_number;
        fs::create_directories(path);
    }
    int calls = 0;
    int last_issue = 0;
};

class FailingWorktreeBackend final : public WorktreeBackend {
public:
    void add(const fs::path&, int) override { throw std::runtime_error("simulated git failure"); }
};

fs::path fresh_root(const std::string& name) {
    const fs::path root = fs::temp_directory_path() / ("cipher-orch-worktrees-" + name);
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void test_allocator_creates_then_reuses_for_same_job() {
    const fs::path root = fresh_root("create");
    FakeWorktreeBackend backend;
    WorktreeAllocator allocator(root, backend);
    check(allocator.allocate(7) == WorktreeOutcome::Created, "allocator: first allocation creates the worktree");
    check(backend.calls == 1 && backend.last_issue == 7, "allocator: the backend is asked once for issue 7");
    check(fs::exists(allocator.path_for(7)), "allocator: the worktree directory exists after creation");
    check(fs::exists(allocator.owner_record_for(7)), "allocator: the owner record is written after creation");
    check(allocator.allocate(7) == WorktreeOutcome::Reused, "allocator: the same job reuses its own worktree");
    check(backend.calls == 1, "allocator: reuse does not call the backend again");
}

void test_allocator_refuses_unrecorded_directory() {
    const fs::path root = fresh_root("unrecorded");
    FakeWorktreeBackend backend;
    WorktreeAllocator allocator(root, backend);
    fs::create_directories(allocator.path_for(8));   // an owner directory with no record, like job-4b1f4de9
    check(allocator.allocate(8) == WorktreeOutcome::Conflict, "allocator: an unrecorded directory is a conflict");
    check(backend.calls == 0, "allocator: the conflict does not touch the directory through the backend");
}

void test_allocator_refuses_record_for_another_job() {
    const fs::path root = fresh_root("other");
    FakeWorktreeBackend backend;
    WorktreeAllocator allocator(root, backend);
    fs::create_directories(allocator.path_for(9));
    std::ofstream(allocator.owner_record_for(9)) << "issue=10\n";
    check(allocator.allocate(9) == WorktreeOutcome::Conflict, "allocator: a record naming another issue is a conflict");
}

void test_allocator_refuses_stale_record_without_directory() {
    const fs::path root = fresh_root("stale");
    FakeWorktreeBackend backend;
    WorktreeAllocator allocator(root, backend);
    std::ofstream(allocator.owner_record_for(11)) << "issue=11\n";
    check(allocator.allocate(11) == WorktreeOutcome::Conflict, "allocator: a record without its directory is a conflict");
    check(backend.calls == 0, "allocator: a stale record does not cause a new worktree to be added");
}

void test_allocator_backend_failure_leaves_no_record() {
    const fs::path root = fresh_root("failure");
    FailingWorktreeBackend backend;
    WorktreeAllocator allocator(root, backend);
    bool threw = false;
    try {
        allocator.allocate(12);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "allocator: a git failure propagates instead of being swallowed");
    check(!fs::exists(allocator.owner_record_for(12)), "allocator: no owner record is written when git fails");
}

void test_allocator_gives_each_job_its_own_directory() {
    const fs::path root = fresh_root("isolated");
    FakeWorktreeBackend backend;
    WorktreeAllocator allocator(root, backend);
    allocator.allocate(20);
    allocator.allocate(21);
    check(allocator.path_for(20) != allocator.path_for(21), "allocator: two jobs get different worktree paths");
    check(allocator.path_for(20).filename() == "job-20", "allocator: directory name follows job-<issue>");
    check(backend.calls == 2, "allocator: each new job adds its own worktree");
}

}  // namespace

int main() {
    test_capacity_admits_up_to_limit_then_queues();
    test_active_jobs_hold_slots();
    test_blocked_job_holds_a_slot();
    test_terminal_jobs_release_slots();
    test_limit_is_configurable_and_restart_safe();
    test_dependency_waits_until_next_issue();
    test_dependency_needs_human_escalates_dependent();
    test_unknown_dependency_escalates();
    test_cycle_escalates_every_member();
    test_failure_isolation_keeps_unrelated_jobs_running();
    test_pause_and_cancel();
    test_global_stop_admits_nothing();
    test_duplicate_issue_throws();
    test_allocator_creates_then_reuses_for_same_job();
    test_allocator_refuses_unrecorded_directory();
    test_allocator_refuses_record_for_another_job();
    test_allocator_refuses_stale_record_without_directory();
    test_allocator_backend_failure_leaves_no_record();
    test_allocator_gives_each_job_its_own_directory();

    std::cout << g_passes << " passed, " << g_failures << " failed\n";
    return g_failures == 0 ? 0 : 1;
}
