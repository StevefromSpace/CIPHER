#include "cipher/tool_registry.hpp"

#include "cipher/cipher_core.hpp"

#include <ctime>
#include <exception>
#include <random>
#include <stdexcept>

namespace cipher {

namespace {

bool is_valid_tool_id(const std::string& id) {
    if (id.empty() || id.front() == '.' || id.back() == '.') {
        return false;
    }
    for (char c : id) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

// Local wall-clock time as "YYYY-MM-DD HH:MM:SS". Mirrors the core format;
// duplicated because cipher_core keeps its helper private.
std::string local_time_text() {
    const std::time_t now = std::time(nullptr);
    std::tm local_tm{};
#if defined(_WIN32)
    if (localtime_s(&local_tm, &now) != 0) {
        return "The local system time is unavailable.";
    }
#else
    if (localtime_r(&now, &local_tm) == nullptr) {
        return "The local system time is unavailable.";
    }
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local_tm);
    return buffer;
}

std::string make_session_tag() {
    std::random_device device;
    const std::uint32_t value = device();
    static constexpr const char* kHex = "0123456789abcdef";
    std::string tag;
    for (int shift = 28; shift >= 0; shift -= 4) {
        tag.push_back(kHex[(value >> shift) & 0xFU]);
    }
    return tag;
}

} // namespace

ToolRegistry::ToolRegistry() : session_tag_(make_session_tag()) {}

void ToolRegistry::register_tool(const ToolDefinition& definition) {
    if (!is_valid_tool_id(definition.id)) {
        throw std::invalid_argument("invalid tool id: '" + definition.id + "'");
    }
    if (tools_.count(definition.id) != 0) {
        throw std::invalid_argument("duplicate tool id: '" + definition.id + "'");
    }
    if (!definition.handler) {
        throw std::invalid_argument("null handler for tool: '" + definition.id + "'");
    }
    if (definition.side_effects == SideEffect::Process || definition.side_effects == SideEffect::Network) {
        throw std::invalid_argument("process and network tools are not allowed: '" + definition.id + "'");
    }
    const bool write_is_gated = definition.default_tier == RiskTier::Ask || definition.default_tier == RiskTier::Block;
    if (definition.side_effects == SideEffect::WriteLocal && !write_is_gated) {
        throw std::invalid_argument("write tool must default to Ask or Block: '" + definition.id + "'");
    }
    tools_.emplace(definition.id, Entry{definition});
}

std::vector<std::string> ToolRegistry::tool_ids() const {
    std::vector<std::string> ids;
    ids.reserve(tools_.size());
    for (const auto& item : tools_) {
        ids.push_back(item.first);
    }
    return ids;
}

ToolResult ToolRegistry::invoke(const std::string& id, const std::string& target, const std::string& arguments) {
    ToolResult result;
    const auto found = tools_.find(id);
    if (found == tools_.end()) {
        result.outcome = ToolOutcome::NotFound;
        result.output = "Unknown tool: " + id;
        record(id, target, arguments.size(), result);
        return result;
    }

    const ToolDefinition& definition = found->second.definition;
    ActionDescriptor action{id, target, definition.category, definition.default_tier};
    result.tier = policy_.evaluate(action);
    if (result.tier == RiskTier::Ask || result.tier == RiskTier::Block) {
        result.outcome = ToolOutcome::Denied;
        result.output = "Denied by permission policy (" + std::string(to_string(result.tier)) + "): " + id + ".";
        record(id, target, arguments.size(), result);
        return result;
    }

    if (dry_run_) {
        result.outcome = ToolOutcome::DryRun;
        result.output = "Dry run: " + id + " was not executed.";
        record(id, target, arguments.size(), result);
        return result;
    }

    result.executed = true;
    try {
        result.output = definition.handler(arguments);
        result.outcome = ToolOutcome::Completed;
    } catch (const std::exception& error) {
        result.outcome = ToolOutcome::Failed;
        result.output = error.what();
    } catch (...) {
        result.outcome = ToolOutcome::Failed;
        result.output = "Unknown error";
    }
    record(id, target, arguments.size(), result);
    return result;
}

void ToolRegistry::record(const std::string& id, const std::string& target, std::size_t argument_bytes, const ToolResult& result) {
    AuditRecord entry;
    entry.sequence = next_sequence_++;
    entry.correlation_id = session_tag_ + "-" + std::to_string(entry.sequence);
    entry.tool_id = id;
    entry.target = target;
    entry.tier = result.tier;
    entry.outcome = result.outcome;
    entry.executed = result.executed;
    entry.argument_bytes = argument_bytes;
    entry.timestamp = std::time(nullptr);
    audit_.push_back(entry);
}

void register_builtin_tools(ToolRegistry& registry) {
    registry.register_tool(ToolDefinition{
        "time.now",
        "Returns the current local time.",
        "system",
        RiskTier::Safe,
        SideEffect::None,
        [](const std::string&) { return local_time_text(); }});

    registry.register_tool(ToolDefinition{
        "system.info",
        "Returns the CIPHER version, build type and platform family only.",
        "system",
        RiskTier::Safe,
        SideEffect::None,
        [](const std::string&) {
#ifdef NDEBUG
            const char* build = "Release";
#else
            const char* build = "Debug";
#endif
#ifdef _WIN32
            const char* platform = "Windows";
#else
            const char* platform = "POSIX";
#endif
            return "CIPHER v" + std::string(CipherCore::version()) + "\nBuild: " + build + "\nPlatform: " + platform;
        }});
}

} // namespace cipher
