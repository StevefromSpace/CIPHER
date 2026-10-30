#ifndef CIPHER_ORCHESTRATION_STATE_MACHINE_HPP
#define CIPHER_ORCHESTRATION_STATE_MACHINE_HPP

#include <string>

namespace cipher::orchestration {

// Pure, deterministic workflow state machine for the CIPHER orchestration layer (Stage 1).
// It owns no GitHub, model, file or process state. Callers supply events; the machine returns
// the next record or a rejection. Contract: docs/AGENT_ROLE_CONTRACT.md (sections 2, 5, 6, 8).

enum class State {
    Issue,
    Research,
    Architecture,
    Implementation,
    PullRequest,
    Qa,
    Ci,
    Review,
    Merge,
    NextIssue,   // terminal: the issue finished with a merged PR
    Blocked,     // paused after the first BLOCKED; resumes to resume_state, counters unchanged
    NeedsHuman   // terminal: explicit owner handoff; never a retry state
};

enum class Verdict { None, Approve, ChangesRequested, Blocked, NeedsHuman };

// Why the job stopped. Used for failure reporting (contract section 11).
enum class EscalationReason {
    None,
    Ambiguity,
    ScopeExpansion,
    SecurityConcern,
    SecurityReviewUnavailable,   // independent (Gemini) security reviewer not wired (sections 1.5, 8.2)
    ProtectedOperation,          // owner-gated operation (section 6)
    RetryBudgetExhausted,        // 2 implementation executions used (section 5.1)
    ReviewRoundsExhausted,       // 3 review rounds without APPROVE (section 5.2)
    DisagreementUnresolved,      // second disagreement after the one re-evaluation (section 5.3)
    BlockedRepeated,             // second BLOCKED on the job (section 5.5)
    NoProgress,                  // repair produced the same findings (section 5.4)
    ExplicitRequest              // a role emitted NEEDS_HUMAN
};

enum class EventKind {
    Start,                 // issue accepted for work
    ResearchComplete,
    ArchitectureComplete,
    ImplementationComplete,  // execution produced a head SHA
    ImplementationFailed,    // execution ended without a usable head
    PullRequestOpened,
    QaResult,
    CiResult,
    ReviewVerdict,
    Disagreement,          // orchestrator reports two non-BLOCKED verdicts on one head that differ
    MergeRequest,          // accepted only when every section 8.2 gate holds; merge then ends the issue
    Blocked,               // any active state; BLOCKED result or tooling failure
    BlockedResumed,        // external cause removed
    Escalate               // any non-terminal state; requires a reason
};

// Facts known when the issue is accepted. Derived from labels and changed paths by a later stage.
struct IssueFacts {
    bool research = false;
    bool needs_design = false;
    bool security_sensitive = false;   // contract section 7 path touched
};

// Merge evidence the machine cannot derive from its own record (contract section 8.2 gates 5-7).
struct MergeEvidence {
    bool no_open_blocker_or_high = false;
    bool scope_within_accepted = false;
    bool protected_content_absent = false;
};

struct Event {
    EventKind kind = EventKind::Start;
    std::string head_sha;                              // PR head the event refers to, if any
    Verdict verdict = Verdict::None;                   // QaResult, ReviewVerdict, Disagreement (first)
    Verdict second_verdict = Verdict::None;            // Disagreement only
    bool ci_passed = false;                            // CiResult
    MergeEvidence evidence;                            // MergeRequest
    EscalationReason reason = EscalationReason::None;  // Escalate
};

// Limits from the contract. Defaults are the contract values; tests may lower or raise them.
struct Policy {
    int max_implementation_executions = 2;   // section 5.1
    int max_review_rounds = 3;               // section 5.2
    int blocked_repeat_budget = 1;           // section 5.5
};

// Counters and flags are stored here, in the job record (section 5: never in the agent's own count).
struct JobRecord {
    State state = State::Issue;
    State resume_state = State::Issue;
    EscalationReason escalation = EscalationReason::None;
    IssueFacts facts;
    int implementation_executions = 0;
    int review_rounds = 0;
    int blocked_count = 0;                   // every BLOCKED seen on the job
    bool reevaluation_used = false;          // one higher-tier re-evaluation per job (section 5.3)
    bool reevaluation_pending = false;
    bool pull_request_open = false;          // retries push to the existing PR, never open a second one
    bool qa_approved = false;                // per attempt; cleared when a retry starts
    bool ci_passed = false;
    bool review_approved = false;
    std::string head_sha;                    // current PR head
    std::string reviewed_head_sha;           // head of the last counted review
};

struct TransitionResult {
    bool accepted = false;
    JobRecord next;
    std::string reason;   // fixed text; identical for identical inputs
};

TransitionResult transition(const JobRecord& job, const Event& event, const Policy& policy);

bool is_terminal(State state);
const char* to_string(State state);
const char* to_string(EscalationReason reason);

}  // namespace cipher::orchestration

#endif  // CIPHER_ORCHESTRATION_STATE_MACHINE_HPP
