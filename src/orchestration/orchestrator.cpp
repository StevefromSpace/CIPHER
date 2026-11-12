#include "cipher/orchestration/orchestrator.hpp"

#include <exception>

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

void Orchestrator::assign_run_id(const std::string& run_id) {
    if (!job_.run_id.empty() || run_id.empty()) return;
    job_.run_id = run_id;
    store_.save(issue_number_, job_);
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
        // Save first: if the save throws, the in-memory job must not show a transition the disk does not have.
        store_.save(issue_number_, result.next);
        job_ = result.next;
    }
    record.to = job_.state;
    record.escalation = job_.escalation;
    records_.push_back(record);
    log_record(record);
    return result.accepted;
}

void Orchestrator::attach_event_log(EventLog& log) {
    event_log_ = &log;
}

const std::string& Orchestrator::log_error() const {
    return log_error_;
}

void Orchestrator::log_record(const ExecutionRecord& record) {
    if (event_log_ == nullptr) return;
    // Reason text is produced by transition(), but a log line must stay one line with three fields.
    std::string reason = record.reason;
    for (char& ch : reason) {
        if (ch == '\t' || ch == '\r' || ch == '\n') ch = ' ';
    }
    std::string detail = std::string("from=") + to_string(record.from) + " to=" + to_string(record.to) +
                         " accepted=" + (record.accepted ? "1" : "0") + " attempt=" +
                         std::to_string(record.attempt) + " reason=" + reason;
    if (record.escalation != EscalationReason::None) {
        detail += std::string(" escalation=") + to_string(record.escalation);
    }
    // The record was saved before this call, so a log failure must not turn a persisted transition into an
    // error (a retry would be rejected or, worse, charged again). Keep the first failure for the caller.
    try {
        event_log_->append(issue_number_, "transition", detail, job_.run_id);
    } catch (const std::exception& error) {
        if (log_error_.empty()) log_error_ = error.what();
    }
}

bool Orchestrator::advance() {
    const std::optional<Role> role = role_for(job_.state);
    if (!role) return false;
    // A flagged record is an execution that may have run already: recover() must decide before anything dispatches.
    if (job_.execution_in_flight) return false;

    // Only an implementation execution counts against the budget (section 5.1, charge on dispatch). The charge
    // and the in-flight flag are saved together before dispatch, so a crash during the run is visible on restart.
    const bool counted = *role == Role::Implementation && job_.state == State::Implementation;
    const std::string previous_family = job_.implementation_family;
    if (counted) {
        if (job_.implementation_executions >= policy_.max_implementation_executions) {
            return apply(escalation_event(EscalationReason::RetryBudgetExhausted));  // defensive: no dispatch
        }
        const int previous_executions = job_.implementation_executions;
        job_.execution_in_flight = true;
        ++job_.implementation_executions;
        try {
            store_.save(issue_number_, job_);
        } catch (...) {
            // Not dispatched: the save did not land, so the charge and flag are rolled back in memory.
            job_.execution_in_flight = false;
            job_.implementation_executions = previous_executions;
            throw;
        }
    }
    const RoleResult result = executor_.run(*role, job_);
    if (counted) {
        // Flag and family are cleared in memory only: apply() persists them with the transition in one save.
        job_.execution_in_flight = false;
        job_.implementation_family = result.model_family;
    }

    // A security-sensitive review must come from a model family other than the implementation family
    // (contract sections 1.5, 8.2). An empty or equal family is treated as no independent review.
    bool accepted = false;
    try {
        if (*role == Role::Reviewer && job_.facts.security_sensitive &&
            (result.model_family.empty() || result.model_family == job_.implementation_family)) {
            accepted = apply(escalation_event(EscalationReason::SecurityReviewUnavailable), result.model_family);
        } else {
            accepted = apply(result.event, result.model_family);
        }
    } catch (...) {
        if (counted) restore_in_flight(previous_family);
        throw;
    }
    // Rejected: the disk still holds the flag from the first save, so memory must match it.
    if (!accepted && counted) restore_in_flight(previous_family);
    return accepted;
}

void Orchestrator::restore_in_flight(const std::string& previous_family) {
    job_.execution_in_flight = true;
    job_.implementation_family = previous_family;
}

bool Orchestrator::recover() {
    if (!job_.execution_in_flight) return false;

    // Clear the flag in memory only: apply() persists flag and failure charge in one atomic save, so a crash
    // here either leaves the job still flagged (recover again) or fully charged, never uncharged and unflagged.
    job_.execution_in_flight = false;
    Event event;
    event.kind = EventKind::ImplementationFailed;
    bool accepted = false;
    try {
        accepted = apply(event);
    } catch (...) {
        job_.execution_in_flight = true;  // save failed: disk still flagged
        throw;
    }
    if (accepted) return true;
    job_.execution_in_flight = true;  // rejected (inconsistent record): stay flagged on disk and in memory
    return false;
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
