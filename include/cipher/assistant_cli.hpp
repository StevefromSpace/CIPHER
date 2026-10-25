#ifndef CIPHER_ASSISTANT_CLI_HPP
#define CIPHER_ASSISTANT_CLI_HPP

// ---------------------------------------------------------------------------
// cipher/assistant_cli.hpp  —  text-only assistant adapters for installed CLIs
//
// Claude Code CLI (ClaudeCodeBackend):
//   Hosted Anthropic inference, so the route is RemoteCloud. Each request runs
//   `claude --print` with every built-in tool, MCP server, skill and settings
//   source disabled, from an empty working directory, with the prompt on stdin.
//   Permission-bypass flags are never used. Login is checked with
//   `claude auth status --json`. Login state is required, but billing is not
//   assumed: it depends on the account the CLI uses.
//
// Hermes Agent CLI (HermesBackend):
//   Always unavailable in this milestone. No tool-free one-shot invocation has
//   been verified, so the adapter fails closed. Its route is Unknown.
// ---------------------------------------------------------------------------

#include "cipher/assistant_backend.hpp"
#include "cipher/process_runner.hpp"

#include <optional>
#include <string>

namespace cipher {

struct ClaudeAuthStatus {
    bool logged_in{false};
    std::string auth_method;
};

// Parses `claude auth status --json`. Returns std::nullopt for malformed output.
// Only the loggedIn and authMethod fields are read; no other values are retained.
std::optional<ClaudeAuthStatus> parse_claude_auth_status(const std::string& json_text);

// Best-effort redaction for CLI diagnostics: drops tokens that look like
// credentials, strips control characters, and bounds the length.
std::string redact_diagnostic(const std::string& text);

// Finds a native executable on PATH. On Windows only *.exe is matched, so
// .cmd and .bat wrappers (which go through cmd.exe) are never selected.
// Returns an empty string when nothing is found.
std::string find_executable_on_path(const std::string& name);

class ClaudeCodeBackend : public IAssistantBackend {
public:
    ClaudeCodeBackend(std::string executable_path, IProcessRunner& runner);

    std::string backend_id() const override { return "claude"; }
    bool is_available() override;
    AssistantCapabilities capabilities() const override;
    AssistantResponse execute_request(const AssistantRequest& request) override;
    std::string diagnostic() const override { return diagnostic_; }

private:
    std::string executable_path_;
    IProcessRunner& runner_;
    std::string diagnostic_;
};

class HermesBackend : public IAssistantBackend {
public:
    std::string backend_id() const override { return "hermes"; }
    bool is_available() override { return false; }
    AssistantCapabilities capabilities() const override;
    AssistantResponse execute_request(const AssistantRequest& request) override;
    std::string diagnostic() const override;
};

} // namespace cipher

#endif // CIPHER_ASSISTANT_CLI_HPP
