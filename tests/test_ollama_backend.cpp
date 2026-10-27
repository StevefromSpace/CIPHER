#include "cipher/assistant_backend.hpp"
#include "cipher/loopback_http.hpp"
#include "cipher/ollama_backend.hpp"

#include <functional>
#include <iostream>
#include <string>

namespace {

int g_failures = 0;
int g_passes   = 0;

void pass(const std::string& name) {
    std::cout << "PASS: " << name << '\n';
    ++g_passes;
}

void fail(const std::string& name, const std::string& reason) {
    std::cerr << "FAIL: " << name << "\n  " << reason << '\n';
    ++g_failures;
}

void check(bool condition, const std::string& name, const std::string& reason) {
    if (condition) pass(name);
    else           fail(name, reason);
}

// Scripted HTTP boundary: returns canned results and records every call.
struct FakeTransport {
    cipher::HttpResult version_result;
    cipher::HttpResult generate_result;
    int calls{0};
    std::string last_method;
    std::string last_path;
    std::string last_body;
    int last_port{0};

    cipher::HttpResult operator()(int port, const std::string& method, const std::string& path,
                                  const std::string& body, double /*timeout_seconds*/) {
        ++calls;
        last_port = port;
        last_method = method;
        last_path = path;
        last_body = body;
        return path == "/api/version" ? version_result : generate_result;
    }
};

cipher::HttpResult ok_http(int status, const std::string& body) {
    cipher::HttpResult result;
    result.outcome = cipher::HttpOutcome::Ok;
    result.status_code = status;
    result.body = body;
    return result;
}

cipher::HttpResult failed_http(cipher::HttpOutcome outcome) {
    cipher::HttpResult result;
    result.outcome = outcome;
    result.detail = "scripted";
    return result;
}

cipher::AssistantRequest make_request(const std::string& prompt) {
    cipher::AssistantRequest request;
    request.prompt = prompt;
    return request;
}

cipher::OllamaConfig default_config() {
    return cipher::OllamaConfig{};
}

const std::string kGenerateOk = R"({"model":"qwen2.5:3b","response":"Hello there","done":true})";

// ---------------------------------------------------------------------------
// Endpoint verification
// ---------------------------------------------------------------------------
void test_loopback_host_accepts_only_ipv4_literal() {
    check(cipher::OllamaBackend::is_loopback_host("127.0.0.1"),
          "loopback host: 127.0.0.1 accepted", "127.0.0.1 was rejected");
    check(!cipher::OllamaBackend::is_loopback_host("localhost"),
          "loopback host: localhost refused (hosts-file redirection)", "localhost was accepted");
    check(!cipher::OllamaBackend::is_loopback_host("0.0.0.0"),
          "loopback host: 0.0.0.0 rejected", "0.0.0.0 was accepted");
    check(!cipher::OllamaBackend::is_loopback_host("192.168.1.10"),
          "loopback host: LAN address rejected", "LAN address was accepted");
}

void test_valid_loopback_endpoint_reports_verified_local() {
    FakeTransport transport{ok_http(200, "{}"), failed_http(cipher::HttpOutcome::ConnectFailed)};
    cipher::OllamaBackend backend(default_config(), std::ref(transport));

    check(backend.capabilities().processing_route == cipher::ProcessingRoute::VerifiedLocal,
          "loopback endpoint claims VerifiedLocal", "route was not VerifiedLocal");
}

void test_non_loopback_endpoint_fails_closed() {
    FakeTransport transport{ok_http(200, "{}"), ok_http(200, kGenerateOk)};
    cipher::OllamaConfig config = default_config();
    config.host = "192.168.1.10";
    cipher::OllamaBackend backend(config, std::ref(transport));

    check(backend.capabilities().processing_route == cipher::ProcessingRoute::Unknown,
          "non-loopback endpoint reports Unknown route", "route was not Unknown");
    check(!backend.is_available(), "non-loopback endpoint is unavailable", "is_available returned true");

    const cipher::AssistantResponse resp = backend.execute_request(make_request("hi"));
    check(resp.status == cipher::AssistantStatus::Unavailable,
          "non-loopback execute returns Unavailable", "status was " + cipher::to_string(resp.status));
    check(transport.calls == 0, "non-loopback endpoint never reaches the transport",
          "transport was called " + std::to_string(transport.calls) + " time(s)");
}

void test_invalid_port_fails_closed() {
    FakeTransport transport{ok_http(200, "{}"), ok_http(200, kGenerateOk)};
    cipher::OllamaConfig config = default_config();
    config.port = 70000;
    cipher::OllamaBackend backend(config, std::ref(transport));

    check(backend.capabilities().processing_route == cipher::ProcessingRoute::Unknown,
          "out-of-range port reports Unknown route", "route was not Unknown");
}

// ---------------------------------------------------------------------------
// Central validation and VerifiedLocal enforcement
// ---------------------------------------------------------------------------
void test_local_only_request_passes_through_manager_on_loopback() {
    FakeTransport transport{ok_http(200, "{}"), ok_http(200, kGenerateOk)};
    cipher::OllamaBackend backend(default_config(), std::ref(transport));

    cipher::AssistantRequest request = make_request("Say hello");
    request.local_only = true;
    const cipher::AssistantResponse resp = cipher::AssistantManager::submit(&backend, request);

    check(resp.is_ok(), "local_only request succeeds through AssistantManager on loopback",
          "status was " + cipher::to_string(resp.status) + ": " + resp.error.value_or(""));
    check(resp.route == cipher::ProcessingRoute::VerifiedLocal,
          "successful response carries VerifiedLocal route", "route was not VerifiedLocal");
}

void test_tool_request_rejected_before_transport() {
    FakeTransport transport{ok_http(200, "{}"), ok_http(200, kGenerateOk)};
    cipher::OllamaBackend backend(default_config(), std::ref(transport));

    cipher::AssistantRequest request = make_request("Run a tool");
    request.request_tools = true;
    const cipher::AssistantResponse resp = cipher::AssistantManager::submit(&backend, request);

    check(resp.status == cipher::AssistantStatus::PolicyViolation,
          "request_tools rejected by central validation", "status was " + cipher::to_string(resp.status));
    check(transport.calls == 1, "rejected request only performs the availability probe",
          "transport was called " + std::to_string(transport.calls) + " time(s)");
}

// ---------------------------------------------------------------------------
// Availability and failure mapping
// ---------------------------------------------------------------------------
void test_unavailable_server_fails_closed() {
    FakeTransport transport{failed_http(cipher::HttpOutcome::ConnectFailed), ok_http(200, kGenerateOk)};
    cipher::OllamaBackend backend(default_config(), std::ref(transport));

    check(!backend.is_available(), "server down: is_available is false", "is_available returned true");
    const cipher::AssistantResponse resp = cipher::AssistantManager::submit(&backend, make_request("hi"));
    check(resp.status == cipher::AssistantStatus::Unavailable,
          "server down: manager returns Unavailable", "status was " + cipher::to_string(resp.status));
    check(transport.last_path != "/api/generate", "server down: no generate request is sent",
          "generate request was sent");
}

void test_connection_failure_maps_to_connection_error() {
    FakeTransport transport{ok_http(200, "{}"), failed_http(cipher::HttpOutcome::ConnectFailed)};
    cipher::OllamaBackend backend(default_config(), std::ref(transport));

    const cipher::AssistantResponse resp = backend.execute_request(make_request("hi"));
    check(resp.status == cipher::AssistantStatus::ConnectionError,
          "connect failure maps to ConnectionError", "status was " + cipher::to_string(resp.status));
    check(resp.error.has_value(), "connect failure carries an error message", "error was empty");
}

void test_timeout_maps_to_timeout() {
    FakeTransport transport{ok_http(200, "{}"), failed_http(cipher::HttpOutcome::TimedOut)};
    cipher::OllamaBackend backend(default_config(), std::ref(transport));

    const cipher::AssistantResponse resp = backend.execute_request(make_request("hi"));
    check(resp.status == cipher::AssistantStatus::Timeout,
          "timeout maps to Timeout", "status was " + cipher::to_string(resp.status));
}

void test_malformed_transport_maps_to_error() {
    FakeTransport transport{ok_http(200, "{}"), failed_http(cipher::HttpOutcome::MalformedResponse)};
    cipher::OllamaBackend backend(default_config(), std::ref(transport));

    const cipher::AssistantResponse resp = backend.execute_request(make_request("hi"));
    check(resp.status == cipher::AssistantStatus::Error,
          "malformed HTTP maps to Error", "status was " + cipher::to_string(resp.status));
}

void test_server_error_body_maps_to_error() {
    FakeTransport transport{ok_http(200, "{}"),
                            ok_http(404, R"({"error":"model 'nope' not found"})")};
    cipher::OllamaBackend backend(default_config(), std::ref(transport));

    const cipher::AssistantResponse resp = backend.execute_request(make_request("hi"));
    check(resp.status == cipher::AssistantStatus::Error, "server error body maps to Error",
          "status was " + cipher::to_string(resp.status));
    check(resp.error.value_or("").find("not found") != std::string::npos,
          "server error message is surfaced", "message was: " + resp.error.value_or(""));
}

void test_missing_response_field_is_malformed() {
    FakeTransport transport{ok_http(200, "{}"), ok_http(200, R"({"model":"x","done":true})")};
    cipher::OllamaBackend backend(default_config(), std::ref(transport));

    const cipher::AssistantResponse resp = backend.execute_request(make_request("hi"));
    check(resp.status == cipher::AssistantStatus::Error,
          "response without 'response' field is Error", "status was " + cipher::to_string(resp.status));
}

void test_non_json_body_is_malformed() {
    FakeTransport transport{ok_http(200, "{}"), ok_http(200, "<html>proxy error</html>")};
    cipher::OllamaBackend backend(default_config(), std::ref(transport));

    const cipher::AssistantResponse resp = backend.execute_request(make_request("hi"));
    check(resp.status == cipher::AssistantStatus::Error,
          "non-JSON body is Error", "status was " + cipher::to_string(resp.status));
}

// ---------------------------------------------------------------------------
// Request/response content
// ---------------------------------------------------------------------------
void test_success_path_builds_request_and_decodes_response() {
    FakeTransport transport{ok_http(200, "{}"),
                            ok_http(200, R"({"response":"Caf\u00e9 \"ok\"\nline2","done":true})")};
    cipher::OllamaBackend backend(default_config(), std::ref(transport));

    const cipher::AssistantResponse resp =
        cipher::AssistantManager::submit(&backend, make_request("say \"hi\"\n"));

    check(resp.is_ok(), "success path returns Ok", "status was " + cipher::to_string(resp.status));
    check(resp.text == "Caf\xC3\xA9 \"ok\"\nline2", "response JSON escapes are decoded",
          "text was: " + resp.text);
    check(transport.last_method == "POST" && transport.last_path == "/api/generate",
          "generate uses POST /api/generate", "method/path was " + transport.last_method + " " + transport.last_path);
    check(transport.last_body.find("\"prompt\":\"say \\\"hi\\\"\\n\"") != std::string::npos,
          "prompt is JSON-escaped in the request body", "body was: " + transport.last_body);
    check(transport.last_body.find("\"stream\":false") != std::string::npos,
          "request disables streaming", "body was: " + transport.last_body);
    check(transport.last_port == 11434, "request uses configured port", "port was " + std::to_string(transport.last_port));
}

void test_model_override_is_used() {
    FakeTransport transport{ok_http(200, "{}"), ok_http(200, kGenerateOk)};
    cipher::OllamaBackend backend(default_config(), std::ref(transport));

    cipher::AssistantRequest request = make_request("hi");
    request.model_override = "llama3.2:1b";
    const cipher::AssistantResponse resp = backend.execute_request(request);

    check(resp.model == "llama3.2:1b", "model override is reported", "model was " + resp.model);
    check(transport.last_body.find("\"model\":\"llama3.2:1b\"") != std::string::npos,
          "model override is sent to Ollama", "body was: " + transport.last_body);
}

// ---------------------------------------------------------------------------
// HTTP response parser
// ---------------------------------------------------------------------------
void test_parser_reads_content_length_body() {
    const cipher::HttpResult result = cipher::parse_http_response(
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    check(result.outcome == cipher::HttpOutcome::Ok && result.status_code == 200 && result.body == "ok",
          "parser reads Content-Length body", "outcome/body mismatch");
}

void test_parser_decodes_chunked_body() {
    const cipher::HttpResult result = cipher::parse_http_response(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n");
    check(result.outcome == cipher::HttpOutcome::Ok && result.body == "Wikipedia",
          "parser decodes chunked body", "body was: " + result.body);
}

void test_parser_rejects_bad_framing() {
    const cipher::HttpResult no_status = cipher::parse_http_response("garbage\r\n\r\nbody");
    check(no_status.outcome == cipher::HttpOutcome::MalformedResponse,
          "parser rejects non-HTTP status line", "outcome was not MalformedResponse");

    const cipher::HttpResult length_mismatch = cipher::parse_http_response(
        "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort");
    check(length_mismatch.outcome == cipher::HttpOutcome::MalformedResponse,
          "parser rejects Content-Length mismatch", "outcome was not MalformedResponse");

    const cipher::HttpResult oversized_chunk = cipher::parse_http_response(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFF\r\nx\r\n");
    check(oversized_chunk.outcome == cipher::HttpOutcome::MalformedResponse,
          "parser rejects chunk size larger than the body", "outcome was not MalformedResponse");

    const cipher::HttpResult no_terminator = cipher::parse_http_response("HTTP/1.1 200 OK\r\n");
    check(no_terminator.outcome == cipher::HttpOutcome::MalformedResponse,
          "parser rejects missing header terminator", "outcome was not MalformedResponse");
}

// ---------------------------------------------------------------------------
// Real socket boundary: nothing listens on this port, so the connect must fail.
// ---------------------------------------------------------------------------
void test_real_loopback_refused_connection() {
    const cipher::HttpResult result = cipher::loopback_http_request(1, "GET", "/", "", 2.0);
    check(result.outcome == cipher::HttpOutcome::ConnectFailed,
          "real loopback connect to closed port reports ConnectFailed",
          "outcome was " + std::to_string(static_cast<int>(result.outcome)));
}

} // namespace

int main() {
    test_loopback_host_accepts_only_ipv4_literal();
    test_valid_loopback_endpoint_reports_verified_local();
    test_non_loopback_endpoint_fails_closed();
    test_invalid_port_fails_closed();
    test_local_only_request_passes_through_manager_on_loopback();
    test_tool_request_rejected_before_transport();
    test_unavailable_server_fails_closed();
    test_connection_failure_maps_to_connection_error();
    test_timeout_maps_to_timeout();
    test_malformed_transport_maps_to_error();
    test_server_error_body_maps_to_error();
    test_missing_response_field_is_malformed();
    test_non_json_body_is_malformed();
    test_success_path_builds_request_and_decodes_response();
    test_model_override_is_used();
    test_parser_reads_content_length_body();
    test_parser_decodes_chunked_body();
    test_parser_rejects_bad_framing();
    test_real_loopback_refused_connection();

    std::cout << "\n" << g_passes << " passed, " << g_failures << " failed\n";
    return g_failures == 0 ? 0 : 1;
}
