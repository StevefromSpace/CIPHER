#ifndef CIPHER_OPENCLAW_BACKEND_HPP
#define CIPHER_OPENCLAW_BACKEND_HPP

#include "cipher/assistant_backend.hpp"
#include "cipher/orchestration/process_runner.hpp"

#include <memory>
#include <string>

namespace cipher {

struct OpenClawConfig {
    std::string cli_path;                       // absolute path of the openclaw executable; empty = unavailable
    std::string model{"ollama/llama3.2"};       // must be an ollama/ model: hosted models are refused
    std::string profile{"minimal"};
};

/**
 * @brief Assistant backend that runs the OpenClaw CLI once per request (no shell, closed argv).
 *
 * Fail-closed: a non-local model, a missing CLI, or any tool in OpenClaw's effective inventory makes the
 * backend refuse the request. OpenClaw is a replaceable upstream; CIPHER only depends on the argv contract here.
 */
class OpenClawBackend : public IAssistantBackend {
public:
    explicit OpenClawBackend(OpenClawConfig config,
                             std::shared_ptr<orchestration::ProcessRunner> runner =
                                 std::make_shared<orchestration::SystemProcessRunner>());

    std::string backend_id() const override { return "openclaw"; }
    bool is_available() override;
    AssistantCapabilities capabilities() const override;
    AssistantResponse execute_request(const AssistantRequest& request) override;

private:
    // Empty string = the tool inventory is verified empty; otherwise the reason it is not.
    std::string tool_inventory_problem(int timeout_ms);

    OpenClawConfig config_;
    std::shared_ptr<orchestration::ProcessRunner> runner_;
};

} // namespace cipher

#endif // CIPHER_OPENCLAW_BACKEND_HPP
