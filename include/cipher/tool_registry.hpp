#ifndef CIPHER_TOOL_REGISTRY_HPP
#define CIPHER_TOOL_REGISTRY_HPP

#include "cipher/assistant_backend.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cipher {

/**
 * @brief Outcome of a single tool invocation attempt.
 */
enum class ToolOutcome {
    Executed,  // Handler ran and returned output
    DryRun,    // Permission allowed the call, but dry-run mode skipped the handler
    Denied,    // Policy tier Ask or Block: handler was never called (fail closed)
    NotFound,  // No tool registered under that name
    Failed     // Handler threw; output carries the exception message
};

/**
 * @brief Result of one ToolRegistry::invoke() call.
 */
struct ToolResult {
    ToolOutcome outcome{ToolOutcome::NotFound};
    RiskTier tier{RiskTier::Block};
    std::string output;
};

/**
 * @brief One audit-log record per invocation attempt, including denials.
 */
struct ToolLogEntry {
    std::string tool;
    RiskTier tier{RiskTier::Block};
    ToolOutcome outcome{ToolOutcome::NotFound};
};

/**
 * @brief Registry of named tools gated by PermissionPolicy.
 *
 * Only RiskTier::Safe and RiskTier::Monitor tools run. Ask (needs owner confirmation,
 * not yet implemented) and Block are refused without calling the handler.
 * The registry registers no built-in tools; callers add them explicitly.
 */
class ToolRegistry {
public:
    using Handler = std::function<std::string(const std::string& input)>;

    /// Returns false (and registers nothing) when the name is empty or already taken.
    bool register_tool(const ActionDescriptor& descriptor, Handler handler);

    /// Applies the policy, then runs the handler unless denied or in dry-run mode.
    ToolResult invoke(const std::string& name, const std::string& input);

    void set_dry_run(bool enabled) { dry_run_ = enabled; }
    bool dry_run() const { return dry_run_; }

    PermissionPolicy& policy() { return policy_; }
    const std::vector<ToolLogEntry>& log() const { return log_; }

private:
    struct Entry {
        ActionDescriptor descriptor;
        Handler handler;
    };

    std::map<std::string, Entry> tools_;
    PermissionPolicy policy_;
    bool dry_run_{false};
    std::vector<ToolLogEntry> log_;
};

} // namespace cipher

#endif // CIPHER_TOOL_REGISTRY_HPP
