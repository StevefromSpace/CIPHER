#include "cipher/config.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

// ---------------------------------------------------------------------------
// config.cpp  —  Implementation of the configuration parser
//
// Reading guide for a C++ learner:
//
//  The parser works line by line.  For each line it:
//    1. Strips leading/trailing whitespace.
//    2. Skips blank lines and comment lines (starting with '#').
//    3. Splits on the first '=' to get a key and a value.
//    4. Validates the key against a fixed allowlist.
//    5. Validates the value and stores it in the Config struct.
//
//  If any line fails validation the parse is considered failed.  The caller
//  receives the list of errors and a default Config (not a partial result)
//  so the application always starts from a consistent state.
// ---------------------------------------------------------------------------

namespace cipher {
namespace {

// ---------------------------------------------------------------------------
// trim
// Removes leading and trailing ASCII whitespace from a string.
// Returns the trimmed copy.
// ---------------------------------------------------------------------------
std::string trim(const std::string& s) {
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    const auto first = std::find_if_not(s.begin(), s.end(), is_space);
    if (first == s.end()) {
        return {};
    }
    const auto last = std::find_if_not(s.rbegin(), s.rend(), is_space);
    return std::string(first, last.base());
}

// ---------------------------------------------------------------------------
// to_lower
// Returns a lower-cased copy of the string.  Only works correctly for ASCII,
// which is sufficient for configuration key and value names.
// ---------------------------------------------------------------------------
std::string to_lower(const std::string& s) {
    std::string result = s;
    for (char& c : result) {
        c = static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

// ---------------------------------------------------------------------------
// parse_lines
// Core parsing logic shared by load() and load_from_string().
// Returns a ConfigResult; errors is non-empty if any line failed.
// ---------------------------------------------------------------------------
ConfigResult parse_lines(std::istream& stream) {
    ConfigResult result;
    result.config = ConfigParser::defaults();  // Start with valid defaults.

    std::string line;
    int line_number = 0;

    while (std::getline(stream, line)) {
        ++line_number;

        // Remove a trailing '\r' so Windows-style CRLF files work on both
        // platforms.  std::getline strips '\n'; we strip '\r' here.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const std::string trimmed = trim(line);

        // Skip blank lines and comments.
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }

        // Every non-comment line must contain '='.
        const auto eq_pos = trimmed.find('=');
        if (eq_pos == std::string::npos) {
            result.errors.push_back({
                line_number,
                "Expected 'key = value' but found no '=' on this line."
            });
            continue;
        }

        const std::string key   = trim(to_lower(trimmed.substr(0, eq_pos)));
        const std::string value = trim(trimmed.substr(eq_pos + 1));

        // ---------------------------------------------------------------
        // Dispatch on the key.
        // ---------------------------------------------------------------
        if (key == "log_level") {
            LogLevel parsed_level{};
            if (!parse_log_level(to_lower(value), parsed_level)) {
                result.errors.push_back({
                    line_number,
                    "Invalid log_level '" + value +
                    "'. Valid values: debug, info, warn, error."
                });
            } else {
                result.config.log_level = parsed_level;
            }

        } else if (key == "log_file") {
            // Any non-empty string is a valid file path at parse time.
            // The Logger will report an error at runtime if the path is
            // inaccessible.  An empty value disables file logging (same as
            // the default).
            result.config.log_file = value;

        } else if (key == "memory_file") {
            // Any non-empty string is accepted here. The memory layer reports
            // load or save failures at runtime. An empty value disables memory.
            result.config.memory_file = value;

        } else if (key == "filesystem_read_root") {
            // Any non-empty string is accepted here. The tool checks the folder at call time.
            result.config.filesystem_read_root = value;

        } else {
            // Reject unknown keys so that misspelled settings are visible.
            result.errors.push_back({
                line_number,
                "Unknown configuration key '" + key +
                "'. Check the spelling against cipher.conf.example."
            });
        }
    }

    // If any errors occurred, reset the config to safe defaults so the caller
    // always receives a consistent state.
    if (!result.ok()) {
        result.config = ConfigParser::defaults();
    }

    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

const char* log_level_name(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::DEBUG: return "debug";
        case LogLevel::INFO:  return "info";
        case LogLevel::WARN:  return "warn";
        case LogLevel::ERROR: return "error";
    }
    return "info"; // unreachable, satisfies compiler
}

bool parse_log_level(const std::string& text, LogLevel& out) noexcept {
    if (text == "debug") { out = LogLevel::DEBUG; return true; }
    if (text == "info")  { out = LogLevel::INFO;  return true; }
    if (text == "warn")  { out = LogLevel::WARN;  return true; }
    if (text == "error") { out = LogLevel::ERROR; return true; }
    return false;
}

Config ConfigParser::defaults() noexcept {
    Config cfg;
    cfg.log_level = LogLevel::INFO;
    cfg.log_file  = "";
    cfg.memory_file = "";
    cfg.filesystem_read_root = "";
    return cfg;
}

ConfigResult ConfigParser::load(const std::string& path) {
    // std::ifstream cannot tell "missing" from "unreadable", so check existence first.
    // A missing file is optional and yields defaults; an existing file that cannot
    // be read is an error and is never silently replaced by defaults.
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    ConfigResult result;

    if (!ec && !exists) {
        result.config = defaults();
        return result;
    }

    std::ifstream file(path);
    if (ec || !file.is_open()) {
        result.config = defaults();
        result.errors.push_back({0, "Cannot read configuration file '" + path +
                                     "'. Check that it exists and is readable."});
        return result;
    }

    return parse_lines(file);
}

ConfigResult ConfigParser::load_from_string(const std::string& text) {
    std::istringstream stream(text);
    return parse_lines(stream);
}

} // namespace cipher
