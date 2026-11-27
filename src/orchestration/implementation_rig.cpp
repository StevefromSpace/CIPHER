#include "cipher/orchestration/implementation_rig.hpp"

#include "cipher/orchestration/executable_resolver.hpp"
#include "cipher/orchestration/qa_profile.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace cipher::orchestration {

namespace {
constexpr std::size_t kMaxOutputBytes = 1 << 20;
constexpr int kClaudeTimeoutMs = 180000;
constexpr double kLocalTimeoutSeconds = 120.0;
constexpr std::size_t kLocalPromptChars = 3900;
constexpr std::size_t kRetryHistoryChars = 400;  // LocalToolAgent keeps only 800 task chars

// Retry history: the tail of the previous attempt's evidence log, so the second execution sees why the first failed.
std::string with_prior_evidence(const std::string& task, const std::filesystem::path& evidence) {
    std::ifstream in(evidence);
    if (!in) return task;
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string log = buffer.str();
    if (log.empty()) return task;
    if (log.size() > kRetryHistoryChars) log = log.substr(log.size() - kRetryHistoryChars);
    return task.substr(0, 300) + "\nPREVIOUS ATTEMPT:\n" + log;
}
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
    backend_ = std::make_unique<GitWorktreeBackend>(git, config.repository, config.base_sha, config.worktree_root, runner_);
    allocator_ = std::make_unique<WorktreeAllocator>(config.worktree_root, *backend_);
    const bool local = config.agent == "local";
    if (local && !config.local_backend) throw std::runtime_error("orchestration_agent=local has no backend");
    const std::string family = local ? config.local_family : "claude";
    std::string agent_task = task;
    if (local) {
        local_backend_ = config.local_backend;
        agent_ = std::make_unique<LocalToolAgent>(*local_backend_, kLocalTimeoutSeconds, kLocalPromptChars);
        agent_task = with_prior_evidence(task, config.worktree_root / ("evidence-" + std::to_string(issue) + ".log"));
    } else {
        agent_ = std::make_unique<ClaudeToolAgent>(runner_, resolve_executable_on_path("claude"), config.model,
                                                   config.max_budget_usd, kClaudeTimeoutMs);
    }

    ImplementationSettings settings;
    settings.issue_number = issue;
    settings.task = agent_task;
    settings.git_executable = git;
    settings.model_family = family;
    if (local) settings.write_policy = WritePolicy::DocsOnly;
    const std::filesystem::path worktree = allocator_->path_for(issue);
    settings.commands["check"] = {git, "-C", worktree.string(), "diff", "--check"};
    settings.verify_command_id = "check";
    settings.evidence_file = config.worktree_root / ("evidence-" + std::to_string(issue) + ".log");
    gh_runner_ = std::make_unique<GhProcessRunner>(runner_, resolve_executable_on_path("gh"), 60000,
                                                  config.repository);
    gh_ = std::make_unique<GhAdapter>(*gh_runner_);

    PipelineSettings pipeline;
    pipeline.issue_number = issue;
    pipeline.git_executable = git;
    pipeline.base_sha = config.base_sha;
    pipeline.evidence_file = settings.evidence_file;
    pipeline.pr_body_file = config.worktree_root / ("pr-body-" + std::to_string(issue) + ".md");
    // QA commands come only from the owner's qa_profiles config, never from the agent or issue text.
    QaTools tools{git, find_executable_on_path("cmake"), find_executable_on_path("ctest"),
                  find_executable_on_path("python")};
    const QaPlan plan = build_qa_plan(config.qa_profiles, worktree, config.worktree_root / ("qa-build-" + std::to_string(issue)), tools);
    pipeline.test_commands = plan.commands;
    pipeline.qa_command_ids = plan.ids;
    ReadOnlySettings readonly;
    readonly.issue_number = issue;
    readonly.task = task;
    readonly.git_executable = git;
    readonly.model_family = family;
    readonly.evidence_file = settings.evidence_file;
    readonly_ = std::make_unique<ReadOnlyExecutor>(readonly, *allocator_, runner_, *agent_, escalator_);
    pipeline_ = std::make_unique<PipelineExecutor>(pipeline, *allocator_, runner_, *gh_, *readonly_);
    executor_ = std::make_unique<ImplementationExecutor>(settings, *allocator_, runner_, *agent_, *pipeline_);
}

}  // namespace cipher::orchestration
