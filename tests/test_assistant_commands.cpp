// ---------------------------------------------------------------------------
// tests/test_assistant_commands.cpp  —  REPL command tests for `ask` and `ai`
//
// Uses a scripted process runner so no CLI is started. The no-fallback rule is
// verified by counting invocations: an unavailable provider must not cause any
// other provider to be contacted.
// ---------------------------------------------------------------------------

#include "cipher/assistant_commands.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_passes = 0;
int g_failures = 0;

void check(bool condition, const std::string& name, const std::string& reason) {
    if (condition) {
        std::cout << "PASS: " << name << '\n';
        ++g_passes;
    } else {
        std::cerr << "FAIL: " << name << "\n  " << reason << '\n';
        ++g_failures;
    }
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

class ScriptedRunner : public cipher::IProcessRunner {
public:
    cipher::ProcessResult run(const cipher::ProcessRequest& request) override {
        calls.push_back(request);
        if (replies.empty()) return {};
        cipher::ProcessResult next = replies.front();
        replies.erase(replies.begin());
        return next;
    }

    void queue(cipher::ProcessResult result) { replies.push_back(std::move(result)); }

    std::vector<cipher::ProcessRequest> calls;
    std::vector<cipher::ProcessResult> replies;
};

cipher::ProcessResult started(int exit_code, const std::string& stdout_text) {
    cipher::ProcessResult result;
    result.started = true;
    result.exit_code = exit_code;
    result.stdout_text = stdout_text;
    return result;
}

const std::string CLAUDE_PATH = "/usr/bin/claude";
const std::string LOGGED_IN_JSON = "{\"loggedIn\": true, \"authMethod\": \"claude.ai\"}";
const std::string LOGGED_OUT_JSON = "{\"loggedIn\": false}";

void test_ai_list_distinguishes_provider_states() {
    ScriptedRunner runner;
    runner.queue(started(0, LOGGED_IN_JSON));
    cipher::AssistantCommands commands(runner, CLAUDE_PATH);

    const auto reply = commands.execute("ai list");
    check(reply.handled, "ai list is handled", "not handled");
    check(contains(reply.text, "native  not implemented"), "ai list marks the native runtime not implemented",
          reply.text);
    check(contains(reply.text, "claude  available"), "ai list reports an authenticated Claude as available",
          reply.text);
    check(contains(reply.text, "hermes  unavailable"), "ai list reports Hermes as unavailable", reply.text);
    check(contains(reply.text, "route: remote_cloud") && contains(reply.text, "route: unknown"),
          "ai list shows the route for each provider", reply.text);
}

void test_ai_list_reports_unauthenticated_claude() {
    ScriptedRunner runner;
    runner.queue(started(0, LOGGED_OUT_JSON));
    cipher::AssistantCommands commands(runner, CLAUDE_PATH);
    const auto reply = commands.execute("ai list");
    check(contains(reply.text, "claude  unavailable") && contains(reply.text, "not logged in"),
          "ai list reports a logged-out Claude as unavailable with guidance", reply.text);
}

void test_ai_usage_errors() {
    ScriptedRunner runner;
    cipher::AssistantCommands commands(runner, CLAUDE_PATH);
    check(commands.execute("ai").text == "Usage: ai list", "bare ai prints usage", "unexpected text");
    check(commands.execute("ai status").text == "Usage: ai list", "unknown ai subcommand prints usage",
          "unexpected text");
    check(runner.calls.empty(), "ai usage errors start no process", "a process was started");
}

void test_ask_argument_errors_send_nothing() {
    ScriptedRunner runner;
    cipher::AssistantCommands commands(runner, CLAUDE_PATH);

    check(contains(commands.execute("ask").text, "Usage: ask"), "bare ask prints usage", "no usage");
    check(contains(commands.execute("ask claude").text, "Usage: ask"),
          "ask without a prompt prints usage", "no usage");
    check(contains(commands.execute("ask claude    ").text, "Usage: ask"),
          "whitespace-only prompt is treated as missing", "no usage");
    check(contains(commands.execute("ask bogus hello").text, "Unknown assistant provider 'bogus'"),
          "unknown provider is rejected with a clear error", "no error");
    check(contains(commands.execute("ask native hello").text, "not implemented"),
          "ask native reports the runtime is not implemented", "no error");
    check(runner.calls.empty(), "argument errors never start a process", "a process was started");
}

void test_ask_hermes_fails_closed_without_fallback() {
    ScriptedRunner runner;
    runner.queue(started(0, LOGGED_IN_JSON));
    cipher::AssistantCommands commands(runner, CLAUDE_PATH);

    const auto reply = commands.execute("ask hermes summarize this");
    check(contains(reply.text, "ask hermes") && contains(reply.text, "failed (unavailable)"),
          "ask hermes reports Hermes as unavailable", reply.text);
    check(contains(reply.text, "No other provider was tried."), "ask hermes states that nothing fell back",
          reply.text);
    check(runner.calls.empty(), "ask hermes never contacts Claude or any other process", "a process was started");
}

void test_ask_claude_success_discloses_cloud_route() {
    ScriptedRunner runner;
    runner.queue(started(0, LOGGED_IN_JSON));
    runner.queue(started(0, "Paris.\n"));
    cipher::AssistantCommands commands(runner, CLAUDE_PATH);

    const auto reply = commands.execute("ask claude capital of France?");
    check(reply.handled, "ask claude is handled", "not handled");
    check(contains(reply.text, "route: remote_cloud"), "successful ask discloses the cloud route", reply.text);
    check(contains(reply.text, "Tools and MCP servers are disabled"), "successful ask states tools are disabled",
          reply.text);
    check(contains(reply.text, "Paris."), "successful ask shows the response", reply.text);
    check(runner.calls.size() == 2 && runner.calls[1].stdin_text == "capital of France?",
          "the prompt text, and only that, is sent over stdin", "invocations: " + std::to_string(runner.calls.size()));
}

void test_ask_claude_unauthenticated_reports_login_guidance() {
    ScriptedRunner runner;
    runner.queue(started(0, LOGGED_OUT_JSON));
    cipher::AssistantCommands commands(runner, CLAUDE_PATH);

    const auto reply = commands.execute("ask claude hello");
    check(contains(reply.text, "failed (unavailable)") && contains(reply.text, "claude auth login"),
          "unauthenticated ask reports login guidance", reply.text);
    check(runner.calls.size() == 1, "unauthenticated ask sends no prompt", "invocations: " + std::to_string(runner.calls.size()));
}

void test_non_assistant_input_is_not_handled() {
    ScriptedRunner runner;
    cipher::AssistantCommands commands(runner, CLAUDE_PATH);
    check(!commands.execute("status").handled, "core commands pass through", "handled");
    check(!commands.execute("remember x y").handled, "memory commands pass through", "handled");
    check(!commands.execute("").handled, "empty input passes through", "handled");
    const auto help = commands.execute("help");
    check(!help.handled && contains(help.text, "ask claude"), "help adds assistant usage without replacing core help",
          help.text);
}

} // anonymous namespace

int main() {
    std::cout << "Running CIPHER Assistant Command Test Suite...\n\n";

    test_ai_list_distinguishes_provider_states();
    test_ai_list_reports_unauthenticated_claude();
    test_ai_usage_errors();
    test_ask_argument_errors_send_nothing();
    test_ask_hermes_fails_closed_without_fallback();
    test_ask_claude_success_discloses_cloud_route();
    test_ask_claude_unauthenticated_reports_login_guidance();
    test_non_assistant_input_is_not_handled();

    std::cout << "\n-------------------------------------------------------------\n";
    std::cout << "Tests passed: " << g_passes << '\n';
    std::cout << "Tests failed: " << g_failures << '\n';
    std::cout << "-------------------------------------------------------------\n";

    return (g_failures == 0) ? 0 : 1;
}
