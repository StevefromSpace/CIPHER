#include "cipher/assistant_commands.hpp"

#include <algorithm>

namespace cipher {

namespace {

constexpr double CLI_REQUEST_TIMEOUT_SECONDS = 120.0;

const char* const ASK_USAGE = "Usage: ask <claude|hermes> <prompt>. No request was sent.";
const char* const AI_USAGE = "Usage: ai list";
const char* const HELP_ADDENDUM =
    "Assistant (optional, text-only): ask claude <prompt>, ask hermes <prompt>, ai list";

const char* const WHITESPACE = " \t\r\n";

std::string trim_whitespace(const std::string& text) {
    const auto first = text.find_first_not_of(WHITESPACE);
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(WHITESPACE);
    return text.substr(first, last - first + 1);
}

// Splits "word rest" into its first word and the trimmed remainder.
void split_first_word(const std::string& text, std::string& word, std::string& rest) {
    const auto end = text.find_first_of(WHITESPACE);
    word = text.substr(0, end);
    rest = end == std::string::npos ? std::string{} : trim_whitespace(text.substr(end));
}

} // anonymous namespace

AssistantCommands::AssistantCommands(IProcessRunner& runner, const std::string& claude_executable)
    : claude_(claude_executable, runner) {}

AssistantCommandResult AssistantCommands::execute(const std::string& input) {
    const std::string trimmed = trim_whitespace(input);
    std::string command;
    std::string arguments;
    split_first_word(trimmed, command, arguments);

    if (command == "ask") return {true, ask(arguments)};
    if (command == "ai") {
        return {true, arguments == "list" ? list_providers() : AI_USAGE};
    }
    if (command == "help") return {false, HELP_ADDENDUM};
    return {};
}

std::string AssistantCommands::ask(const std::string& arguments) {
    std::string provider;
    std::string prompt;
    split_first_word(arguments, provider, prompt);

    if (provider.empty()) return ASK_USAGE;
    if (provider == "native") {
        return "The CIPHER native runtime is not implemented. No request was sent.";
    }
    if (provider == "claude") return submit(claude_, prompt);
    if (provider == "hermes") return submit(hermes_, prompt);
    return "Unknown assistant provider '" + provider + "'. Available: claude, hermes. No request was sent.";
}

std::string AssistantCommands::submit(IAssistantBackend& backend, const std::string& prompt) {
    if (prompt.empty()) return ASK_USAGE;

    AssistantRequest request;
    request.prompt = prompt;
    request.timeout_seconds = CLI_REQUEST_TIMEOUT_SECONDS;
    // Explicit consent for an external route. The validator still rejects
    // local-only requests unless the backend has a verified local route.
    request.local_only = false;

    const AssistantResponse response = AssistantManager::submit(&backend, request);
    const std::string header = "ask " + backend.backend_id() + " -> route: " +
                               to_string(response.route) + ". ";

    if (response.is_ok()) {
        return header + "Tools and MCP servers are disabled for this request.\n" + response.text;
    }
    std::string failure = header + "failed (" + to_string(response.status) + "): " +
                          response.error.value_or("unknown error");
    const std::string diagnostic = backend.diagnostic();
    if (response.status == AssistantStatus::Unavailable && !diagnostic.empty()) {
        failure += "\n" + diagnostic;
    }
    return failure + "\nNo other provider was tried.";
}

std::string AssistantCommands::list_providers() {
    const bool claude_ready = claude_.is_available();
    const std::string claude_line = std::string("  claude  ") +
                                    (claude_ready ? "available    " : "unavailable  ") +
                                    "route: remote_cloud. " + claude_.diagnostic();
    const std::string hermes_line = "  hermes  unavailable  route: unknown. " + hermes_.diagnostic();

    return std::string("Assistant providers:\n") +
           "  native  not implemented  CIPHER has no native model runtime yet.\n" +
           claude_line + "\n" +
           hermes_line;
}

} // namespace cipher
