// ---------------------------------------------------------------------------
// tests/test_assistant_cli.cpp  —  Claude Code and Hermes adapter tests
//
// Most cases use a scripted IProcessRunner, so the exact invocation contract
// is checked without spawning anything. The end-to-end cases run the real
// runner against the deterministic fake CLI fixture.
// ---------------------------------------------------------------------------

#include "cipher/assistant_cli.hpp"
#include "cipher/assistant_backend.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <stdlib.h> // _putenv_s on Windows, setenv elsewhere

#ifndef CIPHER_FAKE_CLI_PATH
#error "CIPHER_FAKE_CLI_PATH must be defined by the build"
#endif

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

// Replays queued results in order and records every request it receives.
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

cipher::ProcessResult ok_result(const std::string& stdout_text) {
    cipher::ProcessResult result;
    result.started = true;
    result.exit_code = 0;
    result.stdout_text = stdout_text;
    return result;
}

const std::string LOGGED_IN_JSON = "{\n  \"loggedIn\": true,\n  \"authMethod\": \"claude.ai\"\n}\n";
const std::string LOGGED_OUT_JSON = "{\n  \"loggedIn\": false\n}\n";

// ---------------------------------------------------------------------------
// 1. Auth status parsing reads only loggedIn and authMethod.
// ---------------------------------------------------------------------------
void test_auth_status_parsing() {
    const auto logged_in = cipher::parse_claude_auth_status(LOGGED_IN_JSON);
    check(logged_in && logged_in->logged_in && logged_in->auth_method == "claude.ai",
          "auth status parses loggedIn and authMethod", "parse failed");

    const auto logged_out = cipher::parse_claude_auth_status(LOGGED_OUT_JSON);
    check(logged_out && !logged_out->logged_in, "auth status reports logged out", "parse failed");

    check(!cipher::parse_claude_auth_status("not json at all"), "malformed auth output is rejected", "accepted");
    check(!cipher::parse_claude_auth_status("{\"loggedIn\": \"yes\"}"),
          "non-boolean loggedIn is rejected", "accepted");
    const auto unsafe = cipher::parse_claude_auth_status("{\"loggedIn\": true, \"authMethod\": \"a b;rm\"}");
    check(unsafe && unsafe->auth_method.empty(), "unsafe authMethod characters are dropped", "kept");
}

// ---------------------------------------------------------------------------
// 2. Diagnostics are redacted and bounded.
// ---------------------------------------------------------------------------
void test_diagnostic_redaction() {
    const std::string redacted = cipher::redact_diagnostic("failed with sk-ant-api03-SECRET\tfor user\n");
    check(!contains(redacted, "SECRET") && contains(redacted, "[REDACTED]"),
          "credential-like tokens are redacted", "output: " + redacted);
    check(!contains(redacted, "\t") && !contains(redacted, "\n"), "control characters are removed",
          "output: " + redacted);

    std::string long_text;
    for (int index = 0; index < 500; ++index) long_text += "word ";
    check(cipher::redact_diagnostic(long_text).size() <= 243, "diagnostics are length-bounded",
          "length " + std::to_string(cipher::redact_diagnostic(long_text).size()));
}

// ---------------------------------------------------------------------------
// 3. The invocation contract: tools, MCP, skills and settings are disabled,
//    the prompt goes over stdin, and no bypass or context flags appear.
// ---------------------------------------------------------------------------
void test_claude_invocation_contract() {
    ScriptedRunner runner;
    runner.queue(ok_result(LOGGED_IN_JSON));
    runner.queue(ok_result("reply\n"));

    cipher::ClaudeCodeBackend backend("/usr/bin/claude", runner);
    cipher::AssistantRequest request;
    request.prompt = "--dangerously-skip-permissions $(rm -rf x) & echo";
    request.local_only = false;
    const auto response = cipher::AssistantManager::submit(&backend, request);
    check(response.is_ok(), "submit succeeds with scripted CLI output", response.error.value_or("none"));
    if (runner.calls.size() < 2) {
        check(false, "invocation contract is observable", "expected auth probe and request");
        return;
    }

    const auto& invocation = runner.calls[1];
    const auto& args = invocation.arguments;
    auto has_pair = [&](const std::string& flag, const std::string& value) {
        for (std::size_t index = 0; index + 1 < args.size(); ++index) {
            if (args[index] == flag && args[index + 1] == value) return true;
        }
        return false;
    };

    check(has_pair("--tools", ""), "built-in tools are disabled with an empty --tools value", "missing");
    bool has_restricted = false;
    bool has_safe_mode = false;
    bool has_strict_mcp = false;
    bool has_no_skills = false;
    bool has_print = false;
    bool prompt_in_args = false;
    bool forbidden_flag = false;
    const std::vector<std::string> forbidden = {
        "--dangerously-skip-permissions", "--bare", "--mcp-config", "--add-dir",
        "--permission-mode", "--settings", "--plugin-dir", "--agents", "--allowedTools"};
    for (const auto& argument : args) {
        has_restricted = has_restricted || argument == "--restricted";
        has_safe_mode = has_safe_mode || argument == "--safe-mode";
        has_strict_mcp = has_strict_mcp || argument == "--strict-mcp-config";
        has_no_skills = has_no_skills || argument == "--disable-slash-commands";
        has_print = has_print || argument == "--print";
        prompt_in_args = prompt_in_args || contains(argument, "dangerously-skip") || contains(argument, "rm -rf");
        for (const auto& flag : forbidden) forbidden_flag = forbidden_flag || argument == flag;
    }
    check(has_restricted && has_safe_mode && has_strict_mcp && has_no_skills && has_print,
          "restricted, safe-mode, strict-MCP, no-skills and print flags are present",
          "one or more required flags missing");
    check(!prompt_in_args, "prompt text never appears in argv", "prompt leaked into arguments");
    check(!forbidden_flag, "no bypass, bare, settings or extra-context flags are used", "forbidden flag present");
    check(invocation.stdin_text == request.prompt, "prompt is delivered over stdin", "stdin mismatch");
    check(!invocation.working_directory.empty() &&
              std::filesystem::path(invocation.working_directory) != std::filesystem::current_path(),
          "child runs in an isolated empty directory, not the caller's", "working directory was: " + invocation.working_directory);
    check(response.text == "reply", "stdout is trimmed into the response text", "text: " + response.text);
    check(response.route == cipher::ProcessingRoute::RemoteCloud, "Claude route is classified remote_cloud", "route mismatch");
}

// ---------------------------------------------------------------------------
// 4. Availability: missing binary, failed auth, unparseable auth, logged out.
// ---------------------------------------------------------------------------
void test_availability_and_auth_failures() {
    ScriptedRunner runner;
    cipher::ClaudeCodeBackend missing("", runner);
    check(!missing.is_available() && contains(missing.diagnostic(), "not found"),
          "missing CLI is unavailable with a diagnostic", missing.diagnostic());

    cipher::ProcessResult failed;
    failed.started = true;
    failed.exit_code = 1;
    runner.queue(failed);
    cipher::ClaudeCodeBackend failing("/usr/bin/claude", runner);
    check(!failing.is_available(), "failed auth status makes Claude unavailable", "available");

    runner.queue(ok_result("garbage"));
    cipher::ClaudeCodeBackend malformed("/usr/bin/claude", runner);
    check(!malformed.is_available() && contains(malformed.diagnostic(), "not recognised"),
          "unparseable auth output makes Claude unavailable", malformed.diagnostic());

    runner.queue(ok_result(LOGGED_OUT_JSON));
    cipher::ClaudeCodeBackend logged_out("/usr/bin/claude", runner);
    check(!logged_out.is_available() && contains(logged_out.diagnostic(), "not logged in"),
          "logged-out CLI is unavailable with login guidance", logged_out.diagnostic());
}

// ---------------------------------------------------------------------------
// 5. Failure mapping: nonzero exit, timeout, overflow, empty output.
// ---------------------------------------------------------------------------
void test_request_failure_mapping() {
    ScriptedRunner runner;

    cipher::ProcessResult exited;
    exited.started = true;
    exited.exit_code = 2;
    exited.stderr_text = "bad key sk-ant-api03-LEAK";

    cipher::ProcessResult timed_out;
    timed_out.started = true;
    timed_out.timed_out = true;

    cipher::ProcessResult overflowed;
    overflowed.started = true;
    overflowed.output_overflow = true;
    overflowed.stdout_text = "partial";

    struct Case {
        cipher::ProcessResult reply;
        cipher::AssistantStatus expected;
        std::string name;
    };
    const std::vector<Case> cases = {
        {exited, cipher::AssistantStatus::Error, "nonzero exit maps to error"},
        {timed_out, cipher::AssistantStatus::Timeout, "timeout maps to timeout"},
        {overflowed, cipher::AssistantStatus::Error, "overflow maps to error"},
        {ok_result("   \n"), cipher::AssistantStatus::Error, "empty output maps to error"},
    };
    for (const auto& test_case : cases) {
        runner.replies.clear();
        runner.queue(ok_result(LOGGED_IN_JSON));
        runner.queue(test_case.reply);
        cipher::ClaudeCodeBackend backend("/usr/bin/claude", runner);
        cipher::AssistantRequest request;
        request.local_only = false;
        request.prompt = "hi";
        const auto response = cipher::AssistantManager::submit(&backend, request);
        check(response.status == test_case.expected && response.text.empty(), test_case.name,
              "status " + cipher::to_string(response.status));
    }

    runner.replies.clear();
    runner.queue(ok_result(LOGGED_IN_JSON));
    runner.queue(exited);
    cipher::ClaudeCodeBackend backend("/usr/bin/claude", runner);
    cipher::AssistantRequest request;
    request.local_only = false;
    request.prompt = "hi";
    const auto response = cipher::AssistantManager::submit(&backend, request);
    check(response.error && !contains(*response.error, "LEAK") && contains(*response.error, "[REDACTED]"),
          "stderr in error messages is redacted", response.error.value_or("none"));
}

// ---------------------------------------------------------------------------
// 6. Central policy: local-only requests are rejected for the cloud route,
//    and the CLI is never invoked.
// ---------------------------------------------------------------------------
void test_local_only_policy_rejects_cloud_route() {
    ScriptedRunner runner;
    runner.queue(ok_result(LOGGED_IN_JSON));
    cipher::ClaudeCodeBackend backend("/usr/bin/claude", runner);
    cipher::AssistantRequest request;
    request.prompt = "hi";
    const auto response = cipher::AssistantManager::submit(&backend, request);
    check(response.status == cipher::AssistantStatus::PolicyViolation,
          "local-only request is rejected for the cloud route", "status " + cipher::to_string(response.status));
    // Only the auth probe may have run. The request itself must never be invoked.
    const bool only_probe = runner.calls.size() == 1 && !runner.calls.empty() &&
                            runner.calls[0].arguments.size() == 3 && runner.calls[0].arguments[0] == "auth";
    check(only_probe, "rejected request never reaches the CLI", "unexpected invocations: " + std::to_string(runner.calls.size()));
}

// ---------------------------------------------------------------------------
// 7. Hermes fails closed: unavailable, route unknown, never executes.
// ---------------------------------------------------------------------------
void test_hermes_fails_closed() {
    cipher::HermesBackend hermes;
    check(!hermes.is_available(), "Hermes reports unavailable", "available");
    check(hermes.capabilities().processing_route == cipher::ProcessingRoute::Unknown,
          "Hermes route is classified unknown", "route mismatch");

    cipher::AssistantRequest request;
    request.prompt = "hi";
    request.local_only = false;
    const auto response = cipher::AssistantManager::submit(&hermes, request);
    check(response.status == cipher::AssistantStatus::Unavailable, "Hermes submit is refused as unavailable",
          "status " + cipher::to_string(response.status));
    check(contains(hermes.diagnostic(), "fail closed"), "Hermes diagnostic explains fail-closed behaviour",
          hermes.diagnostic());
}

// ---------------------------------------------------------------------------
// 8. End to end with the real runner and the deterministic fake CLI.
// ---------------------------------------------------------------------------
void set_environment(const std::string& name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

void test_end_to_end_with_fake_cli() {
    set_environment("CIPHER_FAKE_AUTH", "loggedin");
    cipher::SystemProcessRunner runner;
    cipher::ClaudeCodeBackend backend(CIPHER_FAKE_CLI_PATH, runner);
    check(backend.is_available(), "fake CLI auth probe reports available", backend.diagnostic());

    cipher::AssistantRequest request;
    request.prompt = "hello from cipher";
    request.local_only = false;
    request.timeout_seconds = 20.0;
    const auto response = cipher::AssistantManager::submit(&backend, request);
    check(response.is_ok() && response.text == "stdin:hello from cipher",
          "end-to-end request returns the CLI's text over stdin",
          "status " + cipher::to_string(response.status) + " text " + response.text);

    cipher::AssistantRequest cwd_request = request;
    cwd_request.prompt = "MODE:cwd";
    const auto cwd_response = cipher::AssistantManager::submit(&backend, cwd_request);
    check(cwd_response.is_ok() && !cwd_response.text.empty() &&
              std::filesystem::path(cwd_response.text) != std::filesystem::current_path(),
          "end-to-end child does not run in the caller's directory", cwd_response.text);

    set_environment("CIPHER_FAKE_AUTH", "loggedout");
    check(!backend.is_available() && contains(backend.diagnostic(), "not logged in"),
          "end-to-end logged-out state is unavailable", backend.diagnostic());
    set_environment("CIPHER_FAKE_AUTH", "loggedin");
}

} // anonymous namespace

int main() {
    std::cout << "Running CIPHER Assistant CLI Test Suite...\n\n";

    test_auth_status_parsing();
    test_diagnostic_redaction();
    test_claude_invocation_contract();
    test_availability_and_auth_failures();
    test_request_failure_mapping();
    test_local_only_policy_rejects_cloud_route();
    test_hermes_fails_closed();
    test_end_to_end_with_fake_cli();

    std::cout << "\n-------------------------------------------------------------\n";
    std::cout << "Tests passed: " << g_passes << '\n';
    std::cout << "Tests failed: " << g_failures << '\n';
    std::cout << "-------------------------------------------------------------\n";

    return (g_failures == 0) ? 0 : 1;
}
