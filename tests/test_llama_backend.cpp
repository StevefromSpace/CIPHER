#include "cipher/ask_commands.hpp"
#include "cipher/config.hpp"
#include "cipher/llama_backend.hpp"
#include "cipher/memory_commands.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

// ---------------------------------------------------------------------------
// test_llama_backend.cpp  —  in-process llama.cpp backend (v0.6)
//
// Two groups:
//   - Deterministic tests need no model: bad paths, bad settings and files that
//     are not GGUF must leave the backend unavailable and fail closed.
//   - Real-runtime tests load the GGUF file named by CIPHER_TEST_GGUF and run
//     actual llama.cpp inference. They are skipped (and say so) when it is unset.
//     CI sets it to a small pinned model; see .github/workflows/ci.yml.
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

std::filesystem::path temp_dir() {
    const auto dir = std::filesystem::temp_directory_path() / "cipher_llama_backend_tests";
    std::filesystem::create_directories(dir);
    return dir;
}

cipher::LlamaCppConfig config_for(const std::string& path) {
    cipher::LlamaCppConfig config;
    config.model_path = path;
    return config;
}

// An unavailable backend must refuse every request with Unavailable and report a non-local route.
void check_fails_closed(cipher::LlamaCppBackend& backend, const std::string& name, const std::string& reason_part) {
    check(!backend.is_available(), name + ": unavailable", "backend reported available");
    check(contains(backend.load_error(), reason_part), name + ": load error names the cause",
          "load_error was: " + backend.load_error());
    check(backend.capabilities().processing_route != cipher::ProcessingRoute::VerifiedLocal,
          name + ": route is not VerifiedLocal", "route was VerifiedLocal");

    cipher::AssistantRequest request;
    request.prompt = "hello";
    const cipher::AssistantResponse resp = backend.execute_request(request);
    check(resp.status == cipher::AssistantStatus::Unavailable && resp.error && contains(*resp.error, reason_part),
          name + ": request returns Unavailable with the cause", "status/error did not match");
}

void test_empty_path_is_unavailable() {
    cipher::LlamaCppBackend backend(config_for(""));
    check_fails_closed(backend, "empty path", "llama_model_path");
}

void test_missing_file_is_unavailable() {
    const std::string path = (temp_dir() / "does-not-exist.gguf").string();
    cipher::LlamaCppBackend backend(config_for(path));
    check_fails_closed(backend, "missing file", "does not exist");
}

void test_directory_is_unavailable() {
    cipher::LlamaCppBackend backend(config_for(temp_dir().string()));
    check_fails_closed(backend, "directory path", "not a regular file");
}

void test_non_gguf_file_is_unavailable() {
    const auto path = temp_dir() / "not-a-model.gguf";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "this is not a GGUF model";
    }
    cipher::LlamaCppBackend backend(config_for(path.string()));
    check_fails_closed(backend, "non-GGUF file", "valid GGUF");
}

void test_invalid_settings_are_unavailable() {
    cipher::LlamaCppConfig config = config_for("unused.gguf");
    config.context_tokens = 128;
    config.max_output_tokens = 128;
    cipher::LlamaCppBackend backend(config);
    check_fails_closed(backend, "max_output_tokens >= context_tokens", "Invalid llama.cpp settings");

    cipher::LlamaCppConfig nan_config = config_for("unused.gguf");
    nan_config.temperature = std::nanf("");
    cipher::LlamaCppBackend nan_backend(nan_config);
    check_fails_closed(nan_backend, "NaN temperature", "Invalid llama.cpp settings");
}

void test_unavailable_backend_never_answers_ask() {
    cipher::LlamaCppBackend backend(config_for(""));
    cipher::AskCommands ask(&backend);
    const cipher::AskCommandResult result = ask.execute("ask hello");
    check(result.handled && contains(result.text, "ask failed ("),
          "unavailable: ask fails closed through AssistantManager", "text was: " + result.text);
}

// ---------------------------------------------------------------------------
// Real llama.cpp inference (needs CIPHER_TEST_GGUF)
// ---------------------------------------------------------------------------

cipher::AssistantRequest request_for(const std::string& prompt, double timeout_seconds = 120.0) {
    cipher::AssistantRequest request;
    request.prompt = prompt;
    request.timeout_seconds = timeout_seconds;
    return request;
}

void test_real_model_generates_text(cipher::LlamaCppBackend& backend, const std::string& path) {
    check(backend.is_available(), "real: model loads", "load_error was: " + backend.load_error());
    check(backend.backend_id() == "llama.cpp", "real: backend id", "id was: " + backend.backend_id());
    check(backend.capabilities().processing_route == cipher::ProcessingRoute::VerifiedLocal,
          "real: loaded backend is VerifiedLocal", "route was not VerifiedLocal");

    const cipher::AssistantResponse resp =
        cipher::AssistantManager::submit(&backend, request_for("Once upon a time"));
    check(resp.is_ok() && !resp.text.empty(), "real: generates text through AssistantManager",
          "status " + cipher::to_string(resp.status) + ", error: " + resp.error.value_or(""));
    check(resp.model == std::filesystem::path(path).filename().string() &&
              resp.route == cipher::ProcessingRoute::VerifiedLocal,
          "real: response names the model file and local route", "model was: " + resp.model);
}

// Greedy decoding is deterministic, so two identical requests match only if no state carries over.
void test_real_requests_do_not_share_context(cipher::LlamaCppBackend& backend) {
    const cipher::AssistantResponse first = backend.execute_request(request_for("The cat sat on"));
    backend.execute_request(request_for("A different story about a dragon and a secret"));
    const cipher::AssistantResponse again = backend.execute_request(request_for("The cat sat on"));
    check(first.is_ok() && again.is_ok() && first.text == again.text,
          "real: earlier prompts do not leak into later answers",
          "first: '" + first.text + "' again: '" + again.text + "'");
}

void test_real_failures_are_reported(cipher::LlamaCppBackend& backend) {
    const cipher::AssistantResponse late = backend.execute_request(request_for("Once upon a time", 1e-9));
    check(late.status == cipher::AssistantStatus::Timeout, "real: expired deadline returns Timeout",
          "status was " + cipher::to_string(late.status));

    cipher::AssistantRequest override_request = request_for("hi");
    override_request.model_override = std::string("other-model");
    check(backend.execute_request(override_request).status == cipher::AssistantStatus::Error,
          "real: model_override is refused", "model_override was accepted");

    std::string long_prompt;
    for (int i = 0; i < 2000; ++i) long_prompt += "word ";
    const cipher::AssistantResponse too_long = backend.execute_request(request_for(long_prompt));
    check(too_long.status == cipher::AssistantStatus::Error && too_long.error && contains(*too_long.error, "tokens"),
          "real: prompt larger than the context is refused", "status was " + cipher::to_string(too_long.status));

    check(backend.execute_request(request_for("Once upon a time")).is_ok(),
          "real: backend still answers after failures", "request after failures did not succeed");
}

void test_real_denied_memory_never_reaches_model(cipher::LlamaCppBackend& backend) {
    const auto path = temp_dir() / "memory.txt";
    std::filesystem::remove(path);
    cipher::PermissionPolicy policy;
    policy.add_blacklist_pattern("share secret");
    cipher::MemoryCommands memory(path.string(), policy);
    memory.open();
    memory.execute("remember secret CANARY-llama-private");

    cipher::AskCommands ask(&backend, 120.0, nullptr, &memory);
    const cipher::AskCommandResult result = ask.execute("ask --memory secret repeat it");
    check(contains(result.text, "Denied by permission policy") && result.shared_memory_id.empty(),
          "real: denied memory is refused before llama.cpp", "text was: " + result.text);
}

// Builds the backend the way main() does: cipher.conf text -> Config::llama -> LlamaCppBackend.
std::unique_ptr<cipher::LlamaCppBackend> backend_from_conf(const std::string& path, const std::string& settings) {
    const cipher::ConfigResult parsed =
        cipher::ConfigParser::load_from_string("llama_model_path = " + path + "\n" + settings);
    if (!parsed.ok()) return nullptr;
    return std::make_unique<cipher::LlamaCppBackend>(parsed.config.llama);
}

// Each cipher.conf setting must change what llama.cpp actually does, not just sit in Config.
void test_real_conf_settings_reach_llama(cipher::LlamaCppBackend& small, const std::string& path) {
    const auto big = backend_from_conf(path, "llama_context_tokens = 2048\nllama_max_output_tokens = 4\n"
                                             "llama_temperature = 0\n");
    check(big && big->is_available(), "real conf: backend loads from cipher.conf settings", "parse or load failed");
    if (!big || !big->is_available()) return;

    // Too long for small's 512-token context, short enough for 2048: llama.cpp must have the bigger window.
    std::string prompt;
    for (int i = 0; i < 600; ++i) prompt += "word ";
    check(small.execute_request(request_for(prompt)).status == cipher::AssistantStatus::Error &&
              big->execute_request(request_for(prompt)).is_ok(),
          "real conf: llama_context_tokens sets the llama.cpp context", "the 2048-token context did not take the prompt");

    // Both greedy; the 4-token answer must be the start of the 32-token one.
    const cipher::AssistantResponse shorter = big->execute_request(request_for("Once upon a time"));
    const cipher::AssistantResponse longer = small.execute_request(request_for("Once upon a time"));
    check(shorter.is_ok() && longer.is_ok() && shorter.text.size() < longer.text.size() &&
              longer.text.compare(0, shorter.text.size(), shorter.text) == 0,
          "real conf: llama_max_output_tokens caps the answer",
          "4-token: '" + shorter.text + "' 32-token: '" + longer.text + "'");

    // Greedy runs repeat (checked above); a hot sampler with a random seed should not.
    const auto hot = backend_from_conf(path, "llama_context_tokens = 512\nllama_max_output_tokens = 32\n"
                                             "llama_temperature = 5\n");
    const bool hot_loaded = hot && hot->is_available();
    const std::string a = hot_loaded ? hot->execute_request(request_for("Once upon a time")).text : "";
    const std::string b = hot_loaded ? hot->execute_request(request_for("Once upon a time")).text : "";
    check(hot_loaded && !a.empty() && a != b, "real conf: llama_temperature reaches the sampler",
          "two answers at temperature 5 were identical: '" + a + "'");
}

void run_real_runtime_tests() {
    const char* path = std::getenv("CIPHER_TEST_GGUF");
    if (path == nullptr || *path == '\0') {
        std::cout << "SKIP: real llama.cpp inference (set CIPHER_TEST_GGUF to a local .gguf file)\n";
        return;
    }
    cipher::LlamaCppConfig config = config_for(path);
    config.context_tokens = 512;
    config.max_output_tokens = 32;
    config.temperature = 0.0f;  // greedy, so answers are repeatable
    cipher::LlamaCppBackend backend(config);

    test_real_model_generates_text(backend, path);
    if (!backend.is_available()) return;
    test_real_requests_do_not_share_context(backend);
    test_real_failures_are_reported(backend);
    test_real_denied_memory_never_reaches_model(backend);
    test_real_conf_settings_reach_llama(backend, path);
}

} // namespace

int main() {
    test_empty_path_is_unavailable();
    test_missing_file_is_unavailable();
    test_directory_is_unavailable();
    test_non_gguf_file_is_unavailable();
    test_invalid_settings_are_unavailable();
    test_unavailable_backend_never_answers_ask();
    run_real_runtime_tests();

    std::cout << "\n" << g_passes << " passed, " << g_failures << " failed\n";
    return g_failures == 0 ? 0 : 1;
}
