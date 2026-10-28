#include "cipher/ollama_backend.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <utility>

namespace cipher {

namespace {

constexpr const char* kLoopbackHost = "127.0.0.1";
constexpr int kMaxPort = 65535;
constexpr double kAvailabilityProbeSeconds = 2.0;
constexpr const char* kVersionPath = "/api/version";
constexpr const char* kGeneratePath = "/api/generate";

// Returns the string field `key` of a top-level JSON object, or nullopt if the body is not an
// object or the field is absent or not a string.
std::optional<std::string> top_level_string(const nlohmann::json& body, const char* key) {
    if (!body.is_object()) {
        return std::nullopt;
    }
    const auto it = body.find(key);
    if (it == body.end() || !it->is_string()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

std::string build_generate_body(const std::string& model, const std::string& prompt) {
    const nlohmann::json body{{"model", model}, {"prompt", prompt}, {"stream", false}};
    // Invalid UTF-8 in the prompt is replaced rather than thrown on.
    return body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

AssistantResponse make_failure_response(AssistantStatus status, const std::string& message) {
    AssistantResponse resp;
    resp.status = status;
    resp.error = message;
    return resp;
}

AssistantStatus status_for_transport(HttpOutcome outcome) {
    switch (outcome) {
        case HttpOutcome::ConnectFailed: return AssistantStatus::ConnectionError;
        case HttpOutcome::TimedOut: return AssistantStatus::Timeout;
        case HttpOutcome::TransportFailed: return AssistantStatus::ConnectionError;
        case HttpOutcome::MalformedResponse:
        case HttpOutcome::ResponseTooLarge:
        case HttpOutcome::Ok:
            return AssistantStatus::Error;
    }
    return AssistantStatus::Error;
}

} // anonymous namespace

OllamaBackend::OllamaBackend(OllamaConfig config, HttpTransport transport)
    : config_(std::move(config)),
      transport_(std::move(transport)),
      endpoint_verified_(is_loopback_host(config_.host) && config_.port >= 1 && config_.port <= kMaxPort) {}

bool OllamaBackend::is_loopback_host(const std::string& host) {
    // Only the literal address is accepted. "localhost" is refused because its resolution can be redirected by the hosts file.
    return host == kLoopbackHost;
}

bool OllamaBackend::is_available() {
    if (!endpoint_verified_) {
        return false;
    }
    const HttpResult probe = transport_(config_.port, "GET", kVersionPath, "", kAvailabilityProbeSeconds);
    return probe.outcome == HttpOutcome::Ok && probe.status_code == 200;
}

AssistantCapabilities OllamaBackend::capabilities() const {
    AssistantCapabilities caps;
    caps.supports_text_conversation = true;
    // Verified means the destination address is the loopback literal. It does not prove the listener is Ollama.
    caps.processing_route = endpoint_verified_ ? ProcessingRoute::VerifiedLocal : ProcessingRoute::Unknown;
    return caps;
}

AssistantResponse OllamaBackend::execute_request(const AssistantRequest& request) {
    if (!endpoint_verified_) {
        return make_failure_response(AssistantStatus::Unavailable,
                                     "Ollama endpoint is not verified loopback; refusing to send the request.");
    }

    const auto started = std::chrono::steady_clock::now();
    const std::string model = request.model_override.value_or(config_.model);
    const HttpResult http = transport_(config_.port, "POST", kGeneratePath,
                                       build_generate_body(model, request.prompt), request.timeout_seconds);

    AssistantResponse resp;
    resp.model = model;
    resp.route = capabilities().processing_route;
    resp.duration_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

    if (http.outcome != HttpOutcome::Ok) {
        resp.status = status_for_transport(http.outcome);
        resp.error = "Ollama request failed: " + http.detail;
        return resp;
    }

    // Non-throwing parse: a malformed body yields a discarded value, which has no fields.
    const nlohmann::json body = nlohmann::json::parse(http.body, nullptr, false);
    if (const auto error_text = top_level_string(body, "error")) {
        resp.status = AssistantStatus::Error;
        resp.error = "Ollama returned HTTP " + std::to_string(http.status_code) + ": " + *error_text;
        return resp;
    }
    if (http.status_code != 200) {
        resp.status = AssistantStatus::Error;
        resp.error = "Ollama returned HTTP " + std::to_string(http.status_code) + ".";
        return resp;
    }

    const auto text = top_level_string(body, "response");
    if (!text) {
        resp.status = AssistantStatus::Error;
        resp.error = "Ollama response is missing the 'response' field.";
        return resp;
    }
    resp.text = *text;

    resp.status = AssistantStatus::Ok;
    return resp;
}

} // namespace cipher
