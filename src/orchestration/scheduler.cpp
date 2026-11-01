#include "cipher/orchestration/scheduler.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace cipher::orchestration {
namespace {

bool holds_slot(State state) {
    return state != State::Issue && state != State::NextIssue && state != State::NeedsHuman;
}

// Depth-first walk of the dependency graph from one job. Returns true when a dependency cycle is reachable.
bool reaches_cycle(int issue_number, const std::map<int, const SchedulerJob*>& by_issue,
                   std::set<int>& on_path, std::set<int>& cleared) {
    if (cleared.count(issue_number)) return false;
    if (on_path.count(issue_number)) return true;
    const auto found = by_issue.find(issue_number);
    if (found == by_issue.end()) return false;   // unknown issues are reported by the caller, not here
    on_path.insert(issue_number);
    for (const int dependency : found->second->depends_on) {
        if (reaches_cycle(dependency, by_issue, on_path, cleared)) return true;
    }
    on_path.erase(issue_number);
    cleared.insert(issue_number);
    return false;
}

Decision decide_for_queued(const SchedulerJob& job, const std::map<int, const SchedulerJob*>& by_issue,
                           int& active, int max_parallel_jobs) {
    Decision decision{job.issue_number, Action::Wait, EscalationReason::None};
    if (job.cancelled) {
        decision.action = Action::Escalate;
        decision.reason = EscalationReason::ExplicitRequest;
        return decision;
    }

    std::set<int> on_path;
    std::set<int> cleared;
    if (reaches_cycle(job.issue_number, by_issue, on_path, cleared)) {
        decision.action = Action::Escalate;
        decision.reason = EscalationReason::Ambiguity;
        return decision;
    }

    bool complete = true;
    for (const int dependency : job.depends_on) {
        const auto found = by_issue.find(dependency);
        if (found == by_issue.end()) {
            decision.action = Action::Escalate;
            decision.reason = EscalationReason::Ambiguity;   // unknown dependency
            return decision;
        }
        const State dependency_state = found->second->state;
        if (dependency_state == State::NeedsHuman) {
            decision.action = Action::Escalate;
            decision.reason = EscalationReason::Ambiguity;   // dependency needs the owner first
            return decision;
        }
        if (dependency_state != State::NextIssue) complete = false;
    }

    if (!complete || job.paused) return decision;
    if (active >= max_parallel_jobs) return decision;
    ++active;
    decision.action = Action::Admit;
    return decision;
}

}  // namespace

std::vector<Decision> plan_schedule(const std::vector<SchedulerJob>& jobs, int max_parallel_jobs, GlobalStop stop) {
    if (stop != GlobalStop::None) return {};

    std::map<int, const SchedulerJob*> by_issue;
    for (const auto& job : jobs) {
        if (!by_issue.emplace(job.issue_number, &job).second) {
            throw std::invalid_argument("plan_schedule: duplicate issue number");
        }
    }

    // Capacity comes from the states passed in, which the caller persisted. Nothing is remembered between calls.
    int active = 0;
    for (const auto& job : jobs) {
        if (holds_slot(job.state)) ++active;
    }

    std::vector<const SchedulerJob*> queued;
    for (const auto& job : jobs) {
        if (job.state == State::Issue) queued.push_back(&job);
    }
    std::sort(queued.begin(), queued.end(),
              [](const SchedulerJob* a, const SchedulerJob* b) { return a->issue_number < b->issue_number; });

    std::vector<Decision> decisions;
    decisions.reserve(queued.size());
    for (const SchedulerJob* job : queued) {
        decisions.push_back(decide_for_queued(*job, by_issue, active, max_parallel_jobs));
    }
    return decisions;
}

}  // namespace cipher::orchestration
