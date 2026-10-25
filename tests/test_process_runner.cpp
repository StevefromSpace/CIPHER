// ---------------------------------------------------------------------------
// tests/test_process_runner.cpp  —  Bounded process runner tests
//
// Runs the real SystemProcessRunner against the deterministic fake CLI fixture
// (CIPHER_FAKE_CLI_PATH). No network, credentials or paid services are used.
// ---------------------------------------------------------------------------

#include "cipher/process_runner.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

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

const std::string TEMP_DIR = "test_process_tmp_dir";
// A run that is killed promptly finishes far below this; a hung run would not.
constexpr double PROMPT_COMPLETION_SECONDS = 20.0;

cipher::ProcessRequest fake_request(const std::string& prompt, double timeout_seconds = 10.0) {
    cipher::ProcessRequest request;
    request.executable = CIPHER_FAKE_CLI_PATH;
    request.arguments = {"--print"};
    request.stdin_text = prompt;
    request.timeout_seconds = timeout_seconds;
    return request;
}

// Windows text-mode stdout writes CRLF; compare with line endings normalised.
std::string strip_carriage_returns(std::string text) {
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// ---------------------------------------------------------------------------
// 1. Successful run: stdin reaches the child and stdout is returned.
// ---------------------------------------------------------------------------
void test_stdin_delivered_and_stdout_returned() {
    cipher::SystemProcessRunner runner;
    const auto result = runner.run(fake_request("hello there"));
    check(result.started, "runner starts a valid executable", result.error);
    check(result.exit_code == 0, "successful child exits with code 0",
          "exit code " + std::to_string(result.exit_code));
    check(strip_carriage_returns(result.stdout_text) == "stdin:hello there\n",
          "stdin is delivered and stdout returned", "stdout was: " + result.stdout_text);
    check(!result.timed_out && !result.output_overflow, "successful run reports no timeout or overflow", "flag set");
}

// ---------------------------------------------------------------------------
// 2. Nonzero exit code is reported, with stderr captured separately.
// ---------------------------------------------------------------------------
void test_nonzero_exit_and_stderr_capture() {
    cipher::SystemProcessRunner runner;
    const auto result = runner.run(fake_request("MODE:fail"));
    check(result.exit_code == 2, "nonzero exit code is reported", "exit code " + std::to_string(result.exit_code));
    check(result.stderr_text.find("rejected token") != std::string::npos,
          "stderr is captured separately", "stderr was: " + result.stderr_text);
    check(result.stdout_text.empty(), "stdout stays empty when only stderr is written", "stdout was: " + result.stdout_text);
}

// ---------------------------------------------------------------------------
// 3. Stdout overflow kills the child and reports overflow.
// ---------------------------------------------------------------------------
void test_stdout_overflow_terminates_child() {
    cipher::SystemProcessRunner runner;
    auto request = fake_request("MODE:big-stdout");
    request.max_stdout_bytes = 1024;
    const auto start = std::chrono::steady_clock::now();
    const auto result = runner.run(request);
    check(result.output_overflow, "stdout overflow is reported", "output_overflow was false");
    check(seconds_since(start) < PROMPT_COMPLETION_SECONDS, "stdout overflow terminates the child promptly",
          "run took too long");
}

// ---------------------------------------------------------------------------
// 4. Stderr overflow is bounded independently of stdout.
// ---------------------------------------------------------------------------
void test_stderr_overflow_terminates_child() {
    cipher::SystemProcessRunner runner;
    auto request = fake_request("MODE:big-stderr");
    request.max_stderr_bytes = 1024;
    const auto start = std::chrono::steady_clock::now();
    const auto result = runner.run(request);
    check(result.output_overflow, "stderr overflow is reported", "output_overflow was false");
    check(seconds_since(start) < PROMPT_COMPLETION_SECONDS, "stderr overflow terminates the child promptly",
          "run took too long");
}

// ---------------------------------------------------------------------------
// 5. A hung child is terminated at the deadline.
// ---------------------------------------------------------------------------
void test_timeout_terminates_child() {
    cipher::SystemProcessRunner runner;
    const auto start = std::chrono::steady_clock::now();
    const auto result = runner.run(fake_request("MODE:sleep", 1.0));
    check(result.timed_out, "timeout is reported", "timed_out was false");
    check(seconds_since(start) < PROMPT_COMPLETION_SECONDS, "timeout terminates the child promptly",
          "run took too long");
}

// ---------------------------------------------------------------------------
// 6. A missing executable is reported as not started, never as an exit code.
// ---------------------------------------------------------------------------
void test_missing_executable_is_not_started() {
    cipher::SystemProcessRunner runner;
    cipher::ProcessRequest request;
    request.executable = (std::filesystem::current_path() / "cipher-no-such-binary-xyz").string();
    request.timeout_seconds = 5.0;
    const auto result = runner.run(request);
    check(!result.started, "missing executable is reported as not started", "started was true");
    check(!result.error.empty(), "missing executable produces a diagnostic", "error was empty");
}

// ---------------------------------------------------------------------------
// 7. Arguments are passed literally, never interpreted by a shell.
// ---------------------------------------------------------------------------
void test_arguments_are_not_shell_interpreted() {
    cipher::SystemProcessRunner runner;
    cipher::ProcessRequest request;
    request.executable = CIPHER_FAKE_CLI_PATH;
    request.arguments = {"--argv-check", "a & echo pwned", "$(id)", "x|y"};
    request.timeout_seconds = 10.0;
    const auto result = runner.run(request);
    check(strip_carriage_returns(result.stdout_text) == "a & echo pwned|$(id)|x|y\n",
          "arguments are passed literally without shell interpretation",
          "stdout was: " + result.stdout_text);
}

// ---------------------------------------------------------------------------
// 8. The working directory is applied to the child.
// ---------------------------------------------------------------------------
void test_working_directory_is_applied() {
    std::error_code ec;
    const auto directory = std::filesystem::absolute(TEMP_DIR + "/cwd");
    std::filesystem::create_directories(directory, ec);

    cipher::SystemProcessRunner runner;
    auto request = fake_request("MODE:cwd");
    request.working_directory = directory.string();
    const auto result = runner.run(request);

    std::string reported = result.stdout_text;
    while (!reported.empty() && (reported.back() == '\n' || reported.back() == '\r')) reported.pop_back();
    check(std::filesystem::exists(reported, ec) && std::filesystem::equivalent(reported, directory, ec),
          "child runs in the requested working directory", "reported: " + reported);

    std::filesystem::remove_all(TEMP_DIR, ec);
}

#if defined(_WIN32)
// ---------------------------------------------------------------------------
// 9. Only the child's stdio handles are inherited (Windows).
//    A sentinel inheritable handle in the parent must not be visible to the child.
// ---------------------------------------------------------------------------
void test_sentinel_handle_is_not_inherited() {
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE sentinel = CreateEventW(&attributes, TRUE, FALSE, nullptr);
    check(sentinel != nullptr, "sentinel inheritable handle is created", "CreateEventW failed");
    if (sentinel == nullptr) {
        return;
    }

    cipher::ProcessRequest request;
    request.executable = CIPHER_FAKE_CLI_PATH;
    request.arguments = {"--handle-check", std::to_string(reinterpret_cast<std::uintptr_t>(sentinel))};
    request.timeout_seconds = 10.0;
    const auto result = cipher::SystemProcessRunner().run(request);
    CloseHandle(sentinel);

    check(result.started, "handle-check child starts", result.error);
    check(strip_carriage_returns(result.stdout_text) == "not-visible\n",
          "sentinel handle is not inherited by the child",
          "child reported: " + result.stdout_text);
}
#endif

} // anonymous namespace

int main() {
    std::cout << "Running CIPHER Process Runner Test Suite...\n\n";

    test_stdin_delivered_and_stdout_returned();
    test_nonzero_exit_and_stderr_capture();
    test_stdout_overflow_terminates_child();
    test_stderr_overflow_terminates_child();
    test_timeout_terminates_child();
    test_missing_executable_is_not_started();
    test_arguments_are_not_shell_interpreted();
    test_working_directory_is_applied();
#if defined(_WIN32)
    test_sentinel_handle_is_not_inherited();
#endif

    std::cout << "\n-------------------------------------------------------------\n";
    std::cout << "Tests passed: " << g_passes << '\n';
    std::cout << "Tests failed: " << g_failures << '\n';
    std::cout << "-------------------------------------------------------------\n";

    return (g_failures == 0) ? 0 : 1;
}
