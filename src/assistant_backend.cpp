#include "cipher/assistant_backend.hpp"

#include <cctype>
#include <cmath>
#include <sstream>

namespace cipher {

namespace {

std::string trim_string(const std::string& str) {
    auto start = str.begin();
    while (start != str.end() && std::isspace(static_cast<unsigned char>(*start))) {
        ++start;
    }
    auto end = str.end();
    do {
        --end;
    } while (std::distance(start, end) > 0 && std::isspace(static_cast<unsigned char>(*end)));

    return (start <= end) ? std::string(start, end + 1) : std::string();
}

} // anonymous namespace

std::string to_string(AssistantStatus status) {
    switch (status) {
        case AssistantStatus::Ok: return "ok";
        case AssistantStatus::Disabled: return "disabled";
        case AssistantStatus::Unavailable: return "unavailable";
        case AssistantStatus::ConnectionError: return "connection_error";
        case AssistantStatus::AuthError: return "auth_error";
        case AssistantStatus::Timeout: return "timeout";
        case AssistantStatus::PolicyViolation: return "policy_violation";
        case AssistantStatus::Error: return "error";
    }
    return "unknown";
}

std::string to_string(ProcessingRoute route) {
    switch (route) {
        case ProcessingRoute::VerifiedLocal: return "verified_local";
        case ProcessingRoute::RemoteCloud: return "remote_cloud";
        case ProcessingRoute::Unknown: return "unknown";
        case ProcessingRoute::Mixed: return "mixed";
    }
    return "unknown";
}

std::string to_string(RiskTier tier) {
    switch (tier) {
        case RiskTier::Safe: return "safe";
        case RiskTier::Monitor: return "monitor";
        case RiskTier::Ask: return "ask";
        case RiskTier::Block: return "block";
    }
    return "unknown";
}

// -----------------------------------------------------------------------------
// PermissionPolicy Implementation
// -----------------------------------------------------------------------------

void PermissionPolicy::add_blacklist_pattern(const std::string& pattern) {
    if (!pattern.empty()) {
        blacklist_patterns_.push_back(pattern);
    }
}

void PermissionPolicy::add_whitelist_pattern(const std::string& pattern) {
    if (!pattern.empty()) {
        whitelist_patterns_.push_back(pattern);
    }
}

void PermissionPolicy::set_default_tier(RiskTier tier) {
    default_tier_ = tier;
}

bool PermissionPolicy::matches_pattern(const std::string& text, const std::string& pattern) {
    if (pattern == "*") {
        return true;
    }
    if (pattern.empty()) {
        return text.empty();
    }
    // Suffix wildcard: "cmd*"
    if (pattern.back() == '*') {
        std::string prefix = pattern.substr(0, pattern.size() - 1);
        return text.compare(0, prefix.size(), prefix) == 0;
    }
    // Prefix wildcard: "*cmd"
    if (pattern.front() == '*') {
        std::string suffix = pattern.substr(1);
        if (text.size() < suffix.size()) {
            return false;
        }
        return text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
    // Exact match
    return text == pattern;
}

RiskTier PermissionPolicy::evaluate(const ActionDescriptor& action) const {
    std::string composite = action.name;
    if (!action.target.empty()) {
        composite += " " + action.target;
    }

    // 1. Blacklist beats everything -> RiskTier::Block
    for (const auto& pattern : blacklist_patterns_) {
        if (matches_pattern(composite, pattern) || matches_pattern(action.name, pattern) ||
            (!action.category.empty() && matches_pattern(action.category, pattern))) {
            return RiskTier::Block;
        }
    }

    // 2. Whitelist downgrades to Safe
    for (const auto& pattern : whitelist_patterns_) {
        if (matches_pattern(composite, pattern) || matches_pattern(action.name, pattern) ||
            (!action.category.empty() && matches_pattern(action.category, pattern))) {
            return RiskTier::Safe;
        }
    }

    // 3. Fall back to tool default or policy default tier
    if (action.default_tier != RiskTier::Ask) {
        return action.default_tier;
    }
    return default_tier_;
}

bool PermissionPolicy::is_permitted(const ActionDescriptor& action) const {
    RiskTier tier = evaluate(action);
    return tier == RiskTier::Safe;
}

// -----------------------------------------------------------------------------
// Request Validation
// -----------------------------------------------------------------------------

ValidationResult validate_assistant_request(
    const AssistantRequest& request,
    const AssistantCapabilities& capabilities
) {
    // Invariant 1: Reject blank or whitespace-only prompts
    std::string trimmed_prompt = trim_string(request.prompt);
    if (trimmed_prompt.empty()) {
        return {false, "Prompt must not be empty or whitespace only."};
    }

    // Invariant 2: Reject non-finite timeouts or timeouts outside [1.0, 300.0] seconds
    if (std::isnan(request.timeout_seconds) || std::isinf(request.timeout_seconds) ||
        request.timeout_seconds < 1.0 || request.timeout_seconds > 300.0) {
        return {false, "Timeout must be a finite number bounded between 1.0 and 300.0 seconds."};
    }

    // Invariant 3: Reject tool execution requests (fails closed pending permissioned tool gate)
    if (request.request_tools) {
        return {false, "Tool execution is currently prohibited (fails closed pending permissioned tool gate)."};
    }

    // Invariant 4: Local-only requests must be rejected unless processing_route == VerifiedLocal
    if (request.local_only) {
        if (capabilities.processing_route != ProcessingRoute::VerifiedLocal) {
            std::ostringstream oss;
            oss << "Local-only policy violation: backend route is not verified local (detected route: "
                << to_string(capabilities.processing_route)
                << "). Model names or localhost URLs alone do not constitute proof of local inference.";
            return {false, oss.str()};
        }
    }

    // Invariant 5: Context budget prompt byte bounding
    if (request.budget.max_prompt_bytes > 0 && request.prompt.size() > request.budget.max_prompt_bytes) {
        std::ostringstream oss;
        oss << "Context budget violation: prompt size (" << request.prompt.size()
            << " bytes) exceeds maximum configured prompt limit ("
            << request.budget.max_prompt_bytes << " bytes).";
        return {false, oss.str()};
    }

    return {true, ""};
}

AssistantResponse AssistantManager::submit(
    IAssistantBackend* backend,
    const AssistantRequest& request
) {
    if (!backend) {
        AssistantResponse resp;
        resp.status = AssistantStatus::Unavailable;
        resp.error = "No assistant backend configured.";
        return resp;
    }

    if (!backend->is_available()) {
        AssistantResponse resp;
        resp.status = AssistantStatus::Unavailable;
        resp.error = "Assistant backend '" + backend->backend_id() + "' is currently unavailable.";
        return resp;
    }

    // Central policy validation
    ValidationResult val = validate_assistant_request(request, backend->capabilities());
    if (!val.is_valid) {
        AssistantResponse resp;
        resp.status = AssistantStatus::PolicyViolation;
        resp.error = val.error_message;
        return resp;
    }

    // Safe execution through backend
    return backend->execute_request(request);
}

} // namespace cipher
