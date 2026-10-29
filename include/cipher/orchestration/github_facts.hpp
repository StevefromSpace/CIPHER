#ifndef CIPHER_ORCHESTRATION_GITHUB_FACTS_HPP
#define CIPHER_ORCHESTRATION_GITHUB_FACTS_HPP

#include "cipher/orchestration/state_machine.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cipher::orchestration {

// Stage 2: normalized, read-only view of GitHub state. Nothing here calls the network. A GitHub reader
// fills these structs; this layer turns them into Stage 1 facts and events. The state machine never sees
// GitHub data directly.

enum class CheckState { Pending, Passed, Failed };
enum class ReviewState { None, Approved, ChangesRequested };

struct PullRequestFacts {
    int number = 0;
    bool open = false;
    bool merged = false;
    std::string head_sha;
    std::string base_sha;
    std::vector<std::string> labels;
    CheckState checks = CheckState::Pending;
    ReviewState review = ReviewState::None;
    std::string reviewed_head_sha;   // head the review names; a review on another head is stale
};

struct IssueObservation {
    int number = 0;
    bool open = false;
    std::vector<std::string> labels;
};

// True only when the PR is open and points at the head the job is working on.
bool is_current_head(const PullRequestFacts& pr, const std::string& job_head_sha);

// Maps issue labels to the facts the state machine starts from. Label names are owner-configured; see the
// external-prerequisites section of the Stage 1-9 report.
IssueFacts issue_facts_from(const IssueObservation& issue);

// CiResult for the PR head once checks are terminal. nullopt while pending or when no head is known.
std::optional<Event> ci_event(const PullRequestFacts& pr);

// ReviewVerdict only when the review names the current head. nullopt for no review or a stale one.
std::optional<Event> review_event(const PullRequestFacts& pr);

}  // namespace cipher::orchestration

#endif  // CIPHER_ORCHESTRATION_GITHUB_FACTS_HPP
