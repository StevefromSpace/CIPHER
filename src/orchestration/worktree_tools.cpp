#include "cipher/orchestration/worktree_tools.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace cipher::orchestration {

namespace fs = std::filesystem;

namespace {

constexpr std::uintmax_t kMaxSearchFileBytes = 256 * 1024;
constexpr std::size_t kMaxPatternChars = 200;

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

// Resolves a worktree-relative path to a canonical absolute path inside root, or throws. Rejects absolute paths,
// drive/UNC names, "..", NUL and any ".git" component, then re-checks containment after symlink resolution.
fs::path resolve_inside(const fs::path& root, const std::string& relative, std::string& normalized) {
    if (relative.find('\0') != std::string::npos) throw std::runtime_error("path contains NUL");
    const fs::path rel(relative);
    if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory() || relative.find(':') != std::string::npos)
        throw std::runtime_error("path must be relative to the worktree");
    for (const auto& part : rel) {
        if (part == "..") throw std::runtime_error("path escapes the worktree");
        if (lower(part.string()) == ".git") throw std::runtime_error("path is inside .git");
    }
    const fs::path full = fs::weakly_canonical(root / rel);
    const auto mismatch = std::mismatch(root.begin(), root.end(), full.begin(), full.end());
    if (mismatch.first != root.end()) throw std::runtime_error("path escapes the worktree");
    normalized = lower(full.lexically_relative(root).generic_string());
    if (normalized == ".") normalized.clear();
    for (const auto& part : full.lexically_relative(root))
        if (lower(part.string()) == ".git") throw std::runtime_error("path is inside .git");
    return full;
}

std::string read_file(const fs::path& path, std::size_t limit, bool& truncated) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open file");
    std::string text(limit + 1, '\0');
    in.read(text.data(), static_cast<std::streamsize>(limit + 1));
    text.resize(static_cast<std::size_t>(in.gcount()));
    truncated = text.size() > limit;
    if (truncated) text.resize(limit);
    return text;
}

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    if (!out) throw std::runtime_error("write failed");
}

ToolOutcome fail(const std::string& reason) { return {false, "error: " + reason}; }

}  // namespace

PermissionPolicy default_worktree_policy() {
    PermissionPolicy policy;
    policy.set_default_tier(RiskTier::Ask);
    for (const char* tool : {"fs.write", "fs.edit"})
        for (const char* protected_prefix : {".git", ".github", "tools/pipeline", "docs/agent_role_contract.md"})
            policy.add_blacklist_pattern(std::string(tool) + " " + protected_prefix + "*");
    for (const char* pattern : {"fs.read*", "fs.list*", "fs.search*", "fs.write*", "fs.edit*", "process.run*",
                                "git.status*", "git.diff*", "git.log*"})
        policy.add_whitelist_pattern(pattern);
    return policy;
}

WorktreeTools::WorktreeTools(fs::path root, ProcessRunner& runner, std::string git_executable,
                             std::map<std::string, std::vector<std::string>> commands, PermissionPolicy policy,
                             ToolLimits limits)
    : root_(fs::weakly_canonical(root)),
      runner_(runner),
      git_(std::move(git_executable)),
      commands_(std::move(commands)),
      policy_(std::move(policy)),
      limits_(limits) {}

std::string WorktreeTools::bound(std::string text) const {
    if (text.size() > limits_.max_output_chars) {
        text.resize(limits_.max_output_chars);
        text += "\n[output truncated]";
    }
    return text;
}

ToolOutcome WorktreeTools::call(const ToolCall& request) {
    ToolOutcome outcome;
    try {
        outcome = dispatch(request);
    } catch (const std::exception& error) {
        outcome = fail(error.what());
    }
    audit_.push_back(request.tool + " " + (request.path.empty() ? request.pattern : request.path) + " -> " +
                     (outcome.ok ? "ok" : outcome.output.substr(0, 120)));
    return outcome;
}

ToolOutcome WorktreeTools::dispatch(const ToolCall& request) {
    const std::string& tool = request.tool;
    static const char* const known[] = {"fs.read", "fs.list", "fs.search", "fs.write", "fs.edit",
                                        "process.run", "git.status", "git.diff", "git.log"};
    if (std::find(std::begin(known), std::end(known), tool) == std::end(known)) return fail("unknown tool");

    std::string normalized;
    fs::path full;
    const bool takes_path = tool.rfind("fs.", 0) == 0 || (tool == "git.diff" && !request.path.empty());
    if (takes_path) full = resolve_inside(root_, request.path, normalized);

    ActionDescriptor action;
    action.name = tool;
    action.target = tool == "process.run" ? request.pattern : normalized;
    action.category = tool.substr(0, tool.find('.'));
    const RiskTier tier = policy_.evaluate(action);
    if (tier != RiskTier::Safe && tier != RiskTier::Monitor) return fail("denied by permission policy");

    if (tool == "fs.read") {
        if (!fs::is_regular_file(full)) return fail("not a regular file");
        bool truncated = false;
        std::string text = read_file(full, limits_.max_read_bytes, truncated);
        return {true, truncated ? text + "\n[file truncated]" : text};
    }
    if (tool == "fs.list") {
        if (!fs::is_directory(full)) return fail("not a directory");
        std::vector<std::string> names;
        for (const auto& entry : fs::directory_iterator(full)) {
            const std::string name = entry.path().filename().string();
            if (lower(name) == ".git") continue;
            names.push_back(name + (entry.is_directory() ? "/" : ""));
        }
        std::sort(names.begin(), names.end());
        const bool more = names.size() > limits_.max_list_entries;
        if (more) names.resize(limits_.max_list_entries);
        std::string out;
        for (const auto& name : names) out += name + "\n";
        return {true, out + (more ? "[list truncated]\n" : "")};
    }
    if (tool == "fs.search") {
        if (request.pattern.empty() || request.pattern.size() > kMaxPatternChars) return fail("bad search pattern");
        if (!fs::is_directory(full)) return fail("not a directory");
        std::string out;
        std::size_t hits = 0;
        fs::recursive_directory_iterator it(full, fs::directory_options::skip_permission_denied), end;
        for (; it != end && hits < limits_.max_search_hits; ++it) {
            if (it->is_directory() && lower(it->path().filename().string()) == ".git") {
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_symlink() || !it->is_regular_file() || it->file_size() > kMaxSearchFileBytes) continue;
            std::ifstream in(it->path(), std::ios::binary);
            std::string line;
            for (int number = 1; std::getline(in, line) && hits < limits_.max_search_hits; ++number) {
                if (line.find(request.pattern) == std::string::npos) continue;
                out += it->path().lexically_relative(root_).generic_string() + ":" + std::to_string(number) + ": " +
                       line.substr(0, 200) + "\n";
                ++hits;
            }
        }
        return {true, out.empty() ? "no matches\n" : out};
    }
    if (tool == "fs.write") {
        if (request.content.size() > limits_.max_write_bytes) return fail("content exceeds write limit");
        if (normalized.empty() || fs::is_directory(full)) return fail("not a writable file path");
        write_file(full, request.content);
        return {true, "wrote " + std::to_string(request.content.size()) + " bytes"};
    }
    if (tool == "fs.edit") {
        if (request.old_text.empty()) return fail("old_text is empty");
        if (!fs::is_regular_file(full)) return fail("not a regular file");
        bool truncated = false;
        std::string text = read_file(full, limits_.max_write_bytes, truncated);
        if (truncated) return fail("file too large to edit");
        const auto first = text.find(request.old_text);
        if (first == std::string::npos) return fail("old_text not found");
        if (text.find(request.old_text, first + 1) != std::string::npos) return fail("old_text is ambiguous");
        text.replace(first, request.old_text.size(), request.new_text);
        if (text.size() > limits_.max_write_bytes) return fail("result exceeds write limit");
        write_file(full, text);
        return {true, "edited"};
    }
    if (tool == "process.run") {
        const auto command = commands_.find(request.pattern);
        if (command == commands_.end()) return fail("unknown command id");
        const ProcessResult result = runner_.run(command->second, limits_.process_timeout_ms);
        const bool ok = result.exit_code == 0 && !result.timed_out && !result.output_overflow;
        std::string text = "exit=" + std::to_string(result.exit_code);
        if (result.timed_out) text += " timed_out";
        if (result.output_overflow) text += " output_overflow";
        text = bound(text + "\n" + result.stdout_text + result.stderr_text);
        return {ok, ok ? text : "error: command failed\n" + text};
    }
    if (tool == "git.status") return run_git({"status", "--porcelain"});
    if (tool == "git.log") return run_git({"log", "-n", "10", "--oneline", "--no-color"});
    std::vector<std::string> diff = {"diff", "--no-color", "HEAD"};  // git.diff
    if (!request.path.empty()) {
        diff.push_back("--");
        diff.push_back(full.lexically_relative(root_).generic_string());
    }
    return run_git(diff);
}

ToolOutcome WorktreeTools::run_git(const std::vector<std::string>& args) {
    std::vector<std::string> argv = {git_, "-C", root_.string(), "--no-pager"};
    argv.insert(argv.end(), args.begin(), args.end());
    const ProcessResult result = runner_.run(argv, limits_.process_timeout_ms);
    if (result.exit_code != 0 || result.timed_out || result.output_overflow)
        return fail("git failed: " + bound(result.stderr_text));
    return {true, bound(result.stdout_text)};
}

}  // namespace cipher::orchestration
