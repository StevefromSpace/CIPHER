#include "cipher/cipher_core.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace cipher {
namespace {

std::string trim_left(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return value.substr(first);
}

std::string current_local_time() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    const std::tm* local = std::localtime(&time);

    if (local == nullptr) {
        return "The local system time is unavailable.";
    }

    std::ostringstream output;
    output << std::put_time(local, "%Y-%m-%d %H:%M:%S");
    return output.str();
}

} // namespace

CommandResponse CipherCore::execute(const std::string& input) const {
    const std::string trimmed = trim_left(input);
    if (trimmed.empty()) {
        return {"", false};
    }

    const auto separator = trimmed.find_first_of(" \t");
    const std::string command = trimmed.substr(0, separator);
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
            "No network, shell, file-writing, or AI-provider access is enabled in v0.1.0.",
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
            "  Persistent memory: not implemented\n"
            "  Network access: disabled\n"
            "  Computer-control tools: disabled\n"
            "  External AI-provider dependency: none",
            false};
    }

    if (command == "time") {
        return {"Local system time: " + current_local_time(), false};
    }

    if (command == "echo") {
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
