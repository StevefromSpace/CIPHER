#include "cipher/openclaw_backend.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <utility>

namespace cipher {

namespace {

constexpr const char* kLocalModelPrefix = "ollama/";
constexpr int kProbeTimeoutMs = 10000;

AssistantResponse failure(AssistantStatus status, std::string message) {
    AssistantResponse resp;
    resp.status = status;
    resp.error = std::move(message);
    return resp;
}

// OpenClaw prints a JSON object; the reply text has appeared as reply, text, result.text or payloads[0].text.
std::optional<std::string> reply_text(const nlohmann::json& data) {
    if (!data.is_object()) return std::nullopt;
    for (const char* key : {"reply", "text"}) {
        const auto it = data.find(key);
        if (it != data.end() && it->is_string()) return it->get<std::string>();
    }
    if (const auto r = data.find("result"); r != data.end() && r->is_object()) {
        const auto t = r->find("text");
        if (t != r->end() && t->is_string()) return t->get<std::string>();
    }
    if (const auto p = data.find("payloads"); p != data.end() && p->is_array() && !p->empty()) {
        const auto& first = (*p)[0];
        if (first.is_object()) {
            const auto t = first.find("text");
            if (t != first.end() && t->is_string()) return t->get<std::string>();
        }
    }
    return std::nullopt;
}

} // namespace

OpenClawBackend::OpenClawBackend(OpenClawConfig config, std::shared_ptr<orchestration::ProcessRunner> runner)
    : config_(std::move(config)), runner_(std::move(runner)) {}

bool OpenClawBackend::is_available() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path cli(config_.cli_path);
    return !config_.cli_path.empty() && cli.is_absolute() && fs::is_regular_file(cli, ec) && !ec;
}

AssistantCapabilities OpenClawBackend::capabilities() const {
    AssistantCapabilities caps;
    caps.supports_text_conversation = true;
    caps.supports_sessions = true;
    // OpenClaw's own routing cannot be proven from here; only the ollama/ model prefix is enforced.
    caps.processing_route = ProcessingRoute::Unknown;
    return caps;
}

std::string OpenClawBackend::tool_inventory_problem(int timeout_ms) {
    const auto probe = runner_->run({config_.cli_path, "tools", "list", "--json"}, timeout_ms);
    if (probe.timed_out || probe.output_overflow || probe.exit_code != 0) {
        return "tool inventory probe failed";
    }
    const nlohmann::json data = nlohmann::json::parse(probe.stdout_text, nullptr, false);
    const nlohmann::json* tools = nullptr;
    if (data.is_array()) {
        tools = &data;
    } else if (data.is_object()) {
        const auto it = data.find("tools");
        if (it != data.end() && it->is_array()) tools = &*it;
    }
    if (tools == nullptr) return "tool inventory output was not understood";
    if (!tools->empty()) return "OpenClaw exposes tools; CIPHER refuses to run it with any enabled";
    return {};
}

AssistantResponse OpenClawBackend::execute_request(const AssistantRequest& request) {
    if (!is_available()) {
        return failure(AssistantStatus::Unavailable, "OpenClaw CLI is not configured with an existing absolute path.");
    }
    const std::string model = request.model_override.value_or(config_.model);
    if (model.rfind(kLocalModelPrefix, 0) != 0) {
        return failure(AssistantStatus::PolicyViolation, "OpenClaw model must start with 'ollama/' (local only).");
    }
    if (request.request_tools) {
        return failure(AssistantStatus::PolicyViolation, "OpenClaw is run without tools; tool requests are refused.");
    }

    const auto started = std::chrono::steady_clock::now();
    const int timeout_ms = std::max(1, static_cast<int>(request.timeout_seconds * 1000.0));

    if (const std::string problem = tool_inventory_problem(kProbeTimeoutMs); !problem.empty()) {
        return failure(AssistantStatus::Unavailable, "Tool isolation blocker: " + problem);
    }

    std::vector<std::string> argv{config_.cli_path, "agent", "--message", request.prompt, "--model", model,
                                  "--timeout", std::to_string(static_cast<int>(request.timeout_seconds)),
                                  "--profile", config_.profile, "--json"};
    if (request.session_id && !request.session_id->empty()) {
        argv.push_back("--session-id");
        argv.push_back(*request.session_id);
    }
    const auto run = runner_->run(argv, timeout_ms);

    AssistantResponse resp;
    resp.model = model;
    resp.route = capabilities().processing_route;
    resp.duration_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (run.timed_out) { resp.status = AssistantStatus::Timeout; resp.error = "OpenClaw timed out."; return resp; }
    if (run.output_overflow) { resp.status = AssistantStatus::Error; resp.error = "OpenClaw output exceeded the size limit."; return resp; }
    if (run.exit_code != 0) { resp.status = AssistantStatus::Error; resp.error = "OpenClaw exited with code " + std::to_string(run.exit_code) + "."; return resp; }

    const nlohmann::json data = nlohmann::json::parse(run.stdout_text, nullptr, false);
    const auto text = reply_text(data);
    if (!text) { resp.status = AssistantStatus::Error; resp.error = "OpenClaw output had no reply text."; return resp; }
    resp.text = *text;
    resp.status = AssistantStatus::Ok;
    return resp;
}

} // namespace cipher
