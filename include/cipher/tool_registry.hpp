#ifndef CIPHER_TOOL_REGISTRY_HPP
#define CIPHER_TOOL_REGISTRY_HPP

#include "cipher/assistant_backend.hpp"

#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cipher {

/**
 * @brief Declared side-effect class of a tool. Registration rejects Process and Network.
 */
enum class SideEffect {
    None,
    ReadLocal,
    WriteLocal,
    Process,
    Network
};

/**
 * @brief Lifecycle result of a single invocation.
 *
 * Denied and NotFound never call the handler. Failed means the handler ran and raised an error.
 */
enum class ToolOutcome {
    Completed,
    DryRun,
    Denied,
    NotFound,
    Failed
};

/**
 * @brief Tool body. Receives the argument string and returns the output text.
 */
using ToolHandler = std::function<std::string(const std::string& arguments)>;

struct ToolDefinition {
    std::string id;
    std::string description;
    std::string category;
    RiskTier default_tier{RiskTier::Ask};
    SideEffect side_effects{SideEffect::None};
    ToolHandler handler;
};

struct ToolResult {
    ToolOutcome outcome{ToolOutcome::NotFound};
    RiskTier tier{RiskTier::Block};
    /// True only when the handler was called (Completed or Failed).
    bool executed{false};
    std::string output;
};

/**
 * @brief One session-only audit record. Arguments and output are never stored.
 */
struct AuditRecord {
    std::uint64_t sequence{0};
    std::string correlation_id;
    std::string tool_id;
    std::string target;
    RiskTier tier{RiskTier::Block};
    ToolOutcome outcome{ToolOutcome::NotFound};
    bool executed{false};
    std::size_t argument_bytes{0};
    std::time_t timestamp{0};
};

/**
 * @brief Local wall-clock time as "YYYY-MM-DD HH:MM:SS".
 */
std::string format_local_time(std::time_t when);

/**
 * @brief Static, built-in-only tool registry gated by the existing PermissionPolicy.
 */
class ToolRegistry {
public:
    ToolRegistry();

    /**
     * @brief Registers a tool. Throws std::invalid_argument on an invalid ID, duplicate ID,
     *        null handler, a Process/Network side effect, or WriteLocal without Ask/Block.
     */
    void register_tool(const ToolDefinition& definition);

    /**
     * @brief Invokes a tool. Permission is evaluated on "<id> <target>" before any handler call.
     */
    ToolResult invoke(const std::string& id, const std::string& target, const std::string& arguments);

    void set_dry_run(bool enabled) noexcept { dry_run_ = enabled; }
    bool dry_run() const noexcept { return dry_run_; }

    PermissionPolicy& policy() noexcept { return policy_; }
    const std::vector<AuditRecord>& audit_log() const noexcept { return audit_; }
    std::vector<std::string> tool_ids() const;

    /// Returns the registered definition, or nullptr for an unknown ID.
    const ToolDefinition* find(const std::string& id) const;

private:
    struct Entry {
        ToolDefinition definition;
    };

    void record(const std::string& id, const std::string& target, std::size_t argument_bytes, const ToolResult& result);

    std::map<std::string, Entry> tools_;
    PermissionPolicy policy_;
    bool dry_run_{false};
    std::uint64_t next_sequence_{1};
    std::string session_tag_;
    std::vector<AuditRecord> audit_;
};

/**
 * @brief Registers the approved initial built-in tools: time.now and system.info (both Safe).
 */
void register_builtin_tools(ToolRegistry& registry);

/**
 * @brief Registers memory.list (Monitor), which lists memory entry IDs only from the given file.
 *        The file is read at invocation time. Kept separate from register_builtin_tools so the
 *        built-in set stays path-free.
 */
void register_memory_tools(ToolRegistry& registry, const std::string& memory_file_path);

} // namespace cipher

#endif // CIPHER_TOOL_REGISTRY_HPP
