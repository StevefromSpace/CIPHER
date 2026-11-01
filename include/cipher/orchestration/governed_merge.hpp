#ifndef CIPHER_ORCHESTRATION_GOVERNED_MERGE_HPP
#define CIPHER_ORCHESTRATION_GOVERNED_MERGE_HPP

#include <filesystem>
#include <optional>
#include <string>

namespace cipher::orchestration {

// Merge decision for one PR head. The contract (docs/AGENT_ROLE_CONTRACT.md section 8.2) defines seven gates.
// Three adapter-level preconditions are added: the PR is open, GitHub reports it mergeable, and the head
// re-read at merge time is the head that was gated. The caller supplies every boolean from fresh reads.
struct MergeGateInputs {
    bool pr_open = false;
    bool head_matches_gated_sha = false;   // re-read head SHA equals the SHA the gates were evaluated on
    bool mergeable = false;                // GitHub MERGEABLE, no conflicts with base

    bool ci_all_required_passed = false;   // gate 1
    bool qa_approved_or_not_applicable = false;   // gate 2
    bool reviewer_approved = false;        // gate 3
    bool security_not_applicable_or_approved = false;   // gate 4 (independent family, section 1.5)
    bool no_open_blocker_or_high = false;  // gate 5
    bool scope_accepted = false;           // gate 6
    bool no_protected_content = false;     // gate 7
};

enum class MergeBlock {
    None,                       // every gate passed
    PrNotOpen,
    HeadChanged,                // re-read head differs from the gated head: stop, re-run gates
    NotMergeable,
    CiNotPassed,
    QaNotApproved,
    ReviewerNotApproved,
    SecurityNotApproved,        // NEEDS_HUMAN: independent security family unavailable or not approved
    BlockerOrHighOpen,
    ScopeNotAccepted,           // NEEDS_HUMAN: scope expansion (section 9)
    ProtectedContent            // NEEDS_HUMAN: protected operation (section 6)
};

struct MergeDecision {
    bool allowed = false;
    MergeBlock block = MergeBlock::None;
    bool needs_human = false;   // true for the blocks the contract escalates instead of retrying
};

// Pure: first failing gate in contract order. Same inputs always give the same decision.
MergeDecision evaluate_merge_gates(const MergeGateInputs& inputs);

// Serializes merges across jobs. Exclusive creation of <dir>/merge.lock; a held lock refuses, it never waits or
// steals. A stale lock is a human decision, so it is never removed automatically.
class MergeLock {
public:
    explicit MergeLock(std::filesystem::path directory);

    bool try_acquire(int issue_number);
    void release(int issue_number);
    std::optional<int> holder() const;   // issue number recorded in the lock file, if any

    std::filesystem::path lock_path() const;

private:
    std::filesystem::path directory_;
};

}  // namespace cipher::orchestration

#endif  // CIPHER_ORCHESTRATION_GOVERNED_MERGE_HPP
