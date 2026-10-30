#include "cipher/orchestration/state_machine.hpp"

#include <utility>

namespace cipher::orchestration {
namespace {

TransitionResult accept(JobRecord next) {
    TransitionResult result;
    result.accepted = true;
    result.next = std::move(next);
    result.reason = "accepted";
    return result;
}

TransitionResult reject(const JobRecord& job, const char* why) {
    TransitionResult result;
    result.next = job;
    result.reason = why;
    return result;
}

TransitionResult escalate(JobRecord job, EscalationReason reason) {
    job.state = State::NeedsHuman;
    job.escalation = reason;
    return accept(std::move(job));
}

// Starts an implementation execution. The first start and every retry share this path, so the
// 2-execution limit (section 5.1) is enforced in one place. A retry clears the per-attempt gates.
TransitionResult begin_execution(JobRecord job, const Policy& policy) {
    if (job.implementation_executions >= policy.max_implementation_executions) {
        return escalate(std::move(job), EscalationReason::RetryBudgetExhausted);
    }
    ++job.implementation_executions;
    job.state = State::Implementation;
    job.qa_approved = false;
    job.ci_passed = false;
    job.review_approved = false;
    job.reevaluation_pending = false;
    return accept(std::move(job));
}

// BLOCKED is a diagnostic stop on first occurrence. Each later one spends the repeat budget
// (section 5.5). It never starts an execution and never resets counters.
TransitionResult block(JobRecord job, const Policy& policy) {
    ++job.blocked_count;
    if (job.blocked_count - 1 >= policy.blocked_repeat_budget) {
        return escalate(std::move(job), EscalationReason::BlockedRepeated);
    }
    if (job.state != State::Blocked) {
        job.resume_state = job.state;
        job.state = State::Blocked;
    }
    return accept(std::move(job));
}

// Shared handling for a QA, CI or review result that is not a clean pass.
TransitionResult handle_non_pass(const JobRecord& job, Verdict verdict, const Policy& policy) {
    if (verdict == Verdict::Blocked) {
        return block(job, policy);
    }
    if (verdict == Verdict::NeedsHuman) {
        return escalate(job, EscalationReason::ExplicitRequest);
    }
    return begin_execution(job, policy);
}

TransitionResult on_qa(const JobRecord& job, const Event& event, const Policy& policy) {
    if (job.state != State::Qa) return reject(job, "QA result outside QA");
    if (event.head_sha != job.head_sha) return reject(job, "QA result names a different head");
    if (event.verdict == Verdict::None) return reject(job, "QA result has no verdict");
    if (event.verdict != Verdict::Approve) return handle_non_pass(job, event.verdict, policy);
    JobRecord next = job;
    next.qa_approved = true;
    next.state = State::Ci;
    return accept(std::move(next));
}

TransitionResult on_ci(const JobRecord& job, const Event& event, const Policy& policy) {
    if (job.state != State::Ci) return reject(job, "CI result outside CI");
    if (event.head_sha != job.head_sha) return reject(job, "CI result names a different head");
    if (!job.qa_approved) return reject(job, "CI before QA approval");
    if (!event.ci_passed) return begin_execution(job, policy);
    JobRecord next = job;
    next.ci_passed = true;
    // Section 7: security-sensitive work needs an independent (Gemini) reviewer, not wired yet.
    // Fail closed rather than run a same-family review.
    if (job.facts.security_sensitive) {
        return escalate(std::move(next), EscalationReason::SecurityReviewUnavailable);
    }
    next.state = State::Review;
    return accept(std::move(next));
}

TransitionResult on_review(const JobRecord& job, const Event& event, const Policy& policy) {
    if (job.state != State::Review) return reject(job, "review verdict outside Review");
    if (event.head_sha != job.head_sha) return reject(job, "review names a different head (section 2.1)");
    if (event.verdict == Verdict::None) return reject(job, "review has no verdict");
    // A repeat on the same head is not a new round (section 5.2). The one re-evaluation after a
    // disagreement is the exception, because the disagreement already counted the round.
    if (job.reviewed_head_sha == job.head_sha && !job.reevaluation_pending) {
        return reject(job, "head already reviewed (section 5.2)");
    }
    if (event.verdict == Verdict::Blocked || event.verdict == Verdict::NeedsHuman) {
        return handle_non_pass(job, event.verdict, policy);
    }
    JobRecord next = job;
    if (!next.reevaluation_pending) {
        ++next.review_rounds;
        next.reviewed_head_sha = job.head_sha;
    }
    next.reevaluation_pending = false;
    if (event.verdict == Verdict::Approve) {
        next.review_approved = true;
        next.state = State::Merge;
        return accept(std::move(next));
    }
    if (next.review_rounds >= policy.max_review_rounds) {
        return escalate(std::move(next), EscalationReason::ReviewRoundsExhausted);
    }
    return begin_execution(next, policy);
}

TransitionResult on_disagreement(const JobRecord& job, const Event& event) {
    if (job.state != State::Review) return reject(job, "disagreement outside Review");
    if (event.head_sha != job.head_sha) return reject(job, "disagreement names a different head");
    const bool both_decided = event.verdict != Verdict::None && event.second_verdict != Verdict::None;
    const bool neither_blocked = event.verdict != Verdict::Blocked && event.second_verdict != Verdict::Blocked;
    if (!both_decided || !neither_blocked || event.verdict == event.second_verdict) {
        return reject(job, "not a disagreement between two decided verdicts");
    }
    if (job.reevaluation_used) return escalate(job, EscalationReason::DisagreementUnresolved);
    // Section 5.3 asks for one higher-tier re-evaluation. Section 5.2 forbids a second review of
    // the same head, so the re-evaluation is a one-shot exception that does not count as a round.
    JobRecord next = job;
    next.reevaluation_used = true;
    next.reevaluation_pending = true;
    ++next.review_rounds;
    next.reviewed_head_sha = job.head_sha;
    return accept(std::move(next));
}

TransitionResult on_merge(const JobRecord& job, const Event& event) {
    if (job.state != State::Merge) return reject(job, "merge request outside Merge");
    if (event.head_sha != job.head_sha) return reject(job, "merge request names a different head");
    if (job.facts.security_sensitive) {
        return escalate(job, EscalationReason::SecurityReviewUnavailable);   // gate 4
    }
    if (!event.evidence.scope_within_accepted) return escalate(job, EscalationReason::ScopeExpansion);   // gate 6
    if (!event.evidence.protected_content_absent) return escalate(job, EscalationReason::ProtectedOperation);   // gate 7
    if (!job.ci_passed) return reject(job, "merge gate 1 not met: CI");
    if (!job.qa_approved) return reject(job, "merge gate 2 not met: QA");
    if (!job.review_approved) return reject(job, "merge gate 3 not met: review");
    if (!event.evidence.no_open_blocker_or_high) return reject(job, "merge gate 5 not met: open BLOCKER or HIGH");
    JobRecord next = job;
    next.state = State::NextIssue;
    return accept(std::move(next));
}

}  // namespace

TransitionResult transition(const JobRecord& job, const Event& event, const Policy& policy) {
    if (is_terminal(job.state)) return reject(job, "job is terminal");
    if (job.state == State::Blocked && event.kind != EventKind::Blocked &&
        event.kind != EventKind::BlockedResumed && event.kind != EventKind::Escalate) {
        return reject(job, "job is paused on BLOCKED");
    }

    switch (event.kind) {
        case EventKind::Start: {
            if (job.state != State::Issue) return reject(job, "start outside Issue");
            JobRecord next = job;
            if (job.facts.research) {
                next.state = State::Research;
                return accept(std::move(next));
            }
            if (job.facts.needs_design) {
                next.state = State::Architecture;
                return accept(std::move(next));
            }
            return begin_execution(next, policy);
        }
        case EventKind::ResearchComplete: {
            if (job.state != State::Research) return reject(job, "research complete outside Research");
            JobRecord next = job;
            if (job.facts.needs_design) {
                next.state = State::Architecture;
                return accept(std::move(next));
            }
            return begin_execution(next, policy);
        }
        case EventKind::ArchitectureComplete:
            if (job.state != State::Architecture) return reject(job, "architecture complete outside Architecture");
            return begin_execution(job, policy);
        case EventKind::ImplementationComplete: {
            if (job.state != State::Implementation) return reject(job, "implementation complete outside Implementation");
            if (event.head_sha.empty()) return reject(job, "implementation produced no head");
            JobRecord next = job;
            next.head_sha = event.head_sha;
            // Attempt 2 pushes to the open PR and goes straight to QA (section 8.1).
            next.state = job.pull_request_open ? State::Qa : State::PullRequest;
            return accept(std::move(next));
        }
        case EventKind::ImplementationFailed:
            if (job.state != State::Implementation) return reject(job, "implementation failed outside Implementation");
            return begin_execution(job, policy);
        case EventKind::PullRequestOpened: {
            if (job.state != State::PullRequest) return reject(job, "PR opened outside PullRequest");
            JobRecord next = job;
            next.pull_request_open = true;
            next.state = State::Qa;
            return accept(std::move(next));
        }
        case EventKind::QaResult:
            return on_qa(job, event, policy);
        case EventKind::CiResult:
            return on_ci(job, event, policy);
        case EventKind::ReviewVerdict:
            return on_review(job, event, policy);
        case EventKind::Disagreement:
            return on_disagreement(job, event);
        case EventKind::MergeRequest:
            return on_merge(job, event);
        case EventKind::Blocked:
            return block(job, policy);
        case EventKind::BlockedResumed: {
            if (job.state != State::Blocked) return reject(job, "resume outside Blocked");
            JobRecord next = job;
            next.state = job.resume_state;
            return accept(std::move(next));
        }
        case EventKind::Escalate:
            if (event.reason == EscalationReason::None) return reject(job, "escalation needs a reason");
            return escalate(job, event.reason);
    }
    return reject(job, "unknown event");
}

bool is_terminal(State state) {
    return state == State::NextIssue || state == State::NeedsHuman;
}

const char* to_string(State state) {
    switch (state) {
        case State::Issue: return "ISSUE";
        case State::Research: return "RESEARCH";
        case State::Architecture: return "ARCHITECTURE";
        case State::Implementation: return "IMPLEMENTATION";
        case State::PullRequest: return "PR";
        case State::Qa: return "QA";
        case State::Ci: return "CI";
        case State::Review: return "REVIEW";
        case State::Merge: return "MERGE";
        case State::NextIssue: return "NEXT_ISSUE";
        case State::Blocked: return "BLOCKED";
        case State::NeedsHuman: return "NEEDS_HUMAN";
    }
    return "UNKNOWN";
}

const char* to_string(EscalationReason reason) {
    switch (reason) {
        case EscalationReason::None: return "none";
        case EscalationReason::Ambiguity: return "ambiguity";
        case EscalationReason::ScopeExpansion: return "scope expansion";
        case EscalationReason::SecurityConcern: return "security concern";
        case EscalationReason::SecurityReviewUnavailable: return "independent security review unavailable";
        case EscalationReason::ProtectedOperation: return "protected operation";
        case EscalationReason::RetryBudgetExhausted: return "implementation budget exhausted";
        case EscalationReason::ReviewRoundsExhausted: return "review rounds exhausted";
        case EscalationReason::DisagreementUnresolved: return "reviewer disagreement unresolved";
        case EscalationReason::BlockedRepeated: return "repeated BLOCKED";
        case EscalationReason::NoProgress: return "no progress";
        case EscalationReason::ExplicitRequest: return "explicit NEEDS_HUMAN";
    }
    return "unknown";
}

}  // namespace cipher::orchestration
