#include "cipher/ask_commands.hpp"

#include <iostream>
#include <string>

// ---------------------------------------------------------------------------
// test_ask_commands.cpp  —  owner-typed `ask` command (v0.5 local backend)
//
// A scripted backend stands in for Ollama, so these tests need no server.
// They check routing of the REPL line, the local_only and no-tools request
// shape, the fail-closed route check, and the printed failure messages.
// ---------------------------------------------------------------------------

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

class ScriptedBackend : public cipher::IAssistantBackend {
public:
    cipher::ProcessingRoute route = cipher::ProcessingRoute::VerifiedLocal;
    cipher::AssistantResponse reply;
    int dispatches = 0;
    cipher::AssistantRequest last_request;

    std::string backend_id() const override { return "scripted_backend"; }
    bool is_available() override { return true; }
    cipher::AssistantCapabilities capabilities() const override {
        cipher::AssistantCapabilities caps;
        caps.processing_route = route;
        return caps;
    }
    cipher::AssistantResponse execute_request(const cipher::AssistantRequest& request) override {
        ++dispatches;
        last_request = request;
        return reply;
    }
};

cipher::AssistantResponse ok_reply(const std::string& text) {
    cipher::AssistantResponse resp;
    resp.status = cipher::AssistantStatus::Ok;
    resp.text = text;
    return resp;
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

void test_other_lines_are_not_handled() {
    cipher::AskCommands ask(nullptr);
    check(!ask.execute("memory list").handled, "non-ask line: not handled",
          "memory list was handled");
    check(!ask.execute("asking please").handled, "word starting with ask: not handled",
          "'asking please' was handled");
}

void test_disabled_backend_says_how_to_enable() {
    cipher::AskCommands ask(nullptr);
    const cipher::AskCommandResult result = ask.execute("ask hello");
    check(result.handled && result.text.find("ollama_port") != std::string::npos,
          "disabled: ask explains ollama_port",
          "text was: " + result.text);
}

void test_bare_ask_prints_usage_without_dispatch() {
    ScriptedBackend backend;
    cipher::AskCommands ask(&backend);
    const cipher::AskCommandResult result = ask.execute("  ask   ");
    check(result.handled && result.text == "Usage: ask <question>",
          "bare ask: prints usage", "text was: " + result.text);
    check(backend.dispatches == 0, "bare ask: no request dispatched",
          "dispatches was " + std::to_string(backend.dispatches));
}

void test_success_sends_local_only_request_without_tools() {
    ScriptedBackend backend;
    backend.reply = ok_reply("pong");
    cipher::AskCommands ask(&backend, 45.0);

    const cipher::AskCommandResult result = ask.execute("ask say pong");
    check(result.handled && result.text == "pong", "success: prints model text",
          "text was: " + result.text);
    check(backend.dispatches == 1, "success: one dispatch", "dispatches was " + std::to_string(backend.dispatches));
    check(backend.last_request.prompt == "say pong", "success: prompt is the typed text only",
          "prompt was: " + backend.last_request.prompt);
    check(backend.last_request.local_only, "success: request is local_only",
          "local_only was false");
    check(!backend.last_request.request_tools, "success: tools are never requested",
          "request_tools was true");
    check(backend.last_request.timeout_seconds == 45.0, "success: timeout is passed through",
          "timeout was " + std::to_string(backend.last_request.timeout_seconds));
}

void test_non_local_route_fails_closed_before_dispatch() {
    ScriptedBackend backend;
    backend.route = cipher::ProcessingRoute::Unknown;
    backend.reply = ok_reply("should never be shown");
    cipher::AskCommands ask(&backend);

    const cipher::AskCommandResult result = ask.execute("ask hello");
    check(backend.dispatches == 0, "non-local route: central validation blocks dispatch",
          "dispatches was " + std::to_string(backend.dispatches));
    check(starts_with(result.text, "ask failed ("), "non-local route: failure is printed",
          "text was: " + result.text);
    check(result.text.find("should never be shown") == std::string::npos,
          "non-local route: backend text is withheld", "backend text leaked");
}

void test_backend_error_is_printed_with_status() {
    ScriptedBackend backend;
    backend.reply.status = cipher::AssistantStatus::ConnectionError;
    backend.reply.error = std::string("connection refused");
    cipher::AskCommands ask(&backend);

    const cipher::AskCommandResult result = ask.execute("ask hello");
    check(result.text == "ask failed (" + cipher::to_string(cipher::AssistantStatus::ConnectionError) +
          "): connection refused",
          "backend error: status and message are printed",
          "text was: " + result.text);
}

} // namespace

int main() {
    test_other_lines_are_not_handled();
    test_disabled_backend_says_how_to_enable();
    test_bare_ask_prints_usage_without_dispatch();
    test_success_sends_local_only_request_without_tools();
    test_non_local_route_fails_closed_before_dispatch();
    test_backend_error_is_printed_with_status();

    std::cout << "\n" << g_passes << " passed, " << g_failures << " failed\n";
    return g_failures == 0 ? 0 : 1;
}
