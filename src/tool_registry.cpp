#include "cipher/tool_registry.hpp"

#include "cipher/cipher_core.hpp"
#include "cipher/memory.hpp"

#include <algorithm>
#include <cctype>
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

const ToolDefinition* ToolRegistry::find(const std::string& id) const {
    const auto found = tools_.find(id);
    return found == tools_.end() ? nullptr : &found->second.definition;
}

const char* to_string(CallOrigin origin) noexcept {
    return origin == CallOrigin::Model ? "model" : "owner";
}

ToolResult ToolRegistry::invoke(const std::string& id, const std::string& target, const std::string& arguments,
                                CallOrigin origin) {
    ToolResult result;
    const auto found = tools_.find(id);
    if (found == tools_.end()) {
        result.outcome = ToolOutcome::NotFound;
        result.output = "Unknown tool: " + id;
        record(id, target, arguments.size(), result, origin);
        return result;
    }

    const ToolDefinition& definition = found->second.definition;
    std::string evaluated_target = target;
    if (definition.resolve_target) {
        try {
            evaluated_target = definition.resolve_target(arguments);
        } catch (const std::exception& error) {
            // Fail closed: an argument that cannot be resolved never reaches the policy or the handler.
            result.outcome = ToolOutcome::Denied;
            result.output = error.what();
            record(id, "", arguments.size(), result, origin);
            return result;
        }
    }

    ActionDescriptor action{id, evaluated_target, definition.category, definition.default_tier};
    result.tier = policy_.evaluate(action);
    if (result.tier == RiskTier::Block) {
        result.outcome = ToolOutcome::Denied;
        result.output = "Denied by permission policy (block): " + id + ".";
        record(id, evaluated_target, arguments.size(), result, origin);
        return result;
    }
    // Fail closed: without an owner confirmation callback, an Ask-tier call never runs.
    if (result.tier == RiskTier::Ask && !(confirm_ && confirm_(definition, evaluated_target))) {
        result.outcome = ToolOutcome::Denied;
        result.output = "Owner confirmation was not given: " + id + ".";
        record(id, evaluated_target, arguments.size(), result, origin);
        return result;
    }

    if (dry_run_) {
        result.outcome = ToolOutcome::DryRun;
        result.output = "Dry run: " + id + " was not executed.";
        record(id, evaluated_target, arguments.size(), result, origin);
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
    record(id, evaluated_target, arguments.size(), result, origin);
    return result;
}

void ToolRegistry::record(const std::string& id, const std::string& target, std::size_t argument_bytes, const ToolResult& result,
                          CallOrigin origin) {
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
    entry.origin = origin;
    audit_.push_back(entry);
}

std::string format_local_time(std::time_t when) {
    std::tm local_tm{};
#if defined(_WIN32)
    if (localtime_s(&local_tm, &when) != 0) {
        return "The local system time is unavailable.";
    }
#else
    if (localtime_r(&when, &local_tm) == nullptr) {
        return "The local system time is unavailable.";
    }
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local_tm);
    return buffer;
}

void register_builtin_tools(ToolRegistry& registry) {
    registry.register_tool(ToolDefinition{
        "time.now",
        "Returns the current local time.",
        "system",
        RiskTier::Safe,
        SideEffect::None,
        [](const std::string&) { return format_local_time(std::time(nullptr)); }});

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

namespace {

constexpr std::size_t kMemorySearchMaxHits = 5;
constexpr std::size_t kMemorySearchSnippetBytes = 200;
constexpr std::size_t kMemorySearchMaxKeywordBytes = 64;

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Entry text stays one line and printable so it cannot imitate a TOOL_CALL line to the model.
std::string one_line_snippet(const std::string& content) {
    std::string out = content.substr(0, kMemorySearchSnippetBytes);
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
    }
    return out;
}

} // namespace

void register_memory_tools(ToolRegistry& registry, const std::string& memory_file_path,
                           PermissionPolicy memory_policy) {
    registry.register_tool(ToolDefinition{
        "memory.search",
        "Searches the owner's memory entries for a keyword. Argument is one keyword; returns at most 5 short matches.",
        "memory",
        RiskTier::Monitor,
        SideEffect::ReadLocal,
        [memory_file_path, memory_policy](const std::string& arguments) {
            const std::string keyword = lowered(arguments);
            if (keyword.empty() || keyword.size() > kMemorySearchMaxKeywordBytes) {
                throw std::runtime_error("memory.search needs a keyword of 1 to 64 bytes.");
            }
            MemoryStore store;
            if (!store.load(memory_file_path).is_ok()) {
                throw std::runtime_error("memory file could not be read.");
            }
            std::string hits;
            std::size_t count = 0;
            for (const MemoryEntry& entry : store.all()) {
                const std::string haystack = lowered(entry.id + "\n" + entry.category + "\n" + entry.content);
                if (haystack.find(keyword) == std::string::npos) continue;
                ActionDescriptor descriptor;
                descriptor.name = "recall";
                descriptor.target = entry.id;
                descriptor.category = "memory";
                descriptor.default_tier = RiskTier::Monitor;
                const RiskTier tier = memory_policy.evaluate(descriptor);
                if (tier != RiskTier::Safe && tier != RiskTier::Monitor) continue;
                if (++count > kMemorySearchMaxHits) break;
                hits += entry.id + " [" + entry.category + "]: " + one_line_snippet(entry.content) + "\n";
            }
            return hits.empty() ? std::string("No matching memory entries.") : hits;
        }});

    registry.register_tool(ToolDefinition{
        "memory.list",
        "Lists memory entry IDs only. Contents are never returned.",
        "memory",
        RiskTier::Monitor,
        SideEffect::ReadLocal,
        [memory_file_path](const std::string&) {
            MemoryStore store;
            const MemoryResult loaded = store.load(memory_file_path);
            if (!loaded.is_ok()) {
                throw std::runtime_error("memory file could not be read: " + memory_file_path);
            }
            std::string listing;
            for (const MemoryEntry& entry : store.all()) {
                listing += entry.id + "\n";
            }
            return listing.empty() ? std::string("No memory entries.") : listing;
        }});
}

} // namespace cipher
