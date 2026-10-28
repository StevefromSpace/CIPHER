#include "cipher/model_tool_loop.hpp"

#include <algorithm>
#include <exception>
#include <set>
#include <utility>

namespace cipher {

namespace {

constexpr char kToolRequestMarker[] = "TOOL_CALL:";
constexpr char kToolRequestPrefix[] = "TOOL_CALL: ";

std::string trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

enum class ArgumentMode { None, Required, Optional };

// Only these tools take a model-supplied argument; every other offered tool must be requested bare.
ArgumentMode argument_mode(const std::string& tool_id) {
    if (tool_id == "filesystem.read" || tool_id == "memory.search") return ArgumentMode::Required;
    if (tool_id == "filesystem.list") return ArgumentMode::Optional;
    return ArgumentMode::None;
}

// A tool that ran is not a verified answer: the trace tells the owner what CIPHER actually executed.
std::string with_trace(const std::string& answer, const std::vector<std::string>& ran) {
    if (ran.empty()) return answer;
    std::string trace = "\n\n[CIPHER ran:";
    for (const std::string& call : ran) trace += " " + call + ";";
    return answer + trace + " the answer above is the model's, not verified by CIPHER.]";
}

bool has_control_char(const std::string& text) {
    return std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

// The trimmed reply must be one line: "TOOL_CALL: <id>" or "TOOL_CALL: <id> <argument>". The argument is
// everything after the first space and stays untrusted text; the tool's own validation judges it.
bool parse_tool_request(const std::string& trimmed, std::string& tool_id, std::string& argument) {
    const std::string prefix = kToolRequestPrefix;
    if (trimmed.rfind(prefix, 0) != 0) return false;
    const std::string rest = trimmed.substr(prefix.size());
    if (has_control_char(rest)) return false;
    const std::size_t space = rest.find(' ');
    tool_id = rest.substr(0, space);
    argument = space == std::string::npos ? "" : trim(rest.substr(space + 1));
    return !tool_id.empty() && argument.size() <= ModelToolLoop::kMaxArgumentBytes;
}

} // namespace

std::vector<std::string> model_offered_tool_ids() {
    return {"time.now", "system.info", "filesystem.read", "filesystem.list", "memory.search"};
}

ModelToolLoop::ModelToolLoop(IAssistantBackend& backend,
                             ToolRegistry& registry,
                             ModelToolConfirmation confirm,
                             double timeout_seconds,
                             const std::vector<std::string>& offered_tool_ids)
    : backend_(backend),
      registry_(registry),
      confirm_(std::move(confirm)),
      timeout_seconds_(timeout_seconds) {
    for (const std::string& id : offered_tool_ids) {
        if (registry_.find(id) != nullptr) offered_.push_back(id);
    }
}

std::string ModelToolLoop::build_prompt(const std::string& question, const std::vector<std::string>& results) const {
    std::string prompt =
        "You may request one tool by replying with exactly one line: TOOL_CALL: <tool id>\n"
        "A tool that takes an argument is requested as: TOOL_CALL: <tool id> <argument>\n"
        "You may use several tools one after another, one per reply. Once you have enough, answer in plain text.\n"
        "Available tools:\n";
    for (const std::string& id : offered_) {
        prompt += "- " + id + ": " + registry_.find(id)->description + "\n";
    }
    prompt += "If no tool is needed, answer the question directly.\n";
    for (const std::string& result : results) {
        prompt += "\n" + result + "\n";
    }
    prompt += "\nQuestion: " + question;
    return prompt;
}

std::string ModelToolLoop::run(const std::string& question) {
    // Fail closed: tool output is only ever sent over a verified local route.
    if (backend_.capabilities().processing_route != ProcessingRoute::VerifiedLocal) {
        return "ask stopped: the backend is not a verified local route. Nothing was run.";
    }

    std::vector<std::string> results;
    std::set<std::string> seen_calls;
    std::vector<std::string> ran;
    int failures = 0;
    for (int round = 1; round <= kMaxModelRounds; ++round) {
        AssistantRequest request;
        request.prompt = build_prompt(question, results);
        request.local_only = true;
        request.request_tools = false;
        request.timeout_seconds = timeout_seconds_;

        const AssistantResponse response = AssistantManager::submit(&backend_, request);
        if (!response.is_ok()) {
            return "ask failed (" + to_string(response.status) + "): " + response.error.value_or("no details");
        }

        const std::string reply = trim(response.text);
        if (reply.find(kToolRequestMarker) == std::string::npos) {
            return with_trace(response.text, ran);
        }

        std::string tool_id;
        std::string argument;
        if (!parse_tool_request(reply, tool_id, argument)) {
            return "The model's tool request was malformed. Nothing was run.";
        }
        if (std::find(offered_.begin(), offered_.end(), tool_id) == offered_.end()) {
            return "The model asked for tool '" + tool_id + "', which is not offered. Nothing was run.";
        }
        const ArgumentMode mode = argument_mode(tool_id);
        if ((mode == ArgumentMode::None && !argument.empty()) || (mode == ArgumentMode::Required && argument.empty())) {
            return "The model's tool request was malformed. Nothing was run.";
        }
        if (round == kMaxModelRounds) {
            return "Tool call limit reached. Nothing was run.";
        }

        // The same call twice adds nothing: no second confirmation, no second run.
        const std::string call_key = tool_id + " " + argument;
        if (seen_calls.count(call_key) != 0) {
            results.push_back("Note: '" + call_key + "' was already run and its result is above. Use it or answer.");
            continue;
        }

        // Policy first, so a Block-tier tool is refused without asking the owner.
        const ToolDefinition* definition = registry_.find(tool_id);
        // The tool resolves the untrusted argument first, so the owner confirms the real target, not the model's text.
        std::string target;
        if (definition->resolve_target) {
            try {
                target = definition->resolve_target(argument);
            } catch (const std::exception& error) {
                // Nothing ran, so the model may see the reason and correct itself, a bounded number of times.
                const std::string refusal = std::string("Denied: ") + error.what() + " Nothing was run.";
                if (++failures >= kMaxToolFailures) return refusal;
                results.push_back("Tool " + tool_id + " was not run. " + refusal);
                continue;
            }
        }
        const ActionDescriptor action{tool_id, target, definition->category, definition->default_tier};
        if (registry_.policy().evaluate(action) == RiskTier::Block) {
            return "Denied by permission policy (block): " + tool_id + ". Nothing was run.";
        }

        // Every model-originated call needs an owner "yes", even for Safe-tier tools.
        if (!confirm_ || !confirm_(tool_id, definition->description, target)) {
            return "Owner confirmation was not given: " + tool_id + ". Nothing was run.";
        }

        // invoke() resolves and checks the argument again, so a file swapped after the prompt is still caught.
        const ToolResult result = registry_.invoke(tool_id, "", argument, CallOrigin::Model);
        if (!result.executed) {
            // Offered tools are read-only, so reporting the failure and letting the model try another call is safe.
            if (++failures >= kMaxToolFailures) return result.output;
            results.push_back("Tool " + tool_id + " failed: " + result.output);
            continue;
        }
        if (result.output.size() > kMaxToolOutputBytes) {
            return "Output of '" + tool_id + "' is larger than " + std::to_string(kMaxToolOutputBytes) +
                   " bytes and was not sent to the model.";
        }
        seen_calls.insert(call_key);
        ran.push_back(argument.empty() ? tool_id : tool_id + " " + argument);
        // An empty result is a successful run but not evidence of anything; say so.
        results.push_back(result.output.empty()
                              ? "Tool result for " + tool_id + ": (empty output; this confirms nothing)"
                              : "Tool result for " + tool_id + ":\n" + result.output);
    }
    return "Tool call limit reached. Nothing was run.";
}

} // namespace cipher
