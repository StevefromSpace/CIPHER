#include "cipher/config.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <locale>
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

// Accepts only plain decimal digits in 1..max, so "+80", "80x" or "-1" are rejected.
bool parse_positive_int(const std::string& text, long long max, int& out) {
    if (text.empty() || text.size() > 10) return false;  // 10 digits cannot overflow long long
    long long value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    if (value < 1 || value > max) return false;
    out = static_cast<int>(value);
    return true;
}

bool parse_port(const std::string& text, int& out) { return parse_positive_int(text, 65535, out); }

// A token count llama.cpp takes as int32_t.
bool parse_token_count(const std::string& text, int& out) {
    return parse_positive_int(text, std::numeric_limits<std::int32_t>::max(), out);
}

// A finite, non-negative decimal such as "0.8"; "." is the separator whatever the system locale.
bool parse_temperature(const std::string& text, float& out) {
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    float value = 0.0f;
    if (!(stream >> value) || stream.peek() != std::char_traits<char>::eof()) return false;
    if (!std::isfinite(value) || value < 0.0f) return false;
    out = value;
    return true;
}

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

        } else if (key == "ollama_port") {
            int port = 0;
            if (!parse_port(value, port)) {
                result.errors.push_back({
                    line_number,
                    "Invalid ollama_port '" + value + "'. Use a number from 1 to 65535."
                });
            } else {
                result.config.ollama_port = port;
            }

        } else if (key == "model_tool_calls") {
            if (value != "true" && value != "false") {
                result.errors.push_back({line_number, "Invalid model_tool_calls '" + value + "'. Use true or false."});
            } else {
                result.config.model_tool_calls = value == "true";
            }

        } else if (key == "memory_deny") {
            if (value.empty()) {
                result.errors.push_back({line_number, "memory_deny needs a pattern, for example 'share *'."});
            } else {
                result.config.memory_deny.push_back(value);
            }

        } else if (key == "llama_model_path") {
            if (value.empty()) {
                result.errors.push_back({line_number, "llama_model_path needs the path of a local .gguf model file."});
            } else {
                result.config.llama.model_path = value;
            }

        } else if (key == "llama_context_tokens" || key == "llama_max_output_tokens") {
            int tokens = 0;
            if (!parse_token_count(value, tokens)) {
                result.errors.push_back({line_number, "Invalid " + key + " '" + value + "'. Use a whole number from 1 to 2147483647."});
            } else if (key == "llama_context_tokens") {
                result.config.llama.context_tokens = tokens;
            } else {
                result.config.llama.max_output_tokens = tokens;
            }

        } else if (key == "llama_temperature") {
            float temperature = 0.0f;
            if (!parse_temperature(value, temperature)) {
                result.errors.push_back({line_number, "Invalid llama_temperature '" + value + "'. Use a number of 0 or more, such as 0.8; 0 means greedy."});
            } else {
                result.config.llama.temperature = temperature;
            }

        } else {
            // Reject unknown keys so that misspelled settings are visible.
            result.errors.push_back({
                line_number,
                "Unknown configuration key '" + key +
                "'. Check the spelling against cipher.conf.example."
            });
        }
    }

    // One backend serves `ask`. Choosing between two silently would hide which one sees the owner's prompts.
    if (result.config.ollama_port != 0 && !result.config.llama.model_path.empty()) {
        result.errors.push_back({0, "Set either ollama_port or llama_model_path, not both. `ask` uses one local backend."});
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
    cfg.ollama_port = 0;
    cfg.model_tool_calls = false;
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
