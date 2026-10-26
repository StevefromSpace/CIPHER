#include "cipher/memory_commands.hpp"

#include <string>
#include <utility>

namespace cipher {
namespace {

const char* const DISABLED_TEXT =
    "Memory is disabled. Set memory_file in cipher.conf to enable remember, recall, forget, and memories.";

const char* const UNAVAILABLE_TEXT =
    "Memory is unavailable because its file could not be loaded. Fix or move the file, then restart CIPHER.";

const char* const HELP_TEXT =
    "Memory commands (enabled by memory_file in cipher.conf):\n"
    "  remember <id> <text>  Store text under an id (saved immediately)\n"
    "  recall <id>           Show the text stored under an id\n"
    "  forget <id>           Delete the text stored under an id\n"
    "  memories              List stored ids";

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

bool is_memory_command(const std::string& command) {
    return command == "remember" || command == "recall" ||
           command == "forget"   || command == "memories";
}

} // namespace

MemoryCommands::MemoryCommands(std::string file_path, PermissionPolicy policy)
    : file_path_(std::move(file_path)), policy_(std::move(policy)) {}

MemoryResult MemoryCommands::open() {
    if (!enabled()) {
        MemoryResult disabled;
        disabled.ok = true;
        return disabled;
    }
    MemoryResult loaded = store_.load(file_path_);
    available_ = loaded.is_ok();
    return loaded;
}

MemoryCommandResult MemoryCommands::execute(const std::string& input) {
    const std::string trimmed = trim(input);
    std::string command;
    std::string arguments;
    split_first_word(trimmed, command, arguments);

    if (command == "help") {
        return {false, enabled() ? HELP_TEXT : ""};
    }
    if (!is_memory_command(command)) {
        return {};
    }
    return {true, run(command, arguments)};
}

std::string MemoryCommands::run(const std::string& command, const std::string& arguments) {
    if (!enabled()) {
        return DISABLED_TEXT;
    }
    if (!available_) {
        return UNAVAILABLE_TEXT;
    }
    if (command == "memories") {
        return list_ids();
    }

    std::string id;
    std::string content;
    split_first_word(arguments, id, content);

    if (command == "remember") {
        if (id.empty() || content.empty()) {
            return "Usage: remember <id> <text>";
        }
        return remember(id, content);
    }

    // recall and forget take exactly one id.
    if (id.empty() || !content.empty()) {
        return "Usage: " + command + " <id>";
    }
    return command == "recall" ? recall(id) : forget(id);
}

std::string MemoryCommands::remember(const std::string& id, const std::string& content) {
    const std::string denial = check_permission("remember", id);
    if (!denial.empty()) {
        return denial;
    }
    const MemoryStore before = store_;
    // An update keeps the category the owner set, including hand-edits in the file.
    const auto existing = store_.get(id);
    const std::string category = existing ? existing->category : "general";
    std::string error;
    if (!store_.set(id, content, category, &error)) {
        return error;
    }
    const std::string failure = save_or_roll_back(before);
    if (!failure.empty()) {
        return failure;
    }
    return (existing ? "Updated '" : "Remembered '") + id + "'.";
}

std::string MemoryCommands::recall(const std::string& id) const {
    const auto entry = store_.get(id);
    if (!entry) {
        return "No memory with id '" + id + "'.";
    }
    return entry->content;
}

std::string MemoryCommands::forget(const std::string& id) {
    const std::string denial = check_permission("forget", id);
    if (!denial.empty()) {
        return denial;
    }
    const MemoryStore before = store_;
    if (!store_.remove(id)) {
        return "No memory with id '" + id + "'.";
    }
    const std::string failure = save_or_roll_back(before);
    return failure.empty() ? "Forgot '" + id + "'." : failure;
}

std::string MemoryCommands::list_ids() const {
    if (store_.empty()) {
        return "No memories stored.";
    }
    std::string listing = "Stored memories:";
    for (const auto& entry : store_.all()) {
        listing += "\n  " + entry.id;
    }
    return listing;
}

// Empty string when the action may run. Monitor is allowed; Ask and Block are
// denied. The blacklist and whitelist inside PermissionPolicy decide the tier.
std::string MemoryCommands::check_permission(const std::string& action, const std::string& id) const {
    ActionDescriptor descriptor;
    descriptor.name = action;
    descriptor.target = id;
    descriptor.category = "memory";
    descriptor.default_tier = RiskTier::Monitor;

    const RiskTier tier = policy_.evaluate(descriptor);
    if (tier == RiskTier::Safe || tier == RiskTier::Monitor) {
        return {};
    }
    return "Denied by permission policy (" + to_string(tier) + "): " + action + " '" + id +
           "'. Nothing was changed.";
}

// Returns an empty string on success. On failure, restores `before` and
// returns the message to show the user.
std::string MemoryCommands::save_or_roll_back(const MemoryStore& before) {
    const MemoryResult saved = store_.save(file_path_);
    if (saved.is_ok()) {
        return {};
    }
    store_ = before;
    std::string reason = "unknown error";
    if (!saved.errors.empty()) {
        reason = saved.errors.front().message;
    }
    return "Could not save memory: " + reason + "\nNothing was changed.";
}

} // namespace cipher
