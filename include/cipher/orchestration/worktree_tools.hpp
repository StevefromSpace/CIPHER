#ifndef CIPHER_ORCHESTRATION_WORKTREE_TOOLS_HPP
#define CIPHER_ORCHESTRATION_WORKTREE_TOOLS_HPP

#include "cipher/assistant_backend.hpp"
#include "cipher/orchestration/process_runner.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace cipher::orchestration {

// One model-proposed tool request. Only the fields a tool names are read; the rest are ignored.
// Tools: fs.read fs.list fs.search fs.write fs.edit process.run git.status git.diff git.log
struct ToolCall {
    std::string tool;
    std::string path;       // worktree-relative
    std::string content;    // fs.write
    std::string old_text;   // fs.edit (must occur exactly once)
    std::string new_text;   // fs.edit
    std::string pattern;    // fs.search literal text; process.run command id
};

struct ToolOutcome {
    bool ok = false;
    std::string output;     // result text, or "error: <reason>" when !ok
};

struct ToolLimits {
    std::size_t max_read_bytes = 64 * 1024;
    std::size_t max_write_bytes = 256 * 1024;
    std::size_t max_output_chars = 16 * 1024;
    std::size_t max_list_entries = 200;
    std::size_t max_search_hits = 50;
    int process_timeout_ms = 120000;
};

// Policy for model-driven worktree work: reads/listing/search/git-inspection and writes are allowed; writes to .git,
// .github, tools/pipeline and the governance contract are blocked; everything unlisted falls to Ask, which has no
// owner here and is therefore denied.
PermissionPolicy default_worktree_policy();

// Executes tool calls inside one worktree. The model proposes; this class validates, asks the PermissionPolicy,
// runs, bounds the output and appends one audit line per call. It holds no shell: process.run only launches a
// pre-registered argv chosen by id, so the model supplies no executable or arguments.
class WorktreeTools {
public:
    WorktreeTools(std::filesystem::path root, ProcessRunner& runner, std::string git_executable,
                  std::map<std::string, std::vector<std::string>> commands, PermissionPolicy policy,
                  ToolLimits limits = {});

    ToolOutcome call(const ToolCall& request);
    const std::vector<std::string>& audit() const { return audit_; }

private:
    ToolOutcome dispatch(const ToolCall& request);
    ToolOutcome run_git(const std::vector<std::string>& args);
    std::string bound(std::string text) const;

    std::filesystem::path root_;     // canonical
    ProcessRunner& runner_;
    std::string git_;
    std::map<std::string, std::vector<std::string>> commands_;
    PermissionPolicy policy_;
    ToolLimits limits_;
    std::vector<std::string> audit_;
};

}  // namespace cipher::orchestration

#endif  // CIPHER_ORCHESTRATION_WORKTREE_TOOLS_HPP
