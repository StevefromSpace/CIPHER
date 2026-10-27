#include "cipher/ask_commands.hpp"
#include "cipher/model_tool_loop.hpp"

namespace cipher {

namespace {

std::string trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return "";
    const std::size_t last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

} // namespace

AskCommands::AskCommands(IAssistantBackend* backend, double timeout_seconds, ModelToolLoop* tool_loop)
    : backend_(backend), timeout_seconds_(timeout_seconds), tool_loop_(tool_loop) {}

AskCommandResult AskCommands::execute(const std::string& input) {
    const std::string line = trim(input);
    if (line != "ask" && line.rfind("ask ", 0) != 0) {
        return {};
    }

    AskCommandResult result;
    result.handled = true;

    if (backend_ == nullptr) {
        result.text = "ask is disabled. Set ollama_port in cipher.conf to a local Ollama port.";
        return result;
    }

    const std::string prompt = trim(line.substr(3));
    if (prompt.empty()) {
        result.text = "Usage: ask <question>";
        return result;
    }

    if (tool_loop_ != nullptr) {
        result.text = tool_loop_->run(prompt);
        return result;
    }

    AssistantRequest request;
    request.prompt = prompt;
    request.local_only = true;
    request.timeout_seconds = timeout_seconds_;

    const AssistantResponse response = AssistantManager::submit(backend_, request);
    if (response.is_ok()) {
        result.text = response.text;
    } else {
        result.text = "ask failed (" + to_string(response.status) + "): " +
                      response.error.value_or("no details");
    }
    return result;
}

} // namespace cipher
