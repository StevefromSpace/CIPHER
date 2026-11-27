#include "cipher/orchestration/local_tool_agent.hpp"

#include <stdexcept>

namespace cipher::orchestration {

namespace {

constexpr std::size_t kMaxTaskChars = 800;
constexpr std::size_t kMaxOutputChars = 600;

// Short protocol plus one worked example: small local models drift more than Claude.
const char* const kLocalProtocol =
    "You edit files in a git worktree. Reply with ONE JSON object and nothing else.\n"
    "Tools: fs.read(path) fs.list(path) fs.search(path,pattern) fs.edit(path,old_text,new_text) "
    "fs.write(path,content) process.run(pattern=command id) git.status git.diff git.log.\n"
    "Prefer fs.edit; old_text must occur exactly once. One tool call per reply. Paths are relative.\n"
    "Example: {\"tool\":\"fs.edit\",\"path\":\"docs/a.md\",\"old_text\":\"old\",\"new_text\":\"new\"}\n"
    "When finished reply {\"done\":true,\"summary\":\"...\"}.\n";

bool same_call(const ToolCall& a, const ToolCall& b) {
    return a.tool == b.tool && a.path == b.path && a.content == b.content && a.old_text == b.old_text &&
           a.new_text == b.new_text && a.pattern == b.pattern;
}

std::string render_history(const std::vector<ToolTurn>& transcript, std::size_t budget) {
    std::string out;
    for (auto turn = transcript.rbegin(); turn != transcript.rend(); ++turn) {
        std::string entry = "> " + turn->call.tool + " " + turn->call.path + turn->call.pattern + "\n" +
                            turn->outcome.output.substr(0, kMaxOutputChars) + "\n";
        if (out.size() + entry.size() > budget) break;
        out = entry + out;
    }
    return out;
}

}  // namespace

LocalToolAgent::LocalToolAgent(IAssistantBackend& backend, double timeout_seconds, std::size_t max_prompt_chars)
    : backend_(backend), timeout_seconds_(timeout_seconds), max_prompt_chars_(max_prompt_chars) {}

AgentStep LocalToolAgent::next(const std::string& task, const std::vector<ToolTurn>& transcript) {
    const std::string head = std::string(kLocalProtocol) + "TASK: " + task.substr(0, kMaxTaskChars) + "\nHISTORY:\n";
    const std::string tail = "NEXT:";
    if (head.size() + tail.size() > max_prompt_chars_) throw std::runtime_error("local agent: prompt budget too small");

    AssistantRequest request;
    request.prompt = head + render_history(transcript, max_prompt_chars_ - head.size() - tail.size()) + tail;
    request.timeout_seconds = timeout_seconds_;
    request.local_only = true;
    request.budget.max_prompt_bytes = max_prompt_chars_;

    const AssistantResponse response = AssistantManager::submit(&backend_, request);
    if (!response.is_ok())
        throw std::runtime_error("local agent: " + to_string(response.status) +
                                 (response.error ? ": " + *response.error : std::string()));

    AgentStep step = parse_agent_reply(response.text);
    // Repeat guard: the same call twice in a row counts as a failure (ModelToolLoop's safety shape).
    if (!step.done && !transcript.empty() && same_call(step.call, transcript.back().call))
        throw std::runtime_error("local agent: repeated tool call");
    return step;
}

}  // namespace cipher::orchestration
