#include "cipher/orchestration/implementation_executor.hpp"

#include "cipher/orchestration/claude_executor.hpp"
#include "cipher/orchestration/commit_sha.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

namespace cipher::orchestration {

namespace {

constexpr std::size_t kMaxPromptChars = 3900;  // below the Windows command line limit used by the role prompts
constexpr int kGitTimeoutMs = 60000;

const char* const kProtocol =
    "You are the implementation agent for a git worktree. Reply with ONE JSON object and nothing else.\n"
    "Tool call: {\"tool\":NAME,\"path\":P,\"content\":C,\"old_text\":A,\"new_text\":B,\"pattern\":S}"
    " using only the fields a tool needs.\n"
    "Tools: fs.read(path) fs.list(path) fs.search(path,pattern) fs.write(path,content) "
    "fs.edit(path,old_text,new_text) process.run(pattern=command id) git.status git.diff git.log.\n"
    "When the task is complete reply {\"done\":true,\"summary\":\"...\"}. Paths are relative to the worktree.\n";

std::string field(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end()) return {};
    if (!it->is_string()) throw std::runtime_error(std::string("field '") + key + "' must be a string");
    return it->get<std::string>();
}

std::string render_transcript(const std::vector<ToolTurn>& transcript, std::size_t budget) {
    std::string out;
    for (auto turn = transcript.rbegin(); turn != transcript.rend(); ++turn) {
        std::string entry = "> " + turn->call.tool + " " + turn->call.path + turn->call.pattern + "\n" +
                            turn->outcome.output.substr(0, 600) + "\n";
        if (out.size() + entry.size() > budget) break;
        out = entry + out;
    }
    return out;
}

}  // namespace

AgentStep parse_agent_reply(const std::string& text) {
    const auto open = text.find('{');
    const auto close = text.rfind('}');
    if (open == std::string::npos || close == std::string::npos || close < open)
        throw std::runtime_error("agent reply has no JSON object");
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(text.substr(open, close - open + 1));
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("agent reply is not valid JSON");
    }
    if (!json.is_object()) throw std::runtime_error("agent reply is not an object");

    AgentStep step;
    const auto done = json.find("done");
    if (done != json.end()) {
        if (!done->is_boolean()) throw std::runtime_error("'done' must be a boolean");
        if (done->get<bool>()) {
            step.done = true;
            step.summary = field(json, "summary").substr(0, 500);
            return step;
        }
    }
    step.call.tool = field(json, "tool");
    if (step.call.tool.empty()) throw std::runtime_error("agent reply names no tool");
    step.call.path = field(json, "path");
    step.call.content = field(json, "content");
    step.call.old_text = field(json, "old_text");
    step.call.new_text = field(json, "new_text");
    step.call.pattern = field(json, "pattern");
    return step;
}

ClaudeToolAgent::ClaudeToolAgent(ProcessRunner& runner, std::string claude_executable, std::string model,
                                 std::string max_budget_usd, int timeout_ms)
    : runner_(runner),
      claude_executable_(std::move(claude_executable)),
      model_(std::move(model)),
      max_budget_usd_(std::move(max_budget_usd)),
      timeout_ms_(timeout_ms) {}

AgentStep ClaudeToolAgent::next(const std::string& task, const std::vector<ToolTurn>& transcript) {
    const std::string head = std::string(kProtocol) + "TASK: " + task.substr(0, 800) + "\nHISTORY:\n";
    const std::string prompt = head + render_transcript(transcript, kMaxPromptChars - head.size() - 40) + "NEXT:";
    const ClaudeResult result = run_claude(runner_, claude_executable_, model_, max_budget_usd_, prompt, timeout_ms_);
    if (result.status != ClaudeStatus::Ok) throw std::runtime_error(std::string("claude: ") + to_string(result.status));
    return parse_agent_reply(result.text);
}

ImplementationExecutor::ImplementationExecutor(ImplementationSettings settings, WorktreeAllocator& allocator,
                                               ProcessRunner& runner, ToolAgent& agent, RoleExecutor& fallback)
    : settings_(std::move(settings)), allocator_(allocator), runner_(runner), agent_(agent), fallback_(fallback) {}

RoleResult ImplementationExecutor::run(Role role, const JobRecord& job) {
    // PullRequest needs a GitHub write that is not implemented; it must keep escalating instead of pretending.
    if (role != Role::Implementation || job.state != State::Implementation) return fallback_.run(role, job);
    return execute_implementation();
}

RoleResult ImplementationExecutor::execute_implementation() {
    const std::string family = settings_.model_family;
    auto failed = [&](const std::string& why, const WorktreeTools* tools) {
        if (!settings_.evidence_file.empty()) {
            std::ofstream out(settings_.evidence_file, std::ios::app);
            out << "FAILED: " << why << "\n";
            if (tools != nullptr) for (const auto& line : tools->audit()) out << "  " << line << "\n";
        }
        return RoleResult{Event{EventKind::ImplementationFailed}, family};
    };

    if (allocator_.allocate(settings_.issue_number) == WorktreeOutcome::Conflict)
        return {Event{EventKind::Blocked}, family};
    const std::filesystem::path root = allocator_.path_for(settings_.issue_number);
    WorktreeTools tools(root, runner_, settings_.git_executable, settings_.commands, default_worktree_policy());

    std::vector<ToolTurn> transcript;
    std::string summary;
    bool finished = false;
    try {
        for (int i = 0; i < settings_.max_steps && !finished; ++i) {
            const AgentStep step = agent_.next(settings_.task, transcript);
            if (step.done) {
                finished = true;
                summary = step.summary;
            } else {
                transcript.push_back({step.call, tools.call(step.call)});
            }
        }
    } catch (const std::exception& error) {
        return failed(std::string("agent error: ") + error.what(), &tools);
    }
    if (!finished) return failed("step limit reached", &tools);

    if (!settings_.verify_command_id.empty()) {
        ToolCall verify;
        verify.tool = "process.run";
        verify.pattern = settings_.verify_command_id;
        if (!tools.call(verify).ok) return failed("verification command failed", &tools);
    }

    auto git = [&](std::vector<std::string> args) {
        std::vector<std::string> argv = {settings_.git_executable, "-C", root.string(), "--no-pager"};
        argv.insert(argv.end(), args.begin(), args.end());
        return runner_.run(argv, kGitTimeoutMs);
    };
    try {
        const ProcessResult status = git({"status", "--porcelain"});
        if (status.exit_code != 0) return failed("git status failed", &tools);
        if (status.stdout_text.empty()) return failed("agent made no changes", &tools);
        if (git({"add", "-A"}).exit_code != 0) return failed("git add failed", &tools);
        // --no-verify/no signing: a repository hook or signing prompt must not run on behalf of the model.
        if (git({"-c", "user.name=CIPHER", "-c", "user.email=cipher@localhost", "-c", "commit.gpgsign=false",
                 "commit", "--no-verify", "-q", "-m", "cipher: implementation for issue " +
                 std::to_string(settings_.issue_number)}).exit_code != 0)
            return failed("git commit failed", &tools);
        ProcessResult head = git({"rev-parse", "HEAD"});
        std::string sha = head.stdout_text;
        while (!sha.empty() && (sha.back() == '\n' || sha.back() == '\r')) sha.pop_back();
        if (head.exit_code != 0 || !is_full_sha(sha)) return failed("could not read HEAD", &tools);

        if (!settings_.evidence_file.empty()) {
            std::ofstream out(settings_.evidence_file, std::ios::app);
            out << "COMPLETE head=" << sha << " summary=" << summary << "\n";
            for (const auto& line : tools.audit()) out << "  " << line << "\n";
        }
        Event event;
        event.kind = EventKind::ImplementationComplete;
        event.head_sha = sha;
        return {event, family};
    } catch (const std::exception& error) {
        return failed(std::string("git error: ") + error.what(), &tools);
    }
}

}  // namespace cipher::orchestration
