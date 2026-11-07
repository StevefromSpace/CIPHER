#include "cipher/orchestration/readonly_executor.hpp"

#include <fstream>
#include <set>

namespace cipher::orchestration {

namespace {
const std::set<std::string> kReadTools = {"fs.read", "fs.list", "fs.search", "git.status", "git.diff", "git.log"};
}

ReadOnlyExecutor::ReadOnlyExecutor(ReadOnlySettings settings, WorktreeAllocator& allocator, ProcessRunner& runner,
                                   ToolAgent& agent, RoleExecutor& fallback)
    : settings_(std::move(settings)), allocator_(allocator), runner_(runner), agent_(agent), fallback_(fallback) {}

RoleResult ReadOnlyExecutor::run(Role role, const JobRecord& job) {
    const bool research = role == Role::Research && job.state == State::Research;
    const bool design = role == Role::Architecture && job.state == State::Architecture;
    if (!research && !design) return fallback_.run(role, job);

    const std::string family = settings_.model_family;
    auto note = [&](const std::string& text) {
        if (settings_.evidence_file.empty()) return;
        std::ofstream out(settings_.evidence_file, std::ios::app);
        out << (research ? "RESEARCH " : "ARCHITECTURE ") << text << "\n";
    };
    const RoleResult blocked{Event{EventKind::Blocked}, family};

    if (allocator_.allocate(settings_.issue_number) == WorktreeOutcome::Conflict) return blocked;
    // Empty command map: process.run has nothing to run even if the executor check were bypassed.
    WorktreeTools tools(allocator_.path_for(settings_.issue_number), runner_, settings_.git_executable, {},
                        default_worktree_policy());

    std::vector<ToolTurn> transcript;
    try {
        for (int i = 0; i < settings_.max_steps; ++i) {
            const AgentStep step = agent_.next(settings_.task, transcript);
            if (step.done) {
                note("COMPLETE summary=" + step.summary);
                return {Event{research ? EventKind::ResearchComplete : EventKind::ArchitectureComplete}, family};
            }
            if (kReadTools.count(step.call.tool) == 0) {
                note("FAILED: tool '" + step.call.tool + "' is not allowed for a read-only role");
                return blocked;
            }
            transcript.push_back({step.call, tools.call(step.call)});
        }
    } catch (const std::exception& error) {
        note(std::string("FAILED: agent error: ") + error.what());
        return blocked;
    }
    note("FAILED: step limit reached");
    return blocked;
}

}  // namespace cipher::orchestration
