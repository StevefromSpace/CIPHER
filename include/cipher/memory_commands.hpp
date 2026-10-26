#ifndef CIPHER_MEMORY_COMMANDS_HPP
#define CIPHER_MEMORY_COMMANDS_HPP

// ---------------------------------------------------------------------------
// cipher/memory_commands.hpp  —  REPL commands for persistent memory
//
// Handles the commands remember, recall, forget, and memories by driving a
// MemoryStore. This layer depends only on cipher_memory. cipher_core never
// sees it: the REPL (main.cpp) offers each input here first, then to the core.
//
// Invariants:
//   - Memory is opt-in. An empty file path disables every memory command.
//   - A file that fails to load blocks all writes, so a broken file is never
//     replaced by an empty store.
//   - A mutation that cannot be saved is rolled back, so the in-memory store
//     never diverges from the file on disk.
//   - remember and forget are gated by PermissionPolicy at RiskTier::Monitor.
//     A denied action returns before any store change or file write.
// ---------------------------------------------------------------------------

#include "cipher/assistant_backend.hpp"
#include "cipher/memory.hpp"

#include <string>

namespace cipher {

struct MemoryCommandResult {
    // true: the input was a memory command and `text` is its complete reply.
    bool handled = false;
    // When !handled, `text` is an addendum for the core reply (used by help).
    std::string text;
};

class MemoryCommands {
public:
    // The policy decides whether remember and forget may run. Its blacklist and
    // whitelist are authoritative over the Monitor default.
    explicit MemoryCommands(std::string file_path, PermissionPolicy policy = PermissionPolicy{});

    // Loads the store from the configured file. An empty path succeeds and
    // leaves memory disabled. A missing file succeeds with an empty store.
    // A file that exists but cannot be parsed returns errors and disables writes.
    MemoryResult open();

    MemoryCommandResult execute(const std::string& input);

private:
    bool enabled() const noexcept { return !file_path_.empty(); }
    std::string run(const std::string& command, const std::string& arguments);
    std::string remember(const std::string& id, const std::string& content);
    std::string recall(const std::string& id) const;
    std::string forget(const std::string& id);
    std::string list_ids() const;
    std::string save_or_roll_back(const MemoryStore& before);
    // Empty string when the action may run; otherwise the denial message.
    std::string check_permission(const std::string& action, const std::string& id) const;

    std::string file_path_;
    PermissionPolicy policy_;
    MemoryStore store_;
    bool available_ = false;  // true only after a successful open()
};

} // namespace cipher

#endif // CIPHER_MEMORY_COMMANDS_HPP
