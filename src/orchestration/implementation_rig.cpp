#include "cipher/orchestration/implementation_rig.hpp"

#include "cipher/orchestration/executable_resolver.hpp"
#include "cipher/orchestration/qa_profile.hpp"

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
    gh_runner_ = std::make_unique<GhProcessRunner>(runner_, resolve_executable_on_path("gh"), 60000);
    gh_ = std::make_unique<GhAdapter>(*gh_runner_);

    PipelineSettings pipeline;
    pipeline.issue_number = issue;
    pipeline.git_executable = git;
    pipeline.base_sha = config.base_sha;
    pipeline.evidence_file = settings.evidence_file;
    pipeline.pr_body_file = config.worktree_root / ("pr-body-" + std::to_string(issue) + ".md");
    // QA commands come only from the owner's qa_profiles config, never from the agent or issue text.
    QaTools tools{git, resolve_executable_on_path("cmake"), resolve_executable_on_path("ctest"),
                  resolve_executable_on_path("python")};
    const QaPlan plan = build_qa_plan(config.qa_profiles, worktree, config.worktree_root / ("qa-build-" + std::to_string(issue)), tools);
    pipeline.test_commands = plan.commands;
    pipeline.qa_command_ids = plan.ids;
    ReadOnlySettings readonly;
    readonly.issue_number = issue;
    readonly.task = task;
    readonly.git_executable = git;
    readonly.model_family = "claude";
    readonly.evidence_file = settings.evidence_file;
    readonly_ = std::make_unique<ReadOnlyExecutor>(readonly, *allocator_, runner_, *agent_, escalator_);
    pipeline_ = std::make_unique<PipelineExecutor>(pipeline, *allocator_, runner_, *gh_, *readonly_);
    executor_ = std::make_unique<ImplementationExecutor>(settings, *allocator_, runner_, *agent_, *pipeline_);
}

}  // namespace cipher::orchestration
