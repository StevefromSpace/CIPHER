#ifndef CIPHER_ASSISTANT_BACKEND_HPP
#define CIPHER_ASSISTANT_BACKEND_HPP

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cipher {

/**
 * @brief Execution status returned by an assistant capability provider.
 */
enum class AssistantStatus {
    Ok,
    Disabled,
    Unavailable,
    ConnectionError,
    AuthError,
    Timeout,
    PolicyViolation,
    Error
};

/**
 * @brief Provenance of the model inference processing route.
 */
enum class ProcessingRoute {
    VerifiedLocal, // Cryptographically or process-verified local execution
    RemoteCloud,   // Hosted commercial cloud model
    Unknown,       // Unverified / cannot be proven local
    Mixed          // Hybrid or cloud fallback enabled
};

/**
 * @brief Capability descriptors advertised by an assistant backend.
 */
struct AssistantCapabilities {
    bool supports_text_conversation{true};
    bool supports_sessions{false};
    bool supports_streaming{false};
    ProcessingRoute processing_route{ProcessingRoute::Unknown};
};

/**
 * @brief Standardized prompt submission to an assistant backend.
 */
struct AssistantRequest {
    std::string prompt;
    std::optional<std::string> session_id{std::nullopt};
    double timeout_seconds{30.0};
    bool local_only{true};
    bool request_tools{false}; // Strictly rejected until permissioned tool gate
    std::optional<std::string> model_override{std::nullopt};
};

/**
 * @brief Standardized response returned from an assistant backend.
 */
struct AssistantResponse {
    AssistantStatus status{AssistantStatus::Error};
    std::string text;
    std::string model;
    double duration_ms{0.0};
    std::optional<std::string> error{std::nullopt};

    bool is_ok() const { return status == AssistantStatus::Ok; }
};

/**
 * @brief Validation result containing policy check details.
 */
struct ValidationResult {
    bool is_valid{true};
    std::string error_message;
};

/**
 * @brief Converts an AssistantStatus enum to a string label.
 */
std::string to_string(AssistantStatus status);

/**
 * @brief Converts a ProcessingRoute enum to a string label.
 */
std::string to_string(ProcessingRoute route);

/**
 * @brief Validates an AssistantRequest against CIPHER's policy invariants.
 *
 * Invariants:
 * 1. Prompts must not be empty or whitespace-only.
 * 2. Timeout must be bounded between 1.0 and 300.0 seconds.
 * 3. Tool requests are strictly prohibited (fails closed pending permissioned tools).
 * 4. Local-only requests are rejected unless capabilities.processing_route == VerifiedLocal.
 *    Model name prefixes or loopback URLs alone do not constitute proof.
 */
ValidationResult validate_assistant_request(
    const AssistantRequest& request,
    const AssistantCapabilities& capabilities
);

/**
 * @brief Abstract interface for optional assistant capability providers.
 */
class IAssistantBackend {
public:
    virtual ~IAssistantBackend() = default;

    virtual std::string backend_id() const = 0;
    virtual bool is_available() = 0;
    virtual AssistantCapabilities capabilities() const = 0;
    virtual AssistantResponse execute_request(const AssistantRequest& request) = 0;
};

/**
 * @brief Central coordinator enforcing validation invariants before backend invocation.
 */
class AssistantManager {
public:
    AssistantManager() = default;

    /**
     * @brief Submits a prompt through central validation and dispatches to the backend.
     */
    static AssistantResponse submit(
        IAssistantBackend* backend,
        const AssistantRequest& request
    );
};

} // namespace cipher

#endif // CIPHER_ASSISTANT_BACKEND_HPP
