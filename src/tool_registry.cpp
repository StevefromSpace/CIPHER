#include "cipher/tool_registry.hpp"

#include <exception>

namespace cipher {

bool ToolRegistry::register_tool(const ActionDescriptor& descriptor, Handler handler) {
    if (descriptor.name.empty() || !handler || tools_.count(descriptor.name) != 0) {
        return false;
    }
    tools_.emplace(descriptor.name, Entry{descriptor, std::move(handler)});
    return true;
}

ToolResult ToolRegistry::invoke(const std::string& name, const std::string& input) {
    ToolResult result;
    const auto found = tools_.find(name);
    if (found == tools_.end()) {
        result.outcome = ToolOutcome::NotFound;
        log_.push_back({name, result.tier, result.outcome});
        return result;
    }

    const Entry& entry = found->second;
    result.tier = policy_.evaluate(entry.descriptor);

    if (result.tier == RiskTier::Ask || result.tier == RiskTier::Block) {
        result.outcome = ToolOutcome::Denied;
    } else if (dry_run_) {
        result.outcome = ToolOutcome::DryRun;
    } else {
        try {
            result.output = entry.handler(input);
            result.outcome = ToolOutcome::Executed;
        } catch (const std::exception& error) {
            result.output = error.what();
            result.outcome = ToolOutcome::Failed;
        }
    }

    log_.push_back({name, result.tier, result.outcome});
    return result;
}

} // namespace cipher
