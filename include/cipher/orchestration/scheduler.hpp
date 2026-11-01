#ifndef CIPHER_ORCHESTRATION_SCHEDULER_HPP
#define CIPHER_ORCHESTRATION_SCHEDULER_HPP

#include "cipher/orchestration/state_machine.hpp"

#include <vector>

namespace cipher::orchestration {

// Stage 9 scheduler: a pure function from persisted job states to admission decisions. It holds no counter
// of its own, so a restart, a duplicate event or a re-poll cannot admit more jobs than the limit allows.

inline constexpr int kDefaultMaxParallelJobs = 3;

// A job that is queued (State::Issue) is admitted when a slot is free and its dependencies are complete.
// A job that has started holds a slot until it reaches NextIssue or NeedsHuman.
struct SchedulerJob {
    int issue_number = 0;
    State state = State::Issue;
    std::vector<int> depends_on;   // structured config only; a dependency is complete only at NextIssue
    bool paused = false;           // owner pause: not admitted, does not hold a slot
    bool cancelled = false;        // owner cancel: escalated, never admitted
};

// Stops that apply to every job. Anything else stays local to the job it concerns.
enum class GlobalStop { None, WorktreeConflict, AuthAnomaly, RulesetChange };

enum class Action { Wait, Admit, Escalate };

struct Decision {
    int issue_number = 0;
    Action action = Action::Wait;
    EscalationReason reason = EscalationReason::None;   // set only for Escalate
};

// Decisions for queued jobs only, in ascending issue order. Returns nothing when a global stop is set.
// Throws std::invalid_argument on a duplicate issue number, since the state of that issue would be ambiguous.
std::vector<Decision> plan_schedule(const std::vector<SchedulerJob>& jobs,
                                    int max_parallel_jobs = kDefaultMaxParallelJobs,
                                    GlobalStop stop = GlobalStop::None);

}  // namespace cipher::orchestration

#endif  // CIPHER_ORCHESTRATION_SCHEDULER_HPP
