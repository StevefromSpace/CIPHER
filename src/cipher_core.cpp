#include "cipher/cipher_core.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace cipher {
namespace {

// ---------------------------------------------------------------------------
// trim_left
// Removes leading ASCII whitespace (space, tab, carriage return, newline)
// from a string and returns the result. Does not modify the original.
// ---------------------------------------------------------------------------
std::string trim_left(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return value.substr(first);
}

// ---------------------------------------------------------------------------
// current_local_time
// Returns the local wall-clock time formatted as "YYYY-MM-DD HH:MM:SS".
//
// Why the platform split?
//   std::localtime() is not thread-safe (it uses a shared internal buffer)
//   and MSVC marks it deprecated, producing a warning.  The replacements are:
//     - localtime_s()  Windows / MSVC  (writes into caller-supplied tm struct)
//     - localtime_r()  POSIX / GCC / Clang (same idea, different argument
//                      order)
//   Both functions are C++17-compatible and introduced no new dependencies.
//   The #if guards are the standard way to detect the platform in C++.
// ---------------------------------------------------------------------------
std::string current_local_time() {
    const auto now  = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);

    std::tm local_tm{};   // Zero-initialise so every field starts clean.

#if defined(_WIN32)
    // Windows / MSVC: localtime_s writes the result into local_tm.
    // It returns non-zero on error.
    if (localtime_s(&local_tm, &t) != 0) {
        return "The local system time is unavailable.";
    }
#else
    // POSIX (Linux, macOS, etc.): localtime_r takes the time first, then
    // writes into the supplied struct. Returns nullptr on error.
    if (localtime_r(&t, &local_tm) == nullptr) {
        return "The local system time is unavailable.";
    }
#endif

    std::ostringstream output;
    output << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S");
    return output.str();
}

} // namespace

CommandResponse CipherCore::execute(const std::string& input) const {
    // Commands are case-sensitive: "Help" and "HELP" are not recognised.
    // This is intentional and keeps parsing simple and predictable.
    const std::string trimmed = trim_left(input);
    if (trimmed.empty()) {
        return {"", false};
    }

    const auto separator = trimmed.find_first_of(" \t");
    const std::string command   = trimmed.substr(0, separator);
    const std::string arguments = separator == std::string::npos
                                      ? ""
                                      : trim_left(trimmed.substr(separator + 1));

    if (command == "help") {
        return {
            "Available commands:\n"
            "  help              Show this help message\n"
            "  about             Show what CIPHER is\n"
            "  status            Show current core capabilities\n"
            "  time              Show the local system time\n"
            "  echo <text>       Print text back to the console\n"
            "  exit / quit       Close CIPHER\n\n"
            "File writes are limited to the optional log_file and memory_file. No network, shell, or AI-provider access is enabled in v" + std::string(version()) + ", except the opt-in ask and ai commands.",
            false};
    }

    if (command == "about") {
        return {
            "CIPHER v" + std::string(version()) + "\n"
            "Contextual Intelligence Platform for Home, Enterprise & Research\n"
            "A local-first assistant platform being built in small, testable steps.",
            false};
    }

    if (command == "status") {
        return {
            "CIPHER status\n"
            "  Core: online\n"
            "  Version: " + std::string(version()) + "\n"
            "  AI runtime: not connected\n"
            "  Persistent memory: optional (memory_file in cipher.conf)\n"
            "  Network access: disabled\n"
            "  Computer-control tools: disabled\n"
            "  External AI-provider dependency: none",
            false};
    }

    if (command == "time") {
        return {"Local system time: " + current_local_time(), false};
    }

    if (command == "echo") {
        // Deliberately returns an empty string when no argument is given.
        // echo with no argument is a valid command; it produces no output.
        return {arguments, false};
    }

    if (command == "exit" || command == "quit") {
        return {"CIPHER shutting down. Session ended.", true};
    }

    return {
        "Unknown command: " + command + "\nType 'help' to see the available commands.",
        false};
}

} // namespace cipher
