#ifndef CIPHER_ASSISTANT_BACKEND_HPP
#define CIPHER_ASSISTANT_BACKEND_HPP

#include <chrono>
#include <cmath>
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
 * @brief Risk tier classification for actions and tools (inspired by Dodaios research).
 */
enum class RiskTier {
    Safe,    // Harmless, read-only deterministic action (auto-approved)
    Monitor, // Observable action logged to audit stream
    Ask,     // State-mutating action requiring explicit owner confirmation
    Block    // Forbidden action rejected fail-closed
};

/**
 * @brief Descriptor for an action subjected to permission evaluation.
 */
struct ActionDescriptor {
    std::string name;
    std::string target;
    std::string category;
    RiskTier default_tier{RiskTier::Ask};
};

/**
 * @brief Context window and prompt byte bounding (inspired by OpenClaw research).
 */
struct ContextBudget {
    std::size_t max_prompt_bytes{32768};
    std::size_t max_context_bytes{131072};
    std::size_t max_history_turns{20};
};

/**
 * @brief Standardized event message envelope (inspired by OpenVoiceOS OVOS-MSG-1 research).
 */
struct EventMessage {
    std::string type;
    std::string data;
    std::string session_id;
    std::string source{"cipher"};
    std::string destination{"assistant"};
    double timestamp{0.0};
};

/**
 * @brief Evaluates action permissions following strict Dodaios hierarchy:
 * Priority: Blacklist (Block) > Whitelist (Safe) > Tool Default.
 */
class PermissionPolicy {
public:
    PermissionPolicy() = default;

    void add_blacklist_pattern(const std::string& pattern);
    void add_whitelist_pattern(const std::string& pattern);
    void set_default_tier(RiskTier tier);

    RiskTier evaluate(const ActionDescriptor& action) const;
    bool is_permitted(const ActionDescriptor& action) const;

    const std::vector<std::string>& blacklist_patterns() const { return blacklist_patterns_; }
    const std::vector<std::string>& whitelist_patterns() const { return whitelist_patterns_; }
    RiskTier default_tier() const { return default_tier_; }

private:
    std::vector<std::string> blacklist_patterns_;
    std::vector<std::string> whitelist_patterns_;
    RiskTier default_tier_{RiskTier::Ask};

    static bool matches_pattern(const std::string& text, const std::string& pattern);
};

/**
 * @brief Capability descriptors advertised by an assistant backend.
 */
struct AssistantCapabilities {
    bool supports_text_conversation{true};
    bool supports_sessions{false};
    bool supports_streaming{false};
    bool supports_voice_events{false};
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
    ContextBudget budget{};
};

/**
 * @brief Standardized response returned from an assistant backend.
 */
struct AssistantResponse {
    AssistantStatus status{AssistantStatus::Error};
    std::string text;
    std::string model;
    ProcessingRoute route{ProcessingRoute::Unknown};
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
 * @brief Converts a RiskTier enum to a string label.
 */
std::string to_string(RiskTier tier);

/**
 * @brief Validates an AssistantRequest against CIPHER's policy invariants.
 *
 * Invariants:
 * 1. Prompts must not be empty or whitespace-only.
 * 2. Timeout must be finite and bounded between 1.0 and 300.0 seconds (rejects NaN, +Inf, -Inf).
 * 3. Tool requests are strictly prohibited (fails closed pending permissioned tools).
 * 4. Local-only requests are rejected unless capabilities.processing_route == VerifiedLocal.
 *    Model name prefixes or loopback URLs alone do not constitute proof.
 * 5. Prompt size must not exceed the defined ContextBudget.max_prompt_bytes.
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
