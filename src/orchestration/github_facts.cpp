#include "cipher/orchestration/github_facts.hpp"

namespace cipher::orchestration {

namespace {

// Owner-configured label names. Not present in the repository yet; creating them is owner setup.
constexpr const char* kResearchLabel = "needs-research";
constexpr const char* kDesignLabel = "needs-design";
constexpr const char* kSecurityLabel = "security-sensitive";

bool has_label(const std::vector<std::string>& labels, const char* wanted) {
    for (const auto& label : labels) {
        if (label == wanted) return true;
    }
    return false;
}

}  // namespace

bool is_current_head(const PullRequestFacts& pr, const std::string& job_head_sha) {
    return pr.open && !pr.head_sha.empty() && pr.head_sha == job_head_sha;
}

IssueFacts issue_facts_from(const IssueObservation& issue) {
    IssueFacts facts;
    facts.research = has_label(issue.labels, kResearchLabel);
    facts.needs_design = has_label(issue.labels, kDesignLabel);
    facts.security_sensitive = has_label(issue.labels, kSecurityLabel);
    return facts;
}

std::optional<Event> ci_event(const PullRequestFacts& pr) {
    if (pr.head_sha.empty() || pr.checks == CheckState::Pending) return std::nullopt;
    Event event;
    event.kind = EventKind::CiResult;
    event.head_sha = pr.head_sha;
    event.ci_passed = pr.checks == CheckState::Passed;
    return event;
}

std::optional<Event> review_event(const PullRequestFacts& pr) {
    if (pr.head_sha.empty() || pr.review == ReviewState::None || pr.reviewed_head_sha != pr.head_sha) {
        return std::nullopt;
    }
    Event event;
    event.kind = EventKind::ReviewVerdict;
    event.head_sha = pr.head_sha;
    event.verdict = pr.review == ReviewState::Approved ? Verdict::Approve : Verdict::ChangesRequested;
    return event;
}

}  // namespace cipher::orchestration
