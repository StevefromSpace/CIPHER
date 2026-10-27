#include "cipher/model_tool_loop.hpp"

#include <algorithm>
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

// The trimmed reply must be exactly "TOOL_CALL: <id>". Extra words or lines make it malformed.
bool parse_tool_request(const std::string& trimmed, std::string& tool_id) {
    const std::string prefix = kToolRequestPrefix;
    if (trimmed.rfind(prefix, 0) != 0) return false;
    tool_id = trimmed.substr(prefix.size());
    return !tool_id.empty() && tool_id.find_first_of(" \t\r\n") == std::string::npos;
}

} // namespace

std::vector<std::string> model_offered_tool_ids() {
    return {"time.now", "system.info"};
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
            return response.text;
        }

        std::string tool_id;
        if (!parse_tool_request(reply, tool_id)) {
            return "The model's tool request was malformed. Nothing was run.";
        }
        if (std::find(offered_.begin(), offered_.end(), tool_id) == offered_.end()) {
            return "The model asked for tool '" + tool_id + "', which is not offered. Nothing was run.";
        }
        if (round == kMaxModelRounds) {
            return "Tool call limit reached. Nothing was run.";
        }

        // Policy first, so a Block-tier tool is refused without asking the owner.
        const ToolDefinition* definition = registry_.find(tool_id);
        const ActionDescriptor action{tool_id, "", definition->category, definition->default_tier};
        if (registry_.policy().evaluate(action) == RiskTier::Block) {
            return "Denied by permission policy (block): " + tool_id + ". Nothing was run.";
        }

        // Every model-originated call needs an owner "yes", even for Safe-tier tools.
        if (!confirm_ || !confirm_(tool_id, definition->description)) {
            return "Owner confirmation was not given: " + tool_id + ". Nothing was run.";
        }

        const ToolResult result = registry_.invoke(tool_id, "", "");
        if (!result.executed) {
            return result.output;
        }
        if (result.output.size() > kMaxToolOutputBytes) {
            return "Output of '" + tool_id + "' is larger than " + std::to_string(kMaxToolOutputBytes) +
                   " bytes and was not sent to the model.";
        }
        results.push_back("Tool result for " + tool_id + ":\n" + result.output);
    }
    return "Tool call limit reached. Nothing was run.";
}

} // namespace cipher
