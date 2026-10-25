#ifndef CIPHER_MEMORY_HPP
#define CIPHER_MEMORY_HPP

// ---------------------------------------------------------------------------
// cipher/memory.hpp  —  Persistent Memory Subsystem (v0.3 Foundation)
//
// CIPHER Persistent Memory stores explicit, user-authorized memory records in
// a transparent, portable, human-readable local text format.
//
// Invariants & Boundaries:
//   - Local-first & zero cloud dependencies.
//   - Stores ONLY explicit, owner-provided memory entries.
//   - Never automatically records secrets, credentials, full conversations,
//     or unapproved background observations.
//   - Fully decoupled from cipher_core, cipher_config, and cipher_logger.
//   - Atomic, recoverable file writes: saving never destroys valid data on failure.
//   - Explicit schema versioning and deterministic migration from legacy schemas.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace cipher {

// ---------------------------------------------------------------------------
// MemoryEntry
// Represents a single explicit persistent memory record.
// ---------------------------------------------------------------------------
struct MemoryEntry {
    std::string id;          // Unique identifier/key (non-empty, alphanumeric + underscores/hyphens)
    std::string category;    // Category (e.g., "preference", "fact", "guideline", "general")
    std::string content;     // The stored text payload
    int64_t     created_at = 0; // Unix epoch seconds
    int64_t     updated_at = 0; // Unix epoch seconds

    bool operator==(const MemoryEntry& other) const noexcept {
        return id == other.id &&
               category == other.category &&
               content == other.content &&
               created_at == other.created_at &&
               updated_at == other.updated_at;
    }
};

// ---------------------------------------------------------------------------
// MemoryError
// Describes a single validation, syntax, or I/O problem with line reporting.
// ---------------------------------------------------------------------------
struct MemoryError {
    int         line = 0;    // 1-based line number (0 = file/system level)
    std::string message;     // Human-readable description
};

// ---------------------------------------------------------------------------
// MemoryResult
// Result returned by load and save operations.
// ---------------------------------------------------------------------------
struct MemoryResult {
    bool                     ok = false;
    std::vector<MemoryError> errors;

    bool is_ok() const noexcept { return ok && errors.empty(); }
};

// ---------------------------------------------------------------------------
// MemoryStore
// In-memory store and file serialization engine for persistent memory entries.
// ---------------------------------------------------------------------------
class MemoryStore {
public:
    static constexpr int CURRENT_SCHEMA_VERSION = 1;
    static constexpr int LEGACY_SCHEMA_VERSION = 0;

    MemoryStore() = default;

    // Inspection
    bool has(const std::string& id) const noexcept;
    std::optional<MemoryEntry> get(const std::string& id) const;
    const std::vector<MemoryEntry>& all() const noexcept { return entries_; }
    size_t size() const noexcept { return entries_.size(); }
    bool empty() const noexcept { return entries_.empty(); }
    int schema_version() const noexcept { return schema_version_; }

    // In-memory modification
    void clear() noexcept;

    // Sets an entry. If entry with `id` exists, updates content, category, and updated_at.
    // If not, creates new entry. Validates id and fields.
    bool set(const MemoryEntry& entry, std::string* error_msg = nullptr);
    bool set(const std::string& id,
             const std::string& content,
             const std::string& category = "general",
             std::string* error_msg = nullptr);

    // Removes an entry by ID. Returns true if removed, false if not found.
    bool remove(const std::string& id);

    // Serialization & Parsing
    std::string serialize() const;
    MemoryResult load_from_string(const std::string& text);

    // File I/O
    // If file does not exist, returns ok with empty store.
    MemoryResult load(const std::string& filepath);

    // Saves current store to file using atomic replacement (writes to .tmp and renames).
    MemoryResult save(const std::string& filepath) const;

    // Escaping helpers for special characters and multiline text
    static std::string escape_content(const std::string& input);
    static std::string unescape_content(const std::string& input);

    // Validation helpers
    static bool is_valid_id(const std::string& id) noexcept;
    static bool is_valid_category(const std::string& category) noexcept;

private:
    int                      schema_version_ = CURRENT_SCHEMA_VERSION;
    std::vector<MemoryEntry> entries_;

    // Helper for finding entry index
    std::optional<size_t> find_index(const std::string& id) const noexcept;
};

// Test-only seam: replaces the existence probe used by MemoryStore::load().
// nullptr restores std::filesystem::exists. Production code never sets it.
namespace memory_testing {
using ExistsProbe = bool (*)(const std::string& path, std::error_code& ec);
void set_exists_probe(ExistsProbe probe);
} // namespace memory_testing

} // namespace cipher

#endif // CIPHER_MEMORY_HPP
