#include "cipher/orchestration/implementation_rig.hpp"

#include "cipher/orchestration/executable_resolver.hpp"

namespace cipher::orchestration {

namespace {
constexpr std::size_t kMaxOutputBytes = 1 << 20;
constexpr int kClaudeTimeoutMs = 180000;
}  // namespace

RoleResult ImplementationRig::Escalator::run(Role, const JobRecord&) {
    Event event;
    event.kind = EventKind::Escalate;
    event.reason = EscalationReason::RoleNotSupported;
    return {event, std::string()};
}

ImplementationRig::ImplementationRig(const RigConfig& config, int issue, const std::string& task)
    : runner_(kMaxOutputBytes) {
    const std::string git = resolve_executable_on_path("git");
    const std::string claude = resolve_executable_on_path("claude");
    backend_ = std::make_unique<GitWorktreeBackend>(git, config.repository, config.base_sha, config.worktree_root, runner_);
    allocator_ = std::make_unique<WorktreeAllocator>(config.worktree_root, *backend_);
    agent_ = std::make_unique<ClaudeToolAgent>(runner_, claude, config.model, config.max_budget_usd, kClaudeTimeoutMs);

    ImplementationSettings settings;
    settings.issue_number = issue;
    settings.task = task;
    settings.git_executable = git;
    settings.model_family = "claude";
    const std::filesystem::path worktree = allocator_->path_for(issue);
    settings.commands["check"] = {git, "-C", worktree.string(), "diff", "--check"};
    settings.verify_command_id = "check";
    settings.evidence_file = config.worktree_root / ("evidence-" + std::to_string(issue) + ".log");
    executor_ = std::make_unique<ImplementationExecutor>(settings, *allocator_, runner_, *agent_, escalator_);
}

}  // namespace cipher::orchestration
