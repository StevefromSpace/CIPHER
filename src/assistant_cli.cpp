#include "cipher/assistant_cli.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <sstream>
#include <utility>
#include <vector>

namespace cipher {

namespace {

constexpr double AUTH_PROBE_TIMEOUT_SECONDS = 15.0;
constexpr std::size_t AUTH_PROBE_MAX_BYTES = 16384;
constexpr std::size_t RESPONSE_MAX_BYTES = 262144;
constexpr std::size_t DIAGNOSTIC_MAX_BYTES = 16384;
constexpr std::size_t DIAGNOSTIC_MAX_CHARS = 240;
constexpr std::size_t AUTH_METHOD_MAX_CHARS = 32;

const char* const WHITESPACE = " \t\r\n";

std::string trim_whitespace(const std::string& text) {
    const auto first = text.find_first_not_of(WHITESPACE);
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(WHITESPACE);
    return text.substr(first, last - first + 1);
}

// Position of the value that follows `"key" :`, or npos.
std::size_t json_value_position(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = json.find(needle);
    if (pos == std::string::npos) return std::string::npos;
    pos = json.find_first_not_of(WHITESPACE, pos + needle.size());
    if (pos == std::string::npos || json[pos] != ':') return std::string::npos;
    return json.find_first_not_of(WHITESPACE, pos + 1);
}

bool is_safe_auth_method_char(char character) {
    return std::isalnum(static_cast<unsigned char>(character)) || character == '.' ||
           character == '-' || character == '_';
}

std::string read_auth_method(const std::string& json) {
    const std::size_t value = json_value_position(json, "authMethod");
    if (value == std::string::npos || json[value] != '"') return {};
    const std::size_t end = json.find('"', value + 1);
    if (end == std::string::npos || end - value - 1 > AUTH_METHOD_MAX_CHARS) return {};
    const std::string method = json.substr(value + 1, end - value - 1);
    if (!std::all_of(method.begin(), method.end(), is_safe_auth_method_char)) return {};
    return method;
}

bool looks_sensitive(std::string word) {
    std::transform(word.begin(), word.end(), word.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    for (const char* marker : {"sk-", "key", "token", "secret", "bearer", "password"}) {
        if (word.find(marker) != std::string::npos) return true;
    }
    return false;
}

// Runs in an empty directory so the CLI finds no project context (CLAUDE.md, repo files).
class ScratchDirectory {
public:
    ScratchDirectory() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        if (ec) return;
        std::random_device random;
        const auto candidate = base / ("cipher-claude-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (std::filesystem::create_directory(candidate, ec) && !ec) path_ = candidate;
    }
    ~ScratchDirectory() {
        std::error_code ec;
        if (!path_.empty()) std::filesystem::remove_all(path_, ec);
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    bool ok() const { return !path_.empty(); }
    std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

// Fixed, text-only invocation. The empty "--tools" value disables every built-in tool.
// Prompt input arrives on stdin, not in this list.
std::vector<std::string> claude_print_arguments() {
    return {"--print",
            "--restricted",
            "--tools", "",
            "--strict-mcp-config",
            "--disable-slash-commands",
            "--no-session-persistence",
            "--output-format", "text"};
}

AssistantResponse map_claude_result(const ProcessResult& result, double timeout_seconds,
                                    AssistantResponse response) {
    if (!result.started) {
        response.status = AssistantStatus::Unavailable;
        response.error = "could not start Claude Code CLI: " + result.error;
    } else if (result.timed_out) {
        response.status = AssistantStatus::Timeout;
        response.error = "Claude Code CLI did not finish within " + std::to_string(static_cast<int>(timeout_seconds)) +
                         " seconds and was terminated.";
    } else if (result.output_overflow) {
        response.status = AssistantStatus::Error;
        response.error = "Claude Code CLI output exceeded the response limit; output was discarded.";
    } else if (result.exit_code != 0) {
        response.status = AssistantStatus::Error;
        const std::string detail = redact_diagnostic(result.stderr_text);
        response.error = "Claude Code CLI exited with code " + std::to_string(result.exit_code) +
                         (detail.empty() ? "." : ": " + detail);
    } else if (trim_whitespace(result.stdout_text).empty()) {
        response.status = AssistantStatus::Error;
        response.error = "Claude Code CLI returned an empty response.";
    } else {
        response.status = AssistantStatus::Ok;
        response.text = trim_whitespace(result.stdout_text);
    }
    return response;
}

} // anonymous namespace

std::optional<ClaudeAuthStatus> parse_claude_auth_status(const std::string& json_text) {
    const std::size_t logged_in = json_value_position(json_text, "loggedIn");
    if (logged_in == std::string::npos) return std::nullopt;

    ClaudeAuthStatus status;
    if (json_text.compare(logged_in, 4, "true") == 0) {
        status.logged_in = true;
    } else if (json_text.compare(logged_in, 5, "false") == 0) {
        status.logged_in = false;
    } else {
        return std::nullopt;
    }
    status.auth_method = read_auth_method(json_text);
    return status;
}

std::string redact_diagnostic(const std::string& text) {
    std::string cleaned;
    for (char character : text) {
        const auto code = static_cast<unsigned char>(character);
        if (character == '\n' || character == '\t' || character == '\r') {
            cleaned += ' ';
        } else if (code >= 0x20 && code != 0x7f) {
            cleaned += character;
        }
    }

    std::istringstream words(cleaned);
    std::string word;
    std::string redacted;
    while (words >> word) {
        if (looks_sensitive(word)) word = "[REDACTED]";
        if (!redacted.empty()) redacted += ' ';
        redacted += word;
        if (redacted.size() >= DIAGNOSTIC_MAX_CHARS) break;
    }
    if (redacted.size() > DIAGNOSTIC_MAX_CHARS) {
        redacted.resize(DIAGNOSTIC_MAX_CHARS);
        redacted += "...";
    }
    return redacted;
}

namespace {

// Copies PATH out of the environment. Empty when unset.
std::string read_path_variable() {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, "PATH") != 0 || !value) return {};
    std::string copy(value);
    std::free(value);
    return copy;
#else
    const char* value = std::getenv("PATH");
    return value ? std::string(value) : std::string{};
#endif
}

} // anonymous namespace

std::string find_executable_on_path(const std::string& name) {
#if defined(_WIN32)
    const std::string file_name = name + ".exe";
    const char separator = ';';
#else
    const std::string file_name = name;
    const char separator = ':';
#endif
    std::istringstream directories(read_path_variable());
    std::string directory;
    while (std::getline(directories, directory, separator)) {
        if (directory.empty()) continue;
        const std::filesystem::path candidate = std::filesystem::path(directory) / file_name;
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec) && !ec) return candidate.string();
    }
    return {};
}

ClaudeCodeBackend::ClaudeCodeBackend(std::string executable_path, IProcessRunner& runner)
    : executable_path_(std::move(executable_path)), runner_(runner) {}

bool ClaudeCodeBackend::is_available() {
    diagnostic_.clear();
    if (executable_path_.empty()) {
        diagnostic_ = "Claude Code CLI was not found on PATH.";
        return false;
    }

    ProcessRequest probe;
    probe.executable = executable_path_;
    probe.arguments = {"auth", "status", "--json"};
    probe.timeout_seconds = AUTH_PROBE_TIMEOUT_SECONDS;
    probe.max_stdout_bytes = AUTH_PROBE_MAX_BYTES;
    probe.max_stderr_bytes = DIAGNOSTIC_MAX_BYTES;
    const ProcessResult result = runner_.run(probe);

    if (!result.started) {
        diagnostic_ = "Claude Code CLI could not be started: " + result.error;
        return false;
    }
    if (result.timed_out || result.output_overflow || result.exit_code != 0) {
        diagnostic_ = "Claude Code CLI auth status check failed; availability cannot be verified.";
        return false;
    }
    const auto status = parse_claude_auth_status(result.stdout_text);
    if (!status) {
        diagnostic_ = "Claude Code CLI auth status output was not recognised; availability cannot be verified.";
        return false;
    }
    if (!status->logged_in) {
        diagnostic_ = "Claude Code CLI is not logged in. Run 'claude auth login' in a terminal.";
        return false;
    }
    diagnostic_ = "logged in (auth method: " +
                  (status->auth_method.empty() ? std::string("unknown") : status->auth_method) +
                  "); billing follows that CLI account.";
    return true;
}

AssistantCapabilities ClaudeCodeBackend::capabilities() const {
    AssistantCapabilities capabilities;
    capabilities.supports_text_conversation = true;
    capabilities.processing_route = ProcessingRoute::RemoteCloud;
    return capabilities;
}

AssistantResponse ClaudeCodeBackend::execute_request(const AssistantRequest& request) {
    AssistantResponse response;
    response.route = ProcessingRoute::RemoteCloud;

    const ScratchDirectory scratch;
    if (!scratch.ok()) {
        response.status = AssistantStatus::Error;
        response.error = "could not create an empty working directory for Claude Code CLI.";
        return response;
    }

    ProcessRequest process;
    process.executable = executable_path_;
    process.arguments = claude_print_arguments();
    process.stdin_text = request.prompt;
    process.working_directory = scratch.path();
    process.timeout_seconds = request.timeout_seconds;
    process.max_stdout_bytes = RESPONSE_MAX_BYTES;
    process.max_stderr_bytes = DIAGNOSTIC_MAX_BYTES;

    return map_claude_result(runner_.run(process), request.timeout_seconds, response);
}

AssistantCapabilities HermesBackend::capabilities() const {
    AssistantCapabilities capabilities;
    capabilities.supports_text_conversation = true;
    capabilities.processing_route = ProcessingRoute::Unknown;
    return capabilities;
}

AssistantResponse HermesBackend::execute_request(const AssistantRequest&) {
    AssistantResponse response;
    response.status = AssistantStatus::Unavailable;
    response.route = ProcessingRoute::Unknown;
    response.error = diagnostic();
    return response;
}

std::string HermesBackend::diagnostic() const {
    return "Hermes Agent CLI is disabled: no tool-free one-shot invocation has been verified, "
           "so requests fail closed. Inference route is unknown.";
}

} // namespace cipher
