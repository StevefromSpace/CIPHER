#ifndef CIPHER_CONFIG_HPP
#define CIPHER_CONFIG_HPP

// ---------------------------------------------------------------------------
// cipher/config.hpp  —  Configuration subsystem
//
// CIPHER reads an optional "cipher.conf" file from the current working
// directory.  When the file is absent, sensible defaults are used and startup
// continues normally.  When the file exists but contains errors, startup
// stops and an error message is printed — silent fallback after a broken
// configuration would hide problems.
//
// File format (key = value):
//   - Lines beginning with '#' (after optional leading whitespace) are
//     comments and are ignored.
//   - Blank lines and whitespace-only lines are ignored.
//   - Each setting is "key = value" (whitespace around '=' is trimmed).
//   - Unknown keys are rejected with a line-number error message.
//   - Values are validated; malformed values are rejected with a message.
//
// Supported keys:
//   log_level   — one of: debug, info, warn, error   (default: info)
//   log_file    — path to an optional log file        (default: empty = stderr only)
//   memory_file — path to the persistent memory file  (default: empty = memory disabled)
//   filesystem_read_root — folder that filesystem.read may read from (default: empty = tool unavailable)
//   ollama_port — 127.0.0.1 port of a local Ollama server for the `ask` command (default: 0 = disabled)
//
// What is NOT stored here:
//   Passwords, API keys, tokens, or any other secrets.  The example file
//   documents this restriction explicitly.
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

namespace cipher {

// ---------------------------------------------------------------------------
// LogLevel
// The four severity levels used by the logger.  Levels are ordered so that
// numeric comparison works: DEBUG < INFO < WARN < ERROR.
// ---------------------------------------------------------------------------
enum class LogLevel {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    ERROR = 3,
};

// Returns the canonical lower-case name of a LogLevel ("debug", "info", …).
const char* log_level_name(LogLevel level) noexcept;

// Parses a lower-case string into a LogLevel.
// Returns true and sets `out` on success; returns false on an unknown name.
bool parse_log_level(const std::string& text, LogLevel& out) noexcept;

// ---------------------------------------------------------------------------
// Config
// Plain data struct that holds every validated configuration value.
// Construct it via ConfigParser::load() or ConfigParser::defaults().
// ---------------------------------------------------------------------------
struct Config {
    LogLevel    log_level = LogLevel::INFO;  // Minimum level to emit
    std::string log_file;                    // Empty = stderr only
    std::string memory_file;                 // Empty = memory commands disabled
    std::string filesystem_read_root;        // Empty = filesystem.read not registered
    int         ollama_port = 0;             // 0 = `ask` disabled; otherwise 1-65535
};

// ---------------------------------------------------------------------------
// ConfigError
// Describes a single problem found while parsing a configuration file.
// ---------------------------------------------------------------------------
struct ConfigError {
    int         line;     // 1-based line number in the source file (0 = file-level)
    std::string message;  // Human-readable description
};

// ---------------------------------------------------------------------------
// ConfigResult
// Returned by ConfigParser::load().  On success, errors is empty.
// On failure, config holds the last consistent defaults (not a partial parse).
// ---------------------------------------------------------------------------
struct ConfigResult {
    Config                   config;   // Valid config (defaults if load failed)
    std::vector<ConfigError> errors;   // Non-empty when parsing failed

    bool ok() const noexcept { return errors.empty(); }
};

// ---------------------------------------------------------------------------
// ConfigParser
// Stateless helper that reads a key=value file and produces a ConfigResult.
//
// Usage — from a real file:
//   ConfigResult result = ConfigParser::load("cipher.conf");
//
// Usage — from a string (useful in tests):
//   ConfigResult result = ConfigParser::load_from_string("log_level = debug\n");
//
// Usage — get plain defaults (no file):
//   Config cfg = ConfigParser::defaults();
// ---------------------------------------------------------------------------
class ConfigParser {
public:
    // Returns a Config populated with compile-time defaults.
    static Config defaults() noexcept;

    // Reads a file by path and parses it.
    // If the file does not exist, returns defaults() with no errors.
    // If the file cannot be opened for another reason, returns an error.
    static ConfigResult load(const std::string& path);

    // Parses configuration text supplied directly (used in tests).
    static ConfigResult load_from_string(const std::string& text);
};

} // namespace cipher

#endif // CIPHER_CONFIG_HPP
