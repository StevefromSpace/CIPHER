#ifndef CIPHER_ORCHESTRATION_ORCHESTRATOR_HPP
#define CIPHER_ORCHESTRATION_ORCHESTRATOR_HPP

#include "cipher/orchestration/state_machine.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cipher::orchestration {

// Stage 3-4: orchestration policy. The orchestrator asks a role for work, then feeds the role's event to
// transition(). It never changes a JobRecord field itself. Contract: docs/AGENT_ROLE_CONTRACT.md.

enum class Role { Research, Architecture, Implementation, Qa, Security, Reviewer };

struct RoleResult {
    Event event;                  // what the role reports; validated by transition()
    std::string model_family;     // family of the model that ran the role; empty when unknown
};

// Execution boundary (Stage 4). The orchestrator names a role; the executor picks the model or tool.
class RoleExecutor {
public:
    virtual ~RoleExecutor() = default;
    virtual RoleResult run(Role role, const JobRecord& job) = 0;
};

// Persistence seam (Stage 3). A durable store can replace the in-memory one without changing policy.
class JobStore {
public:
    virtual ~JobStore() = default;
    virtual void save(int issue_number, const JobRecord& job) = 0;
    virtual std::optional<JobRecord> load(int issue_number) const = 0;
};

class InMemoryJobStore final : public JobStore {
public:
    void save(int issue_number, const JobRecord& job) override;
    std::optional<JobRecord> load(int issue_number) const override;

private:
    std::map<int, JobRecord> jobs_;
};

// One entry per attempted transition (Stage 8). Carries no secrets; ids and enums only.
struct ExecutionRecord {
    int issue_number = 0;
    State from = State::Issue;
    State to = State::Issue;
    EventKind event = EventKind::Start;
    std::string head_sha;
    std::string model_family;
    int attempt = 0;                       // implementation executions counted when the record was made
    bool accepted = false;
    std::string reason;                    // transition() reason text
    EscalationReason escalation = EscalationReason::None;
};

class Orchestrator {
public:
    Orchestrator(int issue_number, IssueFacts facts, RoleExecutor& executor, JobStore& store, Policy policy);

    // Applies Start once, from State::Issue. Returns false if the job already started.
    bool start();

    // Routes one event through transition(). A rejected event is recorded and leaves the job unchanged.
    bool apply(const Event& event, const std::string& model_family = std::string());

    // Runs the role for the current state and applies its event. Returns false when the state has no
    // role (waiting on GitHub, paused, or terminal) or the event was rejected.
    bool advance();

    const JobRecord& job() const;
    const std::vector<ExecutionRecord>& records() const;

private:
    std::optional<Role> role_for(State state) const;
    Event escalation_event(EscalationReason reason) const;

    int issue_number_;
    RoleExecutor& executor_;
    JobStore& store_;
    Policy policy_;
    JobRecord job_;
    std::string implementation_family_;
    std::vector<ExecutionRecord> records_;
};

}  // namespace cipher::orchestration

#endif  // CIPHER_ORCHESTRATION_ORCHESTRATOR_HPP
