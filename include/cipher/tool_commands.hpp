#ifndef CIPHER_TOOL_COMMANDS_HPP
#define CIPHER_TOOL_COMMANDS_HPP

#include "cipher/tool_registry.hpp"

#include <string>

namespace cipher {

/**
 * @brief Result of offering one REPL line to ToolCommands. Same contract as MemoryCommandResult:
 *        when handled is false, the line belongs to the core.
 */
struct ToolCommandResult {
    bool handled{false};
    std::string text;
};

/**
 * @brief Owner-typed REPL commands for the tool registry:
 *        `tool list`, `tool log`, and `tool <id> [args]`.
 *
 * Only the owner's typed input reaches this class. Providers and the core never call it.
 * Invocations pass through the registry's permission gate before any handler runs.
 *
 * `memory.list` is registered only when a memory file is configured, so the tool set
 * matches what the owner can actually use.
 */
class ToolCommands {
public:
    explicit ToolCommands(const std::string& memory_file_path);

    ToolCommandResult execute(const std::string& input);

    /// Exposed so the owner-side policy can be adjusted in tests and future config.
    ToolRegistry& registry() noexcept { return registry_; }

private:
    std::string list() const;
    std::string log() const;
    std::string invoke(const std::string& id, const std::string& arguments);

    ToolRegistry registry_;
};

} // namespace cipher

#endif // CIPHER_TOOL_COMMANDS_HPP
