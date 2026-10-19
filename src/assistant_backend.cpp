#include "cipher/assistant_backend.hpp"

#include <cctype>
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

ValidationResult validate_assistant_request(
    const AssistantRequest& request,
    const AssistantCapabilities& capabilities
) {
    // Invariant 1: Reject blank or whitespace-only prompts
    std::string trimmed_prompt = trim_string(request.prompt);
    if (trimmed_prompt.empty()) {
        return {false, "Prompt must not be empty or whitespace only."};
    }

    // Invariant 2: Reject timeouts outside [1.0, 300.0] seconds
    if (request.timeout_seconds < 1.0 || request.timeout_seconds > 300.0) {
        return {false, "Timeout must be bounded between 1.0 and 300.0 seconds."};
    }

    // Invariant 3: Reject tool execution requests (fail closed until permissioned tool gate)
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
