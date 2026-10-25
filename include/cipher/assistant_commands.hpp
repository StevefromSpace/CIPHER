#ifndef CIPHER_ASSISTANT_COMMANDS_HPP
#define CIPHER_ASSISTANT_COMMANDS_HPP

// ---------------------------------------------------------------------------
// cipher/assistant_commands.hpp  —  REPL commands for optional assistant providers
//
// Handles `ask claude <prompt>`, `ask hermes <prompt>` and `ai list`. Like the
// memory commands, this layer is offered each input before cipher_core.
//
// Invariants:
//   - The provider named in `ask` is the only one contacted. Failures never
//     fall back to another provider.
//   - Every request goes through AssistantManager, so central validation applies.
//   - Requests are sent as text only. Nothing is stored, and no memory, files
//     or repository content are added to the prompt.
//   - The CIPHER native runtime is listed as not implemented. `ask native` never
//     sends anything.
// ---------------------------------------------------------------------------

#include "cipher/assistant_cli.hpp"
#include "cipher/process_runner.hpp"

#include <string>

namespace cipher {

struct AssistantCommandResult {
    // true: the input was an assistant command and `text` is its complete reply.
    bool handled = false;
    // When !handled, `text` is an addendum for the core reply (used by help).
    std::string text;
};

class AssistantCommands {
public:
    // `claude_executable` is the resolved Claude Code CLI path. Empty means not installed.
    AssistantCommands(IProcessRunner& runner, const std::string& claude_executable);

    AssistantCommandResult execute(const std::string& input);

private:
    std::string ask(const std::string& arguments);
    std::string list_providers();
    std::string submit(IAssistantBackend& backend, const std::string& prompt);

    ClaudeCodeBackend claude_;
    HermesBackend hermes_;
};

} // namespace cipher

#endif // CIPHER_ASSISTANT_COMMANDS_HPP
