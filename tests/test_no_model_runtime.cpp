#include "cipher/assistant_backend.hpp"
#include "cipher/cipher_core.hpp"
#include "cipher/memory_commands.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// test_no_model_runtime.cpp  —  v0.4 no-model verification
//
// CIPHER must stay useful with no AI model or assistant provider attached.
// These tests run the core, the memory commands and the assistant manager
// together, with no backend or an unavailable backend, and check that a
// failed provider request neither dispatches nor mutates the memory file.
// Same plain pass/fail bookkeeping as the other CIPHER suites.
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

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::string fresh_path(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / "cipher_no_model_tests";
    std::filesystem::create_directories(dir);
    const auto path = dir / (name + ".txt");
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".tmp");
    return path.string();
}

// Counts dispatches so tests can prove a request never reached a provider.
class CountingUnavailableBackend : public cipher::IAssistantBackend {
public:
    int dispatches = 0;

    std::string backend_id() const override { return "test_unavailable_backend"; }
    bool is_available() override { return false; }
    cipher::AssistantCapabilities capabilities() const override {
        cipher::AssistantCapabilities caps;
        caps.processing_route = cipher::ProcessingRoute::Unknown;
        return caps;
    }
    cipher::AssistantResponse execute_request(const cipher::AssistantRequest&) override {
        ++dispatches;
        cipher::AssistantResponse resp;
        resp.status = cipher::AssistantStatus::Ok;
        return resp;
    }
};

cipher::AssistantRequest sample_request() {
    cipher::AssistantRequest request;
    request.prompt = "summarise my notes";
    return request;
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------

void test_core_runs_without_any_provider() {
    const cipher::CipherCore core;

    const auto status = core.execute("status");
    check(contains(status.text, "AI runtime: not connected") &&
          contains(status.text, "External AI-provider dependency: none"),
          "no-model: status reports no provider and no AI runtime",
          "status text: " + status.text);

    const auto time = core.execute("time");
    check(contains(time.text, "Local system time:"),
          "no-model: non-AI command 'time' works",
          "time text: " + time.text);

    const auto unknown = core.execute("ask claude hello");
    check(contains(unknown.text, "Unknown command: ask"),
          "no-model: 'ask' is unknown in this build and does not reach a provider",
          "got: " + unknown.text);

    const auto quit = core.execute("exit");
    check(quit.exit_requested,
          "no-model: exit still ends the session",
          "exit_requested was false");
}

void test_memory_works_without_any_provider() {
    const std::string path = fresh_path("no_model_memory");
    cipher::MemoryCommands session(path);
    check(session.open().is_ok(), "no-model: memory opens with no provider",
          "open() reported an error");

    const auto saved = session.execute("remember plan ship the core first");
    check(contains(saved.text, "Remembered 'plan'"),
          "no-model: remember works with no provider",
          "got: " + saved.text);

    const auto recalled = session.execute("recall plan");
    check(contains(recalled.text, "ship the core first"),
          "no-model: recall returns the stored entry",
          "got: " + recalled.text);
}

void test_unavailable_provider_fails_cleanly_and_does_not_mutate_memory() {
    const std::string path = fresh_path("no_model_unavailable");
    {
        cipher::MemoryCommands seed(path);
        seed.open();
        seed.execute("remember plan keep this entry");
    }
    const std::string before = read_file(path);

    cipher::MemoryCommands session(path);
    session.open();

    CountingUnavailableBackend backend;
    const auto response = cipher::AssistantManager::submit(&backend, sample_request());

    check(response.status == cipher::AssistantStatus::Unavailable,
          "no-model: unavailable provider returns Unavailable",
          "status was not Unavailable");
    check(contains(response.error.value_or(""), "is currently unavailable"),
          "no-model: unavailable provider gives a diagnostic message",
          "error: " + response.error.value_or(""));
    check(backend.dispatches == 0,
          "no-model: a request to an unavailable provider is never dispatched",
          "execute_request was called " + std::to_string(backend.dispatches) + " time(s)");
    check(read_file(path) == before,
          "no-model: failed provider request leaves the memory file byte-identical",
          "memory file changed");
    check(contains(session.execute("recall plan").text, "keep this entry"),
          "no-model: existing memory survives a failed provider request",
          "entry was lost");
}

void test_null_provider_fails_cleanly() {
    const auto response = cipher::AssistantManager::submit(nullptr, sample_request());
    check(response.status == cipher::AssistantStatus::Unavailable &&
          contains(response.error.value_or(""), "No assistant backend configured"),
          "no-model: no configured provider returns Unavailable with a clear message",
          "status or error text was unexpected: " + response.error.value_or(""));

    const cipher::CipherCore core;
    check(contains(core.execute("help").text, "help"),
          "no-model: core still responds after a failed provider request",
          "help did not respond");
}

} // namespace

int main() {
    test_core_runs_without_any_provider();
    test_memory_works_without_any_provider();
    test_unavailable_provider_fails_cleanly_and_does_not_mutate_memory();
    test_null_provider_fails_cleanly();

    std::cout << '\n' << g_passes << " passed, " << g_failures << " failed\n";
    if (g_failures > 0) {
        std::cerr << g_failures << " no-model test(s) failed.\n";
        return 1;
    }
    std::cout << "All no-model runtime tests passed.\n";
    return 0;
}
