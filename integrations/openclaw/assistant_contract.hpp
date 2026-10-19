#ifndef CIPHER_ASSISTANT_CONTRACT_HPP
#define CIPHER_ASSISTANT_CONTRACT_HPP

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cipher {

enum class AssistantStatus {
    Ok,
    Disabled,
    Unavailable,
    ConnectionError,
    AuthError,
    Timeout,
    Error,
    RejectedCloudModel
};

inline const char* to_string(AssistantStatus status) {
    switch (status) {
        case AssistantStatus::Ok: return "ok";
        case AssistantStatus::Disabled: return "disabled";
        case AssistantStatus::Unavailable: return "unavailable";
        case AssistantStatus::ConnectionError: return "connection_error";
        case AssistantStatus::AuthError: return "auth_error";
        case AssistantStatus::Timeout: return "timeout";
        case AssistantStatus::Error: return "error";
        case AssistantStatus::RejectedCloudModel: return "rejected_cloud_model";
    }
    return "unknown";
}

struct AssistantRequest {
    std::string prompt;
    std::optional<std::string> session_id;
    std::optional<double> timeout_seconds;
    std::optional<std::string> model_override;
};

struct AssistantResponse {
    AssistantStatus status{AssistantStatus::Error};
    std::string text;
    std::string model;
    double duration_ms{0.0};
    std::optional<std::string> error;
    std::optional<std::string> raw_output;

    bool is_ok() const { return status == AssistantStatus::Ok; }
};

/**
 * @brief Pure virtual interface for optional CIPHER assistant capability providers.
 *
 * In Milestone v0.4, CIPHER Core can hold an optional std::unique_ptr<IAssistantBackend>.
 * If no backend is configured or enabled, CIPHER Core continues to function normally
 * with its built-in commands (help, status, time, echo, remember, recall, etc.).
 */
class IAssistantBackend {
public:
    virtual ~IAssistantBackend() = default;

    virtual AssistantResponse submit_prompt(const AssistantRequest& request) = 0;
    virtual bool is_available() = 0;
    virtual std::string backend_name() const = 0;
};

} // namespace cipher

#endif // CIPHER_ASSISTANT_CONTRACT_HPP
