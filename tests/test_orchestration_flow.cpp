// Tests for Stage 2-4 orchestration: normalized GitHub facts, event conversion, and the orchestrator
// driving the Stage 1 state machine through a scripted role executor. No network, model or filesystem
// dependency. Plain pass/fail bookkeeping, like the other CIPHER suites.

#include "cipher/orchestration/github_facts.hpp"
#include "cipher/orchestration/orchestrator.hpp"

#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace cipher::orchestration;

int g_failures = 0;
int g_passes = 0;

void check(bool condition, const std::string& name, const std::string& reason) {
    if (condition) {
        ++g_passes;
        std::cout << "PASS: " << name << "\n";
    } else {
        ++g_failures;
        std::cout << "FAIL: " << name << " - " << reason << "\n";
    }
}

const std::string kHeadA = "1111111111111111111111111111111111111111";
const std::string kHeadB = "2222222222222222222222222222222222222222";
const std::string kClaude = "claude";
const std::string kGemini = "gemini";

// Deterministic stand-in for the role executor. Each role returns its queued results in order;
// an empty queue yields a result the state machine rejects, so tests see it as a failure.
class ScriptedExecutor final : public RoleExecutor {
public:
    void queue(Role role, const RoleResult& result) { scripts_[role].push_back(result); }

    RoleResult run(Role role, const JobRecord&) override {
        ++calls_[role];
        auto& queue = scripts_[role];
        if (queue.empty()) return RoleResult{};
        RoleResult next = queue.front();
        queue.erase(queue.begin());
        return next;
    }

    int calls(Role role) const {
        const auto found = calls_.find(role);
        return found == calls_.end() ? 0 : found->second;
    }

private:
    std::map<Role, std::vector<RoleResult>> scripts_;
    std::map<Role, int> calls_;
};

RoleResult role_result(EventKind kind, const std::string& head, const std::string& family) {
    RoleResult result;
    result.event.kind = kind;
    result.event.head_sha = head;
    result.model_family = family;
    return result;
}

RoleResult review_result(Verdict verdict, const std::string& head, const std::string& family) {
    RoleResult result = role_result(EventKind::ReviewVerdict, head, family);
    result.event.verdict = verdict;
    return result;
}

Event merge_request(const std::string& head, bool blocker_clear, bool scope_ok, bool protected_ok) {
    Event event;
    event.kind = EventKind::MergeRequest;
    event.head_sha = head;
    event.evidence.no_open_blocker_or_high = blocker_clear;
    event.evidence.scope_within_accepted = scope_ok;
    event.evidence.protected_content_absent = protected_ok;
    return event;
}

PullRequestFacts pr_at(const std::string& head, CheckState checks) {
    PullRequestFacts pr;
    pr.number = 42;
    pr.open = true;
    pr.head_sha = head;
    pr.checks = checks;
    return pr;
}

// Plain implementation -> PR -> QA -> CI -> review path, up to the Review state.
void queue_standard_path(ScriptedExecutor& executor, const std::string& head) {
    executor.queue(Role::Implementation, role_result(EventKind::ImplementationComplete, head, kClaude));
    executor.queue(Role::Implementation, role_result(EventKind::PullRequestOpened, head, kClaude));
    RoleResult qa = role_result(EventKind::QaResult, head, kClaude);
    qa.event.verdict = Verdict::Approve;
    executor.queue(Role::Qa, qa);
}

void test_issue_labels_map_to_facts() {
    IssueObservation issue;
    issue.labels = {"needs-research", "security-sensitive"};
    const IssueFacts facts = issue_facts_from(issue);
    check(facts.research, "labels: needs-research sets research", "research not set");
    check(!facts.needs_design, "labels: absent label stays false", "needs_design set");
    check(facts.security_sensitive, "labels: security-sensitive sets flag", "flag not set");
}

void test_ci_conversion() {
    check(!ci_event(pr_at(kHeadA, CheckState::Pending)).has_value(), "ci: pending checks produce no event",
          "event produced while pending");
    const auto passed = ci_event(pr_at(kHeadA, CheckState::Passed));
    check(passed && passed->kind == EventKind::CiResult && passed->ci_passed && passed->head_sha == kHeadA,
          "ci: passed checks produce CiResult on the PR head", "wrong CiResult");
    const auto failed = ci_event(pr_at(kHeadA, CheckState::Failed));
    check(failed && !failed->ci_passed, "ci: failed checks produce CiResult with ci_passed false",
          "failed not converted");
    check(!ci_event(pr_at("", CheckState::Passed)).has_value(), "ci: missing head produces no event",
          "event produced without head");
}

void test_review_and_stale_head_detection() {
    PullRequestFacts pr = pr_at(kHeadA, CheckState::Passed);
    pr.review = ReviewState::Approved;
    pr.reviewed_head_sha = kHeadB;
    check(!review_event(pr).has_value(), "review: review on an older head is not converted",
          "stale review converted");

    pr.reviewed_head_sha = kHeadA;
    const auto approved = review_event(pr);
    check(approved && approved->verdict == Verdict::Approve, "review: approval on current head converts",
          "approval not converted");

    pr.review = ReviewState::ChangesRequested;
    const auto changes = review_event(pr);
    check(changes && changes->verdict == Verdict::ChangesRequested, "review: changes requested converts",
          "changes not converted");

    PullRequestFacts closed = pr_at(kHeadA, CheckState::Passed);
    closed.open = false;
    check(!is_current_head(closed, kHeadA), "head: closed PR is never the current head",
          "closed PR treated as current");
    check(!is_current_head(pr_at(kHeadB, CheckState::Passed), kHeadA), "head: mismatched SHA is stale",
          "stale head accepted");
}

void test_happy_path_reaches_next_issue() {
    ScriptedExecutor executor;
    InMemoryJobStore store;
    queue_standard_path(executor, kHeadA);
    executor.queue(Role::Reviewer, review_result(Verdict::Approve, kHeadA, kGemini));

    Orchestrator orch(7, IssueFacts{}, executor, store, Policy{});
    check(orch.start(), "happy: start accepted", "start rejected");
    check(orch.advance() && orch.job().state == State::PullRequest, "happy: implementation reaches PullRequest",
          "wrong state after implementation");
    check(orch.advance() && orch.job().state == State::Qa, "happy: PR opened reaches Qa", "wrong state after PR");
    check(orch.advance() && orch.job().state == State::Ci, "happy: QA pass reaches Ci", "wrong state after QA");

    const auto passed = ci_event(pr_at(kHeadA, CheckState::Passed));
    check(passed && orch.apply(*passed), "happy: CI pass accepted", "CI pass rejected");
    check(orch.job().state == State::Review, "happy: CI pass reaches Review", "wrong state after CI");

    check(orch.advance() && orch.job().state == State::Merge, "happy: approval reaches Merge",
          "wrong state after review");
    check(orch.apply(merge_request(kHeadA, true, true, true)) && orch.job().state == State::NextIssue,
          "happy: merge with all gates reaches NextIssue", "merge rejected");
    check(orch.job().implementation_executions == 1, "happy: one implementation execution", "count is not 1");
}

void test_stale_ci_head_is_rejected() {
    ScriptedExecutor executor;
    InMemoryJobStore store;
    queue_standard_path(executor, kHeadA);
    Orchestrator orch(8, IssueFacts{}, executor, store, Policy{});
    orch.start();
    orch.advance();
    orch.advance();
    orch.advance();

    const auto stale = ci_event(pr_at(kHeadB, CheckState::Passed));
    check(stale && !orch.apply(*stale) && orch.job().state == State::Ci, "stale: CI on another head rejected",
          "stale CI accepted");
}

void test_retry_is_bounded_to_two_executions() {
    ScriptedExecutor executor;
    InMemoryJobStore store;
    queue_standard_path(executor, kHeadA);
    executor.queue(Role::Reviewer, review_result(Verdict::ChangesRequested, kHeadA, kGemini));
    executor.queue(Role::Implementation, role_result(EventKind::ImplementationComplete, kHeadB, kClaude));
    RoleResult qa = role_result(EventKind::QaResult, kHeadB, kClaude);
    qa.event.verdict = Verdict::Approve;
    executor.queue(Role::Qa, qa);
    executor.queue(Role::Reviewer, review_result(Verdict::ChangesRequested, kHeadB, kGemini));
    // A third execution is queued on purpose: the machine must refuse it.
    executor.queue(Role::Implementation, role_result(EventKind::ImplementationComplete, "3333", kClaude));

    Orchestrator orch(9, IssueFacts{}, executor, store, Policy{});
    orch.start();
    orch.advance();          // implementation 1
    orch.advance();          // PullRequestOpened
    orch.advance();          // QA
    orch.apply(*ci_event(pr_at(kHeadA, CheckState::Passed)));
    orch.advance();          // review: changes requested
    check(orch.job().state == State::Implementation, "retry: changes requested returns to Implementation",
          "wrong state after changes requested");
    orch.advance();          // implementation 2 (retry skips PR)
    orch.advance();          // QA on the retry head
    orch.apply(*ci_event(pr_at(kHeadB, CheckState::Passed)));
    orch.advance();          // review 2: changes requested, budget exhausted
    check(orch.job().implementation_executions == 2, "retry: exactly two implementation executions",
          "implementation count is not 2");
    check(orch.job().state == State::NeedsHuman && orch.job().escalation == EscalationReason::RetryBudgetExhausted,
          "retry: budget exhaustion escalates to NeedsHuman", "no escalation after budget");
    check(!orch.advance() && orch.job().implementation_executions == 2, "retry: no third execution after escalation",
          "third execution ran");
}

void test_blocked_pause_does_not_consume_budget() {
    ScriptedExecutor executor;
    InMemoryJobStore store;
    Orchestrator orch(10, IssueFacts{}, executor, store, Policy{});
    orch.start();
    const int executions_before_block = orch.job().implementation_executions;

    Event blocked;
    blocked.kind = EventKind::Blocked;
    check(orch.apply(blocked) && orch.job().state == State::Blocked, "blocked: first BLOCKED pauses the job",
          "first BLOCKED not paused");
    Event resumed;
    resumed.kind = EventKind::BlockedResumed;
    check(orch.apply(resumed) && orch.job().implementation_executions == executions_before_block,
          "blocked: resume leaves execution count unchanged", "resume changed execution count");
    check(orch.apply(blocked) && orch.job().state == State::NeedsHuman &&
              orch.job().escalation == EscalationReason::BlockedRepeated,
          "blocked: repeated BLOCKED escalates to NeedsHuman", "repeated BLOCKED not escalated");
}

void test_protected_operation_escalates_and_stops() {
    ScriptedExecutor executor;
    InMemoryJobStore store;
    Event escalate;
    escalate.kind = EventKind::Escalate;
    escalate.reason = EscalationReason::ProtectedOperation;
    RoleResult stop;
    stop.event = escalate;
    executor.queue(Role::Research, stop);

    IssueFacts facts;
    facts.research = true;
    Orchestrator orch(11, facts, executor, store, Policy{});
    orch.start();
    orch.advance();
    check(orch.job().state == State::NeedsHuman && orch.job().escalation == EscalationReason::ProtectedOperation,
          "protected: protected operation escalates to NeedsHuman", "no escalation");
    check(!orch.advance() && executor.calls(Role::Research) == 1, "protected: NeedsHuman is absorbing",
          "role ran after escalation");
}

void test_security_review_requires_independent_family() {
    IssueFacts facts;
    facts.security_sensitive = true;

    ScriptedExecutor same_family;
    InMemoryJobStore same_store;
    queue_standard_path(same_family, kHeadA);
    same_family.queue(Role::Reviewer, review_result(Verdict::Approve, kHeadA, kClaude));
    Orchestrator same(12, facts, same_family, same_store, Policy{});
    same.start();
    same.advance();
    same.advance();
    same.advance();
    same.apply(*ci_event(pr_at(kHeadA, CheckState::Passed)));
    same.advance();
    check(same.job().state == State::NeedsHuman && same.job().escalation == EscalationReason::SecurityReviewUnavailable,
          "security: same-family reviewer does not satisfy independence", "same family accepted");

    // Stage 1 escalates security-sensitive work at CI until the Gemini reviewer is wired, so even an
    // independent-family approval must not reach Merge yet.
    ScriptedExecutor independent;
    InMemoryJobStore independent_store;
    queue_standard_path(independent, kHeadA);
    independent.queue(Role::Reviewer, review_result(Verdict::Approve, kHeadA, kGemini));
    Orchestrator other(13, facts, independent, independent_store, Policy{});
    other.start();
    other.advance();
    other.advance();
    other.advance();
    other.apply(*ci_event(pr_at(kHeadA, CheckState::Passed)));
    check(other.job().state == State::NeedsHuman && other.job().escalation == EscalationReason::SecurityReviewUnavailable,
          "security: sensitive work escalates at CI even with independent family", "sensitive work reached review");
}

void test_merge_gates_fail_closed() {
    ScriptedExecutor executor;
    InMemoryJobStore store;
    queue_standard_path(executor, kHeadA);
    executor.queue(Role::Reviewer, review_result(Verdict::Approve, kHeadA, kGemini));
    Orchestrator orch(14, IssueFacts{}, executor, store, Policy{});
    orch.start();
    orch.advance();
    orch.advance();
    orch.advance();
    orch.apply(*ci_event(pr_at(kHeadA, CheckState::Passed)));
    orch.advance();

    check(!orch.apply(merge_request(kHeadA, false, true, true)) && orch.job().state == State::Merge,
          "gates: open blocker rejects merge and keeps Merge state", "merge accepted with open blocker");
    check(!orch.apply(merge_request(kHeadB, true, true, true)), "gates: stale merge head rejected",
          "stale merge accepted");
    check(orch.apply(merge_request(kHeadA, true, false, true)) && orch.job().state == State::NeedsHuman,
          "gates: scope expansion escalates to NeedsHuman", "scope gate did not escalate");
}

void test_records_and_persistence() {
    ScriptedExecutor executor;
    InMemoryJobStore store;
    queue_standard_path(executor, kHeadA);
    Orchestrator first(15, IssueFacts{}, executor, store, Policy{});
    first.start();
    first.advance();
    first.apply(merge_request(kHeadB, true, true, true));

    const auto& records = first.records();
    check(records.size() >= 3, "records: every attempted transition is recorded", "missing records");
    check(!records.back().accepted && records.back().issue_number == 15,
          "records: rejected event is recorded as not accepted", "rejection not recorded");
    check(records.front().accepted && records.front().event == EventKind::Start,
          "records: first record is the accepted Start", "first record wrong");

    Orchestrator resumed(15, IssueFacts{}, executor, store, Policy{});
    check(resumed.job().state == first.job().state, "persistence: new orchestrator resumes the saved state",
          "state not restored");
}

}  // namespace

// Throws from the implementation role while armed, to simulate a process that dies mid-execution.
class CrashingExecutor final : public RoleExecutor {
public:
    RoleResult run(Role role, const JobRecord&) override {
        if (role == Role::Implementation && armed) throw std::runtime_error("simulated crash");
        ++implementation_runs;
        return role_result(EventKind::ImplementationComplete, kHeadA, kClaude);
    }

    bool armed = true;
    int implementation_runs = 0;
};

void test_restart_recovery() {
    IssueFacts facts;

    // Family is persisted in the job record, so a reloaded job still enforces reviewer independence.
    ScriptedExecutor family_executor;
    InMemoryJobStore family_store;
    queue_standard_path(family_executor, kHeadA);
    Orchestrator first(20, facts, family_executor, family_store, Policy{});
    first.start();
    first.advance();
    Orchestrator reloaded(20, facts, family_executor, family_store, Policy{});
    check(reloaded.job().implementation_family == kClaude, "restart: implementation family survives reload",
          "family lost on reload");

    // A crash during an implementation execution is a counted failure on restart, never a silent re-run.
    CrashingExecutor crashing;
    InMemoryJobStore crash_store;
    Orchestrator crashed(21, facts, crashing, crash_store, Policy{});
    crashed.start();
    bool threw = false;
    try {
        crashed.advance();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "restart: crash propagates out of the role", "no exception");
    check(crash_store.load(21)->execution_in_flight, "restart: in-flight marker saved before the role runs",
          "marker not saved");

    Orchestrator restarted(21, facts, crashing, crash_store, Policy{});
    const int runs_before_recover = crashing.implementation_runs;
    check(restarted.recover(), "restart: in-flight execution is recovered", "nothing recovered");
    check(restarted.job().implementation_executions == 2 && restarted.job().state == State::Implementation &&
              !restarted.job().execution_in_flight,
          "restart: recovered execution is counted once and the attempt is retried", "counter or state wrong");
    check(crashing.implementation_runs == runs_before_recover, "restart: recovery never re-runs the role",
          "role ran during recovery");
    check(!restarted.recover(), "restart: recovery is a no-op when nothing is in flight", "recovered twice");

    // Second crash on the last allowed attempt: recovery escalates instead of starting a third execution.
    crashing.armed = true;
    Orchestrator second_crash(21, facts, crashing, crash_store, Policy{});
    try {
        second_crash.advance();
    } catch (const std::runtime_error&) {
    }
    Orchestrator final_restart(21, facts, crashing, crash_store, Policy{});
    final_restart.recover();
    check(final_restart.job().state == State::NeedsHuman &&
              final_restart.job().escalation == EscalationReason::RetryBudgetExhausted &&
              final_restart.job().implementation_executions == 2,
          "restart: second crash on the last attempt escalates without a third execution",
          "third execution or wrong escalation");
}

int main() {
    test_issue_labels_map_to_facts();
    test_ci_conversion();
    test_review_and_stale_head_detection();
    test_happy_path_reaches_next_issue();
    test_stale_ci_head_is_rejected();
    test_retry_is_bounded_to_two_executions();
    test_blocked_pause_does_not_consume_budget();
    test_protected_operation_escalates_and_stops();
    test_security_review_requires_independent_family();
    test_merge_gates_fail_closed();
    test_records_and_persistence();
    test_restart_recovery();

    std::cout << g_passes << " passed, " << g_failures << " failed\n";
    return g_failures == 0 ? 0 : 1;
}
