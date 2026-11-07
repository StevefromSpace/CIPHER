#include "cipher/orchestration/claude_role_executor.hpp"

#include <stdexcept>
#include <utility>

namespace cipher::orchestration {

namespace {

constexpr const char* kClaudeFamily = "claude";

const char* role_name(Role role) {
    switch (role) {
        case Role::Research: return "research";
        case Role::Architecture: return "architecture";
        case Role::Implementation: return "implementation";
        case Role::Qa: return "qa";
        case Role::Security: return "security";
        case Role::Reviewer: return "reviewer";
    }
    return "unknown";
}

}  // namespace

std::string build_role_prompt(Role role, int issue_number) {
    // The model sees no repository. Its text is advisory only and never becomes an event's authority.
    const std::string prompt = std::string("You are the ") + role_name(role) + " role for CIPHER issue #" +
                               std::to_string(issue_number) +
                               ". You have no tools and cannot read files. Reply in at most five plain-text lines.";
    if (prompt.size() > kMaxRolePromptChars) throw std::length_error("role prompt exceeds the bounded length");
    return prompt;
}

ClaudeRoleExecutor::ClaudeRoleExecutor(ProcessRunner& runner, int issue_number, std::string claude_executable, std::string model,
                                       std::string max_budget_usd, int timeout_ms)
    : runner_(runner),
      issue_number_(issue_number),
      claude_executable_(std::move(claude_executable)),
      model_(std::move(model)),
      max_budget_usd_(std::move(max_budget_usd)),
      timeout_ms_(timeout_ms) {}

RoleResult ClaudeRoleExecutor::run(Role role, const JobRecord& /*job*/) {
    // Escalate before any process call for roles this tool-less executor cannot perform safely.
    if (role == Role::Security) {
        return {Event{EventKind::Escalate, {}, Verdict::None, Verdict::None, false, {},
                      EscalationReason::SecurityReviewUnavailable},
                std::string()};
    }
    if (role != Role::Research && role != Role::Architecture) {
        return {Event{EventKind::Escalate, {}, Verdict::None, Verdict::None, false, {},
                      EscalationReason::RoleNotSupported},
                std::string()};
    }

    ClaudeResult result;
    try {
        result = run_claude(runner_, claude_executable_, model_, max_budget_usd_,
                            build_role_prompt(role, issue_number_), timeout_ms_);
    } catch (const std::invalid_argument&) {
        return {Event{EventKind::Blocked}, kClaudeFamily};   // bad model/budget/prompt: nothing was launched
    }
    // A failed call is a tooling failure (BLOCKED), never an implementation attempt. The text is not stored.
    if (result.status != ClaudeStatus::Ok) {
        return {Event{EventKind::Blocked}, kClaudeFamily};
    }
    const EventKind done = role == Role::Research ? EventKind::ResearchComplete : EventKind::ArchitectureComplete;
    return {Event{done}, kClaudeFamily};
}

}  // namespace cipher::orchestration
