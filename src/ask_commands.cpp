#include "cipher/ask_commands.hpp"
#include "cipher/memory_commands.hpp"
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

AskCommands::AskCommands(IAssistantBackend* backend, double timeout_seconds, ModelToolLoop* tool_loop,
                         const MemoryCommands* memory)
    : backend_(backend), timeout_seconds_(timeout_seconds), tool_loop_(tool_loop), memory_(memory) {}

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

    if (prompt == "--memory" || prompt.rfind("--memory ", 0) == 0 || prompt.rfind("--memory\t", 0) == 0) {
        result.text = ask_with_memory(trim(prompt.substr(8)), result.shared_memory_id);
        return result;
    }

    if (tool_loop_ != nullptr) {
        result.text = tool_loop_->run(prompt);
        return result;
    }
    result.text = submit(prompt);
    return result;
}

std::string AskCommands::ask_with_memory(const std::string& arguments, std::string& shared_id) {
    const std::size_t split = arguments.find_first_of(" \t");
    const std::string id = arguments.substr(0, split);
    const std::string question = split == std::string::npos ? "" : trim(arguments.substr(split + 1));
    if (id.empty() || question.empty()) {
        return "Usage: ask --memory <id> <question>";
    }
    if (memory_ == nullptr) {
        return "ask --memory needs memory_file in cipher.conf.";
    }
    std::string content;
    if (!memory_->share(id, content)) {
        return content;
    }
    shared_id = id;
    // No tool loop: shared memory never goes to a model that can propose tool calls.
    return submit("The owner shared this saved note, id '" + id + "':\n" + content + "\n\nQuestion: " + question,
                  "Shared memory '" + id + "' with the local model.\n");
}

std::string AskCommands::submit(const std::string& prompt, const std::string& ok_prefix) {
    AssistantRequest request;
    request.prompt = prompt;
    request.local_only = true;
    request.timeout_seconds = timeout_seconds_;

    const AssistantResponse response = AssistantManager::submit(backend_, request);
    if (response.is_ok()) {
        return ok_prefix + response.text;
    }
    return "ask failed (" + to_string(response.status) + "): " + response.error.value_or("no details");
}

} // namespace cipher
