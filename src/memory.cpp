// ---------------------------------------------------------------------------
// src/memory.cpp  —  Persistent Memory Subsystem Implementation
// ---------------------------------------------------------------------------

#include "cipher/memory.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace cipher {


namespace {

// Trim leading and trailing ASCII whitespace
std::string trim(const std::string& str) {
    auto start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    auto end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

// Current Unix epoch seconds
int64_t current_timestamp() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// Check if string consists purely of digits (optional leading sign)
bool is_integer(const std::string& s) {
    if (s.empty()) return false;
    size_t start = 0;
    if (s[0] == '-' || s[0] == '+') {
        if (s.size() == 1) return false;
        start = 1;
    }
    for (size_t i = start; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
            return false;
        }
    }
    return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

bool MemoryStore::is_valid_id(const std::string& id) noexcept {
    if (id.empty()) return false;
    for (char c : id) {
        if (!std::isalnum(static_cast<unsigned char>(c)) &&
            c != '_' && c != '-' && c != '.') {
            return false;
        }
    }
    return true;
}

bool MemoryStore::is_valid_category(const std::string& category) noexcept {
    if (category.empty()) return true; // defaults to "general"
    if (category.front() == ' ' || category.back() == ' ') return false;
    for (char c : category) {
        if (c == '\n' || c == '\r' || c == '\t' || static_cast<unsigned char>(c) < 32) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// In-Memory Inspection and Manipulation
// ---------------------------------------------------------------------------

std::optional<size_t> MemoryStore::find_index(const std::string& id) const noexcept {
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].id == id) {
            return i;
        }
    }
    return std::nullopt;
}

bool MemoryStore::has(const std::string& id) const noexcept {
    return find_index(id).has_value();
}

std::optional<MemoryEntry> MemoryStore::get(const std::string& id) const {
    auto idx = find_index(id);
    if (idx) {
        return entries_[*idx];
    }
    return std::nullopt;
}

void MemoryStore::clear() noexcept {
    entries_.clear();
    schema_version_ = CURRENT_SCHEMA_VERSION;
}

bool MemoryStore::set(const MemoryEntry& entry, std::string* error_msg) {
    if (!is_valid_id(entry.id)) {
        if (error_msg) {
            *error_msg = "Invalid memory entry ID '" + entry.id +
                         "': IDs must be non-empty and contain only alphanumeric, '_', '-', or '.' characters.";
        }
        return false;
    }

    if (!is_valid_category(entry.category)) {
        if (error_msg) {
            *error_msg = "Invalid memory category '" + entry.category +
                         "': categories must not contain control characters or leading/trailing whitespace.";
        }
        return false;
    }

    int64_t now = current_timestamp();
    auto idx = find_index(entry.id);

    MemoryEntry sanitized = entry;
    if (sanitized.category.empty()) {
        sanitized.category = "general";
    }

    if (idx) {
        // Update existing entry
        sanitized.created_at = (entry.created_at > 0) ? entry.created_at : entries_[*idx].created_at;
        sanitized.updated_at = (entry.updated_at > 0) ? entry.updated_at : now;
        entries_[*idx] = sanitized;
    } else {
        // Create new entry
        if (sanitized.created_at <= 0) sanitized.created_at = now;
        if (sanitized.updated_at <= 0) sanitized.updated_at = sanitized.created_at;
        entries_.push_back(sanitized);
    }

    return true;
}

bool MemoryStore::set(const std::string& id,
                      const std::string& content,
                      const std::string& category,
                      std::string* error_msg) {
    MemoryEntry entry;
    entry.id = id;
    entry.content = content;
    entry.category = category.empty() ? "general" : category;
    return set(entry, error_msg);
}

bool MemoryStore::remove(const std::string& id) {
    auto idx = find_index(id);
    if (!idx) {
        return false;
    }
    entries_.erase(entries_.begin() + *idx);
    return true;
}

// ---------------------------------------------------------------------------
// Escaping Helpers
// ---------------------------------------------------------------------------

std::string MemoryStore::escape_content(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 16);
    for (char c : input) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

std::string MemoryStore::unescape_content(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\\' && i + 1 < input.size()) {
            char next = input[i + 1];
            switch (next) {
                case '\\': out += '\\'; ++i; break;
                case 'n':  out += '\n'; ++i; break;
                case 'r':  out += '\r'; ++i; break;
                case 't':  out += '\t'; ++i; break;
                default:
                    // Unknown escape sequence, preserve literally
                    out += '\\';
                    out += next;
                    ++i;
                    break;
            }
        } else {
            out += input[i];
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

std::string MemoryStore::serialize() const {
    std::ostringstream ss;
    ss << "# CIPHER Persistent Memory Store\n";
    ss << "# Format: cipher-memory-v1\n";
    ss << "schema_version = " << CURRENT_SCHEMA_VERSION << "\n";
    ss << "entry_count = " << entries_.size() << "\n\n";

    for (const auto& entry : entries_) {
        ss << "[entry:" << entry.id << "]\n";
        ss << "category = " << entry.category << "\n";
        ss << "created_at = " << entry.created_at << "\n";
        ss << "updated_at = " << entry.updated_at << "\n";
        ss << "content = " << escape_content(entry.content) << "\n\n";
    }

    return ss.str();
}

// ---------------------------------------------------------------------------
// Parsing and Schema Migration
// ---------------------------------------------------------------------------

MemoryResult MemoryStore::load_from_string(const std::string& text) {
    MemoryResult result;
    std::istringstream ss(text);
    std::string line;
    int line_number = 0;

    int parsed_schema = -1;
    bool has_schema_header = false;
    int expected_entry_count = -1;

    std::vector<MemoryEntry> temp_entries;
    std::optional<MemoryEntry> current_entry;
    std::vector<std::string> seen_ids;

    // Check if the input is purely empty or whitespace
    bool has_any_content = false;

    while (std::getline(ss, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::string trimmed = trim(line);

        // Skip blank lines and comments
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        has_any_content = true;

        // Check for section header [entry:<id>]
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            std::string section_name = trimmed.substr(1, trimmed.size() - 2);

            // Commit previous entry if open
            if (current_entry) {
                temp_entries.push_back(*current_entry);
                current_entry.reset();
            }

            const std::string prefix = "entry:";
            if (section_name.rfind(prefix, 0) == 0) {
                std::string entry_id = section_name.substr(prefix.size());
                if (!is_valid_id(entry_id)) {
                    result.errors.push_back({line_number, "Invalid entry section identifier '" + entry_id + "'."});
                    continue;
                }

                // Check for duplicate entry IDs
                if (std::find(seen_ids.begin(), seen_ids.end(), entry_id) != seen_ids.end()) {
                    result.errors.push_back({line_number, "Duplicate memory entry ID '" + entry_id + "'."});
                    continue;
                }

                seen_ids.push_back(entry_id);
                current_entry = MemoryEntry{};
                current_entry->id = entry_id;
                current_entry->category = "general";
            } else {
                result.errors.push_back({line_number, "Unknown section header '[" + section_name + "]'."});
            }
            continue;
        }

        // Key = Value parsing
        auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            result.errors.push_back({line_number, "Syntax error: Expected 'key = value' or '[section]'."});
            continue;
        }

        std::string key = trim(line.substr(0, eq_pos));
        std::string raw_val = line.substr(eq_pos + 1);

        if (key.empty()) {
            result.errors.push_back({line_number, "Syntax error: Missing key before '='."});
            continue;
        }

        if (current_entry) {
            // Inside an entry section
            if (key == "content") {
                // Strip only the single space separator after '=' if present, preserving leading/trailing content spaces
                if (!raw_val.empty() && raw_val.front() == ' ') {
                    raw_val.erase(0, 1);
                }
                current_entry->content = unescape_content(raw_val);
            } else {
                std::string val = trim(raw_val);
                if (key == "category") {
                    if (!is_valid_category(val)) {
                        result.errors.push_back({line_number, "Invalid category '" + val + "': must not contain control characters or leading/trailing whitespace."});
                    } else {
                        current_entry->category = val.empty() ? "general" : val;
                    }
                } else if (key == "created_at") {
                    if (!is_integer(val)) {
                        result.errors.push_back({line_number, "Invalid created_at timestamp '" + val + "': must be an integer."});
                    } else {
                        try {
                            current_entry->created_at = std::stoll(val);
                        } catch (...) {
                            result.errors.push_back({line_number, "Timestamp out of range '" + val + "'."});
                        }
                    }
                } else if (key == "updated_at") {
                    if (!is_integer(val)) {
                        result.errors.push_back({line_number, "Invalid updated_at timestamp '" + val + "': must be an integer."});
                    } else {
                        try {
                            current_entry->updated_at = std::stoll(val);
                        } catch (...) {
                            result.errors.push_back({line_number, "Timestamp out of range '" + val + "'."});
                        }
                    }
                } else {
                    result.errors.push_back({line_number, "Unknown entry attribute '" + key + "'."});
                }
            }
        } else {
            // Top-level store metadata
            std::string val = trim(raw_val);
            if (key == "schema_version") {
                if (!is_integer(val)) {
                    result.errors.push_back({line_number, "Invalid schema_version value '" + val + "': must be an integer."});
                } else {
                    try {
                        parsed_schema = std::stoi(val);
                        has_schema_header = true;
                        if (parsed_schema < 0 || parsed_schema > CURRENT_SCHEMA_VERSION) {
                            result.errors.push_back({line_number, "Unsupported schema version " + val +
                                                    ". Supported versions: 0, " + std::to_string(CURRENT_SCHEMA_VERSION) + "."});
                        }
                    } catch (...) {
                        result.errors.push_back({line_number, "Schema version out of range: '" + val + "'."});
                    }
                }
            } else if (key == "entry_count") {
                if (!is_integer(val)) {
                    result.errors.push_back({line_number, "Invalid entry_count value '" + val + "'."});
                } else {
                    try {
                        expected_entry_count = std::stoi(val);
                    } catch (...) {
                        result.errors.push_back({line_number, "Entry count out of range: '" + val + "'."});
                    }
                }
            } else {
                // Legacy v0 format detection: flat key = value records without [entry:...] sections
                if (!has_schema_header) {
                    parsed_schema = LEGACY_SCHEMA_VERSION;
                }

                if (parsed_schema == LEGACY_SCHEMA_VERSION) {
                    // Legacy migration: key is ID, val is content
                    if (!is_valid_id(key)) {
                        result.errors.push_back({line_number, "Legacy entry ID '" + key + "' is invalid."});
                    } else if (std::find(seen_ids.begin(), seen_ids.end(), key) != seen_ids.end()) {
                        result.errors.push_back({line_number, "Duplicate legacy entry ID '" + key + "'."});
                    } else {
                        seen_ids.push_back(key);
                        MemoryEntry leg;
                        leg.id = key;
                        leg.category = "general";
                        // Strip only single space delimiter for legacy format too
                        if (!raw_val.empty() && raw_val.front() == ' ') {
                            raw_val.erase(0, 1);
                        }
                        leg.content = unescape_content(raw_val);
                        int64_t now = current_timestamp();
                        leg.created_at = now;
                        leg.updated_at = now;
                        temp_entries.push_back(leg);
                    }
                } else {
                    result.errors.push_back({line_number, "Unknown top-level property '" + key + "'."});
                }
            }
        }
    }

    // Flush last entry
    if (current_entry) {
        temp_entries.push_back(*current_entry);
    }

    // Check entry count if header explicitly provided one
    if (expected_entry_count >= 0 && static_cast<int>(temp_entries.size()) != expected_entry_count) {
        result.errors.push_back({line_number, "Mismatched entry count: header specifies " +
                                 std::to_string(expected_entry_count) + " entries, but " +
                                 std::to_string(temp_entries.size()) + " were parsed."});
    }

    // If completely empty file, treat as valid clean empty store
    if (!has_any_content) {
        clear();
        result.ok = true;
        return result;
    }

    if (!result.errors.empty()) {
        result.ok = false;
        return result;
    }

    // Schema Migration & Commit
    // If legacy schema 0 was parsed, automatically migrate to current schema 1
    if (parsed_schema == LEGACY_SCHEMA_VERSION) {
        schema_version_ = CURRENT_SCHEMA_VERSION;
    } else {
        schema_version_ = (parsed_schema != -1) ? parsed_schema : CURRENT_SCHEMA_VERSION;
    }

    entries_ = std::move(temp_entries);
    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// File I/O with Atomic Recoverable Write
// ---------------------------------------------------------------------------

namespace memory_testing {
namespace {
ExistsProbe g_exists_probe = nullptr;
} // namespace

void set_exists_probe(ExistsProbe probe) {
    g_exists_probe = probe;
}
} // namespace memory_testing

MemoryResult MemoryStore::load(const std::string& filepath) {
    MemoryResult result;
    std::error_code ec;

    const bool file_exists = memory_testing::g_exists_probe
        ? memory_testing::g_exists_probe(filepath, ec)
        : std::filesystem::exists(filepath, ec);
    if (ec) {
        // Inspection failed: existence is unknown, so never reset the store to empty
        result.errors.push_back({0, "Failed to inspect memory file: " + filepath + " (" + ec.message() + ")"});
        result.ok = false;
        return result;
    }

    if (!file_exists) {
        // Missing storage file is not an error — initialize clean empty store
        clear();
        result.ok = true;
        return result;
    }

    std::ifstream file(filepath, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        result.errors.push_back({0, "Failed to open memory file for reading: " + filepath});
        result.ok = false;
        return result;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    if (file.bad()) {
        result.errors.push_back({0, "I/O read failure while reading memory file: " + filepath});
        result.ok = false;
        return result;
    }
    return load_from_string(ss.str());
}

MemoryResult MemoryStore::save(const std::string& filepath) const {
    MemoryResult result;
    std::string serialized = serialize();

    std::string temp_path = filepath + ".tmp";

    // Write to temporary file first
    {
        std::ofstream tmp_file(temp_path, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!tmp_file.is_open()) {
            result.errors.push_back({0, "Failed to open temporary file for write: " + temp_path});
            result.ok = false;
            return result;
        }

        tmp_file.write(serialized.data(), serialized.size());
        tmp_file.flush();

        if (!tmp_file.good()) {
            result.errors.push_back({0, "Failed writing complete payload to temporary file: " + temp_path});
            tmp_file.close();
            std::error_code rm_ec;
            std::filesystem::remove(temp_path, rm_ec);
            result.ok = false;
            return result;
        }
    }

    // Atomic replacement: rename temporary file to target path
#if defined(_WIN32)
    std::wstring temp_w = std::filesystem::path(temp_path).wstring();
    std::wstring target_w = std::filesystem::path(filepath).wstring();

    if (!MoveFileExW(temp_w.c_str(), target_w.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD err = GetLastError();
        result.errors.push_back({0, "Failed to atomically replace memory file '" + filepath +
                                 "' (Win32 error " + std::to_string(err) + ")."});
        std::error_code rm_tmp_ec;
        std::filesystem::remove(temp_path, rm_tmp_ec);
        result.ok = false;
        return result;
    }
#else
    std::error_code rename_ec;
    std::filesystem::rename(temp_path, filepath, rename_ec);

    if (rename_ec) {
        result.errors.push_back({0, "Failed to atomically replace memory file '" + filepath +
                                 "': " + rename_ec.message()});
        std::error_code rm_tmp_ec;
        std::filesystem::remove(temp_path, rm_tmp_ec);
        result.ok = false;
        return result;
    }
#endif

    result.ok = true;
    return result;
}


} // namespace cipher
