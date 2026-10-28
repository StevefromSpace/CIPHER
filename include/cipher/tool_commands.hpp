#ifndef CIPHER_TOOL_COMMANDS_HPP
#define CIPHER_TOOL_COMMANDS_HPP

#include "cipher/tool_registry.hpp"

#include <iostream>
#include <string>
#include <vector>

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
 * `memory.list` is registered only when a memory file is configured, and `filesystem.read`
 * only when a filesystem root is configured, so the tool set matches what the owner can use.
 *
 * Ask-tier tools are confirmed on this REPL: the owner must type exactly `yes`. EOF or any
 * other answer denies the call. The streams are parameters so tests can drive the prompt.
 *
 * `filesystem.read` refuses the memory file (and its `.tmp` save file) plus @p other_private_files
 * (the log and configuration files), so a root that overlaps them cannot expose CIPHER's own data.
 */
class ToolCommands {
public:
    ToolCommands(const std::string& memory_file_path, const std::string& filesystem_root_path,
                 std::istream& answers = std::cin, std::ostream& prompts = std::cout,
                 const std::vector<std::string>& other_private_files = {});

    // The registry's confirmation callback captures this object, so it must not be copied.
    ToolCommands(const ToolCommands&) = delete;
    ToolCommands& operator=(const ToolCommands&) = delete;

    ToolCommandResult execute(const std::string& input);

    /// Exposed so the owner-side policy can be adjusted in tests and future config.
    ToolRegistry& registry() noexcept { return registry_; }

private:
    std::string list() const;
    std::string log() const;
    std::string invoke(const std::string& id, const std::string& arguments);
    bool confirm(const ToolDefinition& definition, const std::string& target);

    ToolRegistry registry_;
    std::istream& answers_;
    std::ostream& prompts_;
};

} // namespace cipher

#endif // CIPHER_TOOL_COMMANDS_HPP
