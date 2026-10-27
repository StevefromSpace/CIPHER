#include "cipher/tool_commands.hpp"

#include "cipher/filesystem_tools.hpp"

#include <string>

namespace cipher {
namespace {

const char* const USAGE_TEXT =
    "Tool commands:\n"
    "  tool list            List the available tools and their risk tiers\n"
    "  tool <id> [args]     Run a tool (permission is checked first)\n"
    "  tool log             Show this session's tool attempts (arguments are not shown)";

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

// Splits "word rest of text" into the first word and the trimmed remainder.
void split_first_word(const std::string& text, std::string& word, std::string& rest) {
    const auto separator = text.find_first_of(" \t");
    word = text.substr(0, separator);
    rest = separator == std::string::npos ? std::string{} : trim(text.substr(separator + 1));
}

std::string outcome_name(ToolOutcome outcome) {
    switch (outcome) {
        case ToolOutcome::Completed: return "Completed";
        case ToolOutcome::DryRun:    return "DryRun";
        case ToolOutcome::Denied:    return "Denied";
        case ToolOutcome::NotFound:  return "NotFound";
        case ToolOutcome::Failed:    return "Failed";
    }
    return "Unknown";
}

} // namespace

ToolCommands::ToolCommands(const std::string& memory_file_path, const std::string& filesystem_root_path,
                           std::istream& answers, std::ostream& prompts)
    : answers_(answers), prompts_(prompts) {
    register_builtin_tools(registry_);
    if (!memory_file_path.empty()) {
        register_memory_tools(registry_, memory_file_path);
    }
    if (!filesystem_root_path.empty()) {
        register_filesystem_tools(registry_, filesystem_root_path);
    }
    registry_.set_ask_confirmation([this](const ToolDefinition& definition, const std::string& target) {
        return confirm(definition, target);
    });
}

ToolCommandResult ToolCommands::execute(const std::string& input) {
    std::string command;
    std::string rest;
    split_first_word(trim(input), command, rest);
    if (command != "tool") {
        return {};
    }

    std::string subcommand;
    std::string arguments;
    split_first_word(rest, subcommand, arguments);

    if (subcommand.empty()) {
        return {true, USAGE_TEXT};
    }
    if (subcommand == "list") {
        return {true, list()};
    }
    if (subcommand == "log") {
        return {true, log()};
    }
    return {true, invoke(subcommand, arguments)};
}

std::string ToolCommands::list() const {
    std::string text = "Tools:";
    for (const std::string& id : registry_.tool_ids()) {
        const ToolDefinition* definition = registry_.find(id);
        text += "\n  " + id + "  [" + to_string(definition->default_tier) + "]  " + definition->description;
    }
    return text;
}

std::string ToolCommands::log() const {
    if (registry_.audit_log().empty()) {
        return "No tool attempts this session.";
    }
    std::string text;
    for (const AuditRecord& entry : registry_.audit_log()) {
        if (!text.empty()) {
            text += "\n";
        }
        text += "#" + std::to_string(entry.sequence) + "  " + entry.correlation_id + "  " + entry.tool_id +
                "  tier=" + to_string(entry.tier) + "  outcome=" + outcome_name(entry.outcome) +
                "  executed=" + (entry.executed ? "yes" : "no") +
                "  argument_bytes=" + std::to_string(entry.argument_bytes) +
                "  " + format_local_time(entry.timestamp);
    }
    return text;
}

std::string ToolCommands::invoke(const std::string& id, const std::string& arguments) {
    // Built-in tools have no target, so the owner command passes an empty one.
    const ToolResult result = registry_.invoke(id, "", arguments);
    return id + ": " + outcome_name(result.outcome) + " (tier " + to_string(result.tier) +
           (result.executed ? ", executed)" : ", not executed)") + "\n" + result.output;
}

bool ToolCommands::confirm(const ToolDefinition& definition, const std::string& target) {
    prompts_ << "Owner confirmation: run " << definition.id;
    if (!target.empty()) {
        prompts_ << " (" << target << ")";
    }
    prompts_ << "? Type yes to confirm: " << std::flush;
    std::string answer;
    if (!std::getline(answers_, answer)) {
        return false;
    }
    return trim(answer) == "yes";
}

} // namespace cipher
