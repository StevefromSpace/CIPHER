#include "cipher/orchestration/orchestrator.hpp"

namespace cipher::orchestration {

void InMemoryJobStore::save(int issue_number, const JobRecord& job) {
    jobs_[issue_number] = job;
}

std::optional<JobRecord> InMemoryJobStore::load(int issue_number) const {
    const auto found = jobs_.find(issue_number);
    if (found == jobs_.end()) return std::nullopt;
    return found->second;
}

Orchestrator::Orchestrator(int issue_number, IssueFacts facts, RoleExecutor& executor, JobStore& store, Policy policy)
    : issue_number_(issue_number), executor_(executor), store_(store), policy_(policy) {
    if (const auto saved = store_.load(issue_number_)) {
        job_ = *saved;
    } else {
        job_.facts = facts;
        store_.save(issue_number_, job_);
    }
}

bool Orchestrator::start() {
    if (job_.state != State::Issue) return false;
    Event event;
    event.kind = EventKind::Start;
    return apply(event);
}

bool Orchestrator::apply(const Event& event, const std::string& model_family) {
    const TransitionResult result = transition(job_, event, policy_);

    ExecutionRecord record;
    record.issue_number = issue_number_;
    record.from = job_.state;
    record.event = event.kind;
    record.head_sha = event.head_sha;
    record.model_family = model_family;
    record.attempt = job_.implementation_executions;
    record.accepted = result.accepted;
    record.reason = result.reason;

    if (result.accepted) {
        job_ = result.next;
        store_.save(issue_number_, job_);
    }
    record.to = job_.state;
    record.escalation = job_.escalation;
    records_.push_back(record);
    return result.accepted;
}

bool Orchestrator::advance() {
    const std::optional<Role> role = role_for(job_.state);
    if (!role) return false;

    // Only an implementation execution counts against the budget. Mark it before the role runs, so a crash
    // during the run is visible on restart (see recover()).
    const bool counted = *role == Role::Implementation && job_.state == State::Implementation;
    if (counted) {
        job_.execution_in_flight = true;
        store_.save(issue_number_, job_);
    }
    const RoleResult result = executor_.run(*role, job_);
    if (counted) {
        job_.execution_in_flight = false;
        job_.implementation_family = result.model_family;
        store_.save(issue_number_, job_);
    }

    // A security-sensitive review must come from a model family other than the implementation family
    // (contract sections 1.5, 8.2). An empty or equal family is treated as no independent review.
    if (*role == Role::Reviewer && job_.facts.security_sensitive) {
        const bool independent = !result.model_family.empty() && result.model_family != job_.implementation_family;
        if (!independent) {
            return apply(escalation_event(EscalationReason::SecurityReviewUnavailable), result.model_family);
        }
    }
    return apply(result.event, result.model_family);
}

bool Orchestrator::recover() {
    if (!job_.execution_in_flight) return false;

    job_.execution_in_flight = false;
    store_.save(issue_number_, job_);
    Event event;
    event.kind = EventKind::ImplementationFailed;
    return apply(event);
}

const JobRecord& Orchestrator::job() const {
    return job_;
}

const std::vector<ExecutionRecord>& Orchestrator::records() const {
    return records_;
}

std::optional<Role> Orchestrator::role_for(State state) const {
    switch (state) {
        case State::Research:
            return Role::Research;
        case State::Architecture:
            return Role::Architecture;
        case State::Implementation:
        case State::PullRequest:   // the implementation role opens the PR and reports PullRequestOpened
            return Role::Implementation;
        case State::Qa:
            return Role::Qa;
        case State::Review:
            return Role::Reviewer;
        case State::Issue:
        case State::Ci:            // CI is observed on GitHub, not produced by a role
        case State::Merge:         // merge is a GitHub write, not in this stage
        case State::NextIssue:
        case State::Blocked:
        case State::NeedsHuman:
            return std::nullopt;
    }
    return std::nullopt;
}

Event Orchestrator::escalation_event(EscalationReason reason) const {
    Event event;
    event.kind = EventKind::Escalate;
    event.reason = reason;
    return event;
}

}  // namespace cipher::orchestration
