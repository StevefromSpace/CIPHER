#ifndef CIPHER_OLLAMA_BACKEND_HPP
#define CIPHER_OLLAMA_BACKEND_HPP

#include "cipher/assistant_backend.hpp"
#include "cipher/loopback_http.hpp"

#include <functional>
#include <string>

namespace cipher {

/**
 * @brief Connection settings for a local Ollama server.
 */
struct OllamaConfig {
    std::string host{"127.0.0.1"};
    int port{11434};
    std::string model{"qwen2.5:3b"};
};

/** Loopback HTTP call signature; injectable so tests can replace the socket layer. */
using HttpTransport = std::function<HttpResult(
    int port,
    const std::string& method,
    const std::string& path,
    const std::string& body,
    double timeout_seconds
)>;

/**
 * @brief Assistant backend that talks to Ollama's HTTP API on loopback.
 *
 * Only the literal IPv4 loopback address is accepted as host. Any other host fails
 * closed: the backend is unavailable and reports ProcessingRoute::Unknown.
 */
class OllamaBackend : public IAssistantBackend {
public:
    explicit OllamaBackend(OllamaConfig config, HttpTransport transport = loopback_http_request);

    std::string backend_id() const override { return "ollama"; }
    bool is_available() override;
    AssistantCapabilities capabilities() const override;
    AssistantResponse execute_request(const AssistantRequest& request) override;

    static bool is_loopback_host(const std::string& host);

private:
    OllamaConfig config_;
    HttpTransport transport_;
    bool endpoint_verified_;
};

} // namespace cipher

#endif // CIPHER_OLLAMA_BACKEND_HPP
