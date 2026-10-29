// Tests for the Stage 1 orchestration state machine. Covers the contract workflow (docs/AGENT_ROLE_CONTRACT.md
// sections 2, 5, 6, 8) with no GitHub, model or network dependency. Plain pass/fail bookkeeping, like the
// other CIPHER suites.

#include "cipher/orchestration/state_machine.hpp"

#include <iostream>
#include <string>

namespace {

using namespace cipher::orchestration;

int g_failures = 0;
int g_passes = 0;

void pass(const std::string& name) {
    ++g_passes;
    std::cout << "PASS: " << name << "\n";
}

void fail(const std::string& name, const std::string& reason) {
    ++g_failures;
    std::cout << "FAIL: " << name << " - " << reason << "\n";
}

void check(bool condition, const std::string& name, const std::string& reason) {
    if (condition) {
        pass(name);
    } else {
        fail(name, reason);
    }
}

const std::string kHeadA = "1111111111111111111111111111111111111111";
const std::string kHeadB = "2222222222222222222222222222222222222222";

Event event(EventKind kind) {
    Event e;
    e.kind = kind;
    return e;
}

Event with_head(EventKind kind, const std::string& head) {
    Event e = event(kind);
    e.head_sha = head;
    return e;
}

Event with_verdict(EventKind kind, Verdict verdict, const std::string& head = kHeadA) {
    Event e = with_head(kind, head);
    e.verdict = verdict;
    return e;
}

Event with_ci(bool passed, const std::string& head = kHeadA) {
    Event e = with_head(EventKind::CiResult, head);
    e.ci_passed = passed;
    return e;
}

Event with_escalation(EscalationReason reason) {
    Event e = event(EventKind::Escalate);
    e.reason = reason;
    return e;
}

Event with_merge(const std::string& head, bool scope_ok = true, bool protected_ok = true, bool no_blockers = true) {
    Event e = with_head(EventKind::MergeRequest, head);
    e.evidence.scope_within_accepted = scope_ok;
    e.evidence.protected_content_absent = protected_ok;
    e.evidence.no_open_blocker_or_high = no_blockers;
    return e;
}

// Applies an event and fails the named test if it is rejected.
JobRecord apply(const JobRecord& job, const Event& e, const Policy& policy, const std::string& name) {
    TransitionResult result = transition(job, e, policy);
    if (!result.accepted) {
        fail(name, "unexpected rejection: " + result.reason);
    }
    return result.next;
}

// Drives a job from ISSUE to IMPLEMENTATION with no research or design step.
JobRecord start_plain(const Policy& policy) {
    JobRecord job;
    return apply(job, event(EventKind::Start), policy, "start");
}

// Gets a job to REVIEW on head A through a clean QA and CI pass.
JobRecord to_review(const Policy& policy) {
    JobRecord job = start_plain(policy);
    job = apply(job, with_head(EventKind::ImplementationComplete, kHeadA), policy, "impl complete");
    job = apply(job, with_head(EventKind::PullRequestOpened, kHeadA), policy, "pr opened");
    job = apply(job, with_verdict(EventKind::QaResult, Verdict::Approve), policy, "qa approve");
    job = apply(job, with_ci(true), policy, "ci pass");
    return job;
}

void test_happy_path_to_next_issue() {
    const Policy policy;
    JobRecord job = to_review(policy);
    check(job.state == State::Review, "happy path reaches REVIEW", "state was not REVIEW");

    job = apply(job, with_verdict(EventKind::ReviewVerdict, Verdict::Approve), policy, "review approve");
    check(job.state == State::Merge && job.review_approved, "approve reaches MERGE", "state was not MERGE");

    TransitionResult merged = transition(job, with_merge(kHeadA), policy);
    check(merged.accepted && merged.next.state == State::NextIssue,
          "merge with all gates reaches NEXT_ISSUE", "merge was not accepted: " + merged.reason);
}

void test_full_workflow_with_research_and_design() {
    const Policy policy;
    JobRecord job;
    job.facts.research = true;
    job.facts.needs_design = true;
    job = apply(job, event(EventKind::Start), policy, "start research");
    check(job.state == State::Research, "research before architecture", "start did not enter RESEARCH");
    job = apply(job, event(EventKind::ResearchComplete), policy, "research complete");
    check(job.state == State::Architecture, "architecture after research", "did not enter ARCHITECTURE");
    job = apply(job, event(EventKind::ArchitectureComplete), policy, "architecture complete");
    check(job.state == State::Implementation && job.implementation_executions == 1,
          "implementation is execution 1", "implementation count was not 1");
}

void test_pr_path_skipped_on_retry() {
    const Policy policy;
    JobRecord job = to_review(policy);
    job = apply(job, with_verdict(EventKind::ReviewVerdict, Verdict::ChangesRequested), policy, "changes requested");
    check(job.state == State::Implementation && job.implementation_executions == 2,
          "changes requested starts attempt 2", "attempt 2 did not start");

    job = apply(job, with_head(EventKind::ImplementationComplete, kHeadB), policy, "attempt 2 complete");
    check(job.state == State::Qa, "attempt 2 goes to QA, not PR", "attempt 2 did not enter QA");
}

void test_review_rejected_before_qa_and_ci_on_retry() {
    const Policy policy;
    JobRecord job = to_review(policy);
    job = apply(job, with_verdict(EventKind::ReviewVerdict, Verdict::ChangesRequested), policy, "changes requested");
    job = apply(job, with_head(EventKind::ImplementationComplete, kHeadB), policy, "attempt 2 complete");

    TransitionResult early = transition(job, with_verdict(EventKind::ReviewVerdict, Verdict::Approve, kHeadB), policy);
    check(!early.accepted, "review rejected before QA and CI on retry", "review was accepted out of order");
}

void test_implementation_budget_is_two_total() {
    const Policy policy;
    JobRecord job = to_review(policy);
    job = apply(job, with_verdict(EventKind::ReviewVerdict, Verdict::ChangesRequested), policy, "attempt 1 failed");
    job = apply(job, with_head(EventKind::ImplementationComplete, kHeadB), policy, "attempt 2 complete");
    job = apply(job, with_verdict(EventKind::QaResult, Verdict::Approve, kHeadB), policy, "qa 2");
    job = apply(job, with_ci(false, kHeadB), policy, "ci failed on attempt 2");
    check(job.state == State::NeedsHuman && job.escalation == EscalationReason::RetryBudgetExhausted,
          "attempt 2 failure escalates without a third run", "no escalation after attempt 2");
}

void test_ci_failure_consumes_attempt() {
    const Policy policy;
    JobRecord job = start_plain(policy);
    job = apply(job, with_head(EventKind::ImplementationComplete, kHeadA), policy, "impl");
    job = apply(job, with_head(EventKind::PullRequestOpened, kHeadA), policy, "pr");
    job = apply(job, with_verdict(EventKind::QaResult, Verdict::Approve), policy, "qa");
    job = apply(job, with_ci(false), policy, "ci failed");
    check(job.state == State::Implementation && job.implementation_executions == 2,
          "CI failure starts attempt 2", "CI failure did not start attempt 2");
}

void test_qa_must_precede_ci() {
    const Policy policy;
    JobRecord job = start_plain(policy);
    job = apply(job, with_head(EventKind::ImplementationComplete, kHeadA), policy, "impl");
    job = apply(job, with_head(EventKind::PullRequestOpened, kHeadA), policy, "pr");
    TransitionResult result = transition(job, with_ci(true), policy);
    check(!result.accepted, "CI before QA rejected", "CI was accepted before QA");
}

void test_pr_head_mismatch_rejected() {
    const Policy policy;
    JobRecord job = to_review(policy);
    TransitionResult result = transition(job, with_verdict(EventKind::ReviewVerdict, Verdict::Approve, kHeadB), policy);
    check(!result.accepted, "review on a different head rejected", "stale-head verdict was accepted");
}

void test_repeat_review_on_same_head_rejected() {
    const Policy policy;
    JobRecord job = to_review(policy);
    job = apply(job, with_verdict(EventKind::ReviewVerdict, Verdict::ChangesRequested), policy, "round 1");
    // Attempt 2 is pending, so no review should be accepted on the old head.
    TransitionResult result = transition(job, with_verdict(EventKind::ReviewVerdict, Verdict::Approve), policy);
    check(!result.accepted, "no second review on the same head", "review on the reviewed head was accepted");
}

void test_review_rounds_exhausted_escalates() {
    Policy policy;
    policy.max_implementation_executions = 10;   // test-only: contract limits make round 3 unreachable (see report)
    JobRecord job = to_review(policy);
    job = apply(job, with_verdict(EventKind::ReviewVerdict, Verdict::ChangesRequested), policy, "round 1");
    job = apply(job, with_head(EventKind::ImplementationComplete, kHeadB), policy, "impl 2");
    job = apply(job, with_verdict(EventKind::QaResult, Verdict::Approve, kHeadB), policy, "qa 2");
    job = apply(job, with_ci(true, kHeadB), policy, "ci 2");
    job = apply(job, with_verdict(EventKind::ReviewVerdict, Verdict::ChangesRequested, kHeadB), policy, "round 2");
    job = apply(job, with_head(EventKind::ImplementationComplete, kHeadA), policy, "impl 3");
    job = apply(job, with_verdict(EventKind::QaResult, Verdict::Approve, kHeadA), policy, "qa 3");
    job = apply(job, with_ci(true, kHeadA), policy, "ci 3");
    job = apply(job, with_verdict(EventKind::ReviewVerdict, Verdict::ChangesRequested, kHeadA), policy, "round 3");
    check(job.state == State::NeedsHuman && job.escalation == EscalationReason::ReviewRoundsExhausted,
          "third ChangesRequested without APPROVE escalates", "round limit did not escalate");
}

void test_blocked_is_diagnostic_then_escalates() {
    const Policy policy;
    JobRecord job = start_plain(policy);
    const int executions_before = job.implementation_executions;
    job = apply(job, event(EventKind::Blocked), policy, "first blocked");
    check(job.state == State::Blocked, "first BLOCKED pauses the job", "first BLOCKED did not pause");
    check(job.implementation_executions == executions_before, "BLOCKED starts no execution",
          "BLOCKED changed the execution count");

    job = apply(job, event(EventKind::BlockedResumed), policy, "resume");
    check(job.state == State::Implementation && job.implementation_executions == executions_before,
          "resume keeps the counters", "resume changed state or counters");

    job = apply(job, event(EventKind::Blocked), policy, "second blocked");
    check(job.state == State::NeedsHuman && job.escalation == EscalationReason::BlockedRepeated,
          "second BLOCKED escalates", "second BLOCKED did not escalate");
}

void test_paused_job_rejects_work_events() {
    const Policy policy;
    JobRecord job = start_plain(policy);
    job = apply(job, event(EventKind::Blocked), policy, "blocked");
    TransitionResult result = transition(job, with_head(EventKind::ImplementationComplete, kHeadA), policy);
    check(!result.accepted, "paused job rejects work", "work event accepted while BLOCKED");
}

void test_blocked_review_does_not_count_round() {
    const Policy policy;
    JobRecord job = to_review(policy);
    job = apply(job, with_verdict(EventKind::ReviewVerdict, Verdict::Blocked), policy, "review blocked");
    check(job.state == State::Blocked && job.review_rounds == 0, "BLOCKED review does not count a round",
          "BLOCKED review counted a round");
}

void test_disagreement_reevaluates_once() {
    const Policy policy;
    JobRecord job = to_review(policy);
    Event dis = with_head(EventKind::Disagreement, kHeadA);
    dis.verdict = Verdict::Approve;
    dis.second_verdict = Verdict::ChangesRequested;
    job = apply(job, dis, policy, "first disagreement");
    check(job.state == State::Review && job.reevaluation_pending, "disagreement allows one re-evaluation",
          "disagreement did not re-evaluate");

    job = apply(job, with_verdict(EventKind::ReviewVerdict, Verdict::Approve), policy, "re-evaluation");
    check(job.state == State::Merge, "re-evaluation on same head is accepted", "re-evaluation was rejected");
}

void test_second_disagreement_escalates() {
    const Policy policy;
    JobRecord job = to_review(policy);
    Event dis = with_head(EventKind::Disagreement, kHeadA);
    dis.verdict = Verdict::Approve;
    dis.second_verdict = Verdict::ChangesRequested;
    job = apply(job, dis, policy, "first disagreement");
    job = apply(job, dis, policy, "second disagreement");
    check(job.state == State::NeedsHuman && job.escalation == EscalationReason::DisagreementUnresolved,
          "second disagreement escalates", "second disagreement did not escalate");
}

void test_security_sensitive_fails_closed_at_review() {
    const Policy policy;
    JobRecord job;
    job.facts.security_sensitive = true;
    job = apply(job, event(EventKind::Start), policy, "start");
    job = apply(job, with_head(EventKind::ImplementationComplete, kHeadA), policy, "impl");
    job = apply(job, with_head(EventKind::PullRequestOpened, kHeadA), policy, "pr");
    job = apply(job, with_verdict(EventKind::QaResult, Verdict::Approve), policy, "qa");
    job = apply(job, with_ci(true), policy, "ci");
    check(job.state == State::NeedsHuman && job.escalation == EscalationReason::SecurityReviewUnavailable,
          "security-sensitive work escalates instead of reviewing", "security work reached review");
}

void test_merge_gates_each_block_merge() {
    const Policy policy;
    JobRecord job = to_review(policy);
    job = apply(job, with_verdict(EventKind::ReviewVerdict, Verdict::Approve), policy, "approve");

    TransitionResult no_blockers = transition(job, with_merge(kHeadA, true, true, false), policy);
    check(!no_blockers.accepted, "merge gate 5 blocks merge", "merge accepted with open BLOCKER or HIGH");

    TransitionResult wrong_head = transition(job, with_merge(kHeadB), policy);
    check(!wrong_head.accepted, "merge on a different head rejected", "merge accepted on stale head");

    TransitionResult scope = transition(job, with_merge(kHeadA, false, true, true), policy);
    check(scope.accepted && scope.next.state == State::NeedsHuman && scope.next.escalation == EscalationReason::ScopeExpansion,
          "merge gate 6 escalates on scope", "scope failure did not escalate");

    TransitionResult protected_content = transition(job, with_merge(kHeadA, true, false, true), policy);
    check(protected_content.accepted && protected_content.next.escalation == EscalationReason::ProtectedOperation,
          "merge gate 7 escalates on protected content", "protected content did not escalate");

    JobRecord no_review;
    no_review.state = State::Merge;
    no_review.ci_passed = true;
    no_review.qa_approved = true;
    TransitionResult review_gate = transition(no_review, with_merge(""), policy);
    check(!review_gate.accepted, "merge gate 3 blocks merge without review", "merge accepted without review");
}

void test_escalation_is_absorbing() {
    const Policy policy;
    JobRecord job = start_plain(policy);
    job = apply(job, with_escalation(EscalationReason::Ambiguity), policy, "escalate");
    check(job.state == State::NeedsHuman && job.escalation == EscalationReason::Ambiguity,
          "escalate records a reason", "escalation did not record reason");

    TransitionResult again = transition(job, with_escalation(EscalationReason::ScopeExpansion), policy);
    check(!again.accepted, "NEEDS_HUMAN is absorbing", "second escalation accepted");

    TransitionResult start_again = transition(job, event(EventKind::Start), policy);
    check(!start_again.accepted, "NEEDS_HUMAN rejects work", "work accepted after NEEDS_HUMAN");
}

void test_escalation_requires_reason() {
    const Policy policy;
    JobRecord job = start_plain(policy);
    TransitionResult result = transition(job, event(EventKind::Escalate), policy);
    check(!result.accepted, "escalation without a reason rejected", "reasonless escalation was accepted");
}

void test_no_next_issue_after_terminal() {
    const Policy policy;
    JobRecord job = to_review(policy);
    job = apply(job, with_verdict(EventKind::ReviewVerdict, Verdict::Approve), policy, "approve");
    job = apply(job, with_merge(kHeadA), policy, "merge");
    check(job.state == State::NextIssue, "merge reaches NEXT_ISSUE", "merge did not reach NEXT_ISSUE");
    TransitionResult again = transition(job, event(EventKind::Start), policy);
    check(!again.accepted, "NEXT_ISSUE is terminal", "work accepted after NEXT_ISSUE");
}

void test_policy_defaults_match_contract() {
    const Policy policy;
    check(policy.max_implementation_executions == 2, "policy: 2 implementation executions",
          "default implementation limit is not 2");
    check(policy.max_review_rounds == 3, "policy: 3 review rounds", "default review round limit is not 3");
    check(policy.blocked_repeat_budget == 1, "policy: 1 BLOCKED repeat unit", "default BLOCKED budget is not 1");
}

void test_transition_is_deterministic() {
    const Policy policy;
    JobRecord job = to_review(policy);
    Event approve = with_verdict(EventKind::ReviewVerdict, Verdict::Approve);
    TransitionResult first = transition(job, approve, policy);
    TransitionResult second = transition(job, approve, policy);
    check(first.accepted == second.accepted && first.reason == second.reason &&
              first.next.state == second.next.state,
          "transition is deterministic", "same input gave different results");
}

}  // namespace

int main() {
    test_happy_path_to_next_issue();
    test_full_workflow_with_research_and_design();
    test_pr_path_skipped_on_retry();
    test_review_rejected_before_qa_and_ci_on_retry();
    test_implementation_budget_is_two_total();
    test_ci_failure_consumes_attempt();
    test_qa_must_precede_ci();
    test_pr_head_mismatch_rejected();
    test_repeat_review_on_same_head_rejected();
    test_review_rounds_exhausted_escalates();
    test_blocked_is_diagnostic_then_escalates();
    test_paused_job_rejects_work_events();
    test_blocked_review_does_not_count_round();
    test_disagreement_reevaluates_once();
    test_second_disagreement_escalates();
    test_security_sensitive_fails_closed_at_review();
    test_merge_gates_each_block_merge();
    test_escalation_is_absorbing();
    test_escalation_requires_reason();
    test_no_next_issue_after_terminal();
    test_policy_defaults_match_contract();
    test_transition_is_deterministic();

    std::cout << "\n" << g_passes << " passed, " << g_failures << " failed\n";
    return g_failures == 0 ? 0 : 1;
}
