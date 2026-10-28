#include "cipher/filesystem_tools.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cipher {
namespace {

namespace fs = std::filesystem;

// shortcut: whole-file read capped at 64 KiB, upgrade with a streaming reader when larger files are needed
constexpr std::uintmax_t MAX_READ_BYTES = 64 * 1024;

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

// Windows junctions and mount points are reparse points that std::filesystem may not
// report as symlinks, so the attribute is checked directly. POSIX has no junctions.
bool is_reparse_point_or_symlink(const fs::path& path) {
    if (fs::is_symlink(path)) {
        return true;
    }
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return false;
#endif
}

// Returns the canonical path of a regular file under the canonical root.
// Throws std::runtime_error with the reason when the argument is refused.
fs::path resolve_under_root(const fs::path& canonical_root, const std::string& argument,
                            const std::vector<std::string>& private_files) {
    const std::string relative_text = trim(argument);
    if (relative_text.empty()) {
        throw std::runtime_error("filesystem.read needs a relative path under the allowed folder.");
    }

    const fs::path relative(relative_text);
    if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
        throw std::runtime_error("filesystem.read accepts only relative paths.");
    }
    // shortcut: ':' is refused outright so Windows alternate data streams (file.txt:stream) cannot be read
    if (relative_text.find(':') != std::string::npos) {
        throw std::runtime_error("filesystem.read refuses ':' in paths.");
    }

    fs::path current = canonical_root;
    for (const fs::path& component : relative) {
        if (component == ".." || component == ".") {
            throw std::runtime_error("filesystem.read refuses '.' and '..' path components.");
        }
        current /= component;
        if (is_reparse_point_or_symlink(current)) {
            throw std::runtime_error("filesystem.read refuses symlinks and junctions.");
        }
    }

    if (!fs::is_regular_file(current)) {
        throw std::runtime_error("filesystem.read: not a regular file under the allowed folder.");
    }

    // Final check on the canonical result: it must still sit under the canonical root.
    const fs::path canonical_file = fs::canonical(current);
    const std::string root_prefix = canonical_root.string() + static_cast<char>(fs::path::preferred_separator);
    if (canonical_file.string().compare(0, root_prefix.size(), root_prefix) != 0) {
        throw std::runtime_error("filesystem.read: path resolves outside the allowed folder.");
    }

    // Memory, log and config files are CIPHER's own data and stay behind memory_deny and the owner's REPL.
    // equivalent() compares file identity, so case, 8.3 names and hard links cannot dodge it; a private
    // path that does not exist yet yields an error and no match, and is checked again on the next call.
    for (const std::string& private_file : private_files) {
        std::error_code error;
        if (!private_file.empty() && fs::equivalent(canonical_file, private_file, error) && !error) {
            throw std::runtime_error("filesystem.read refuses CIPHER's own memory, log and configuration files.");
        }
    }
    return canonical_file;
}

// Canonical root is computed per call, so a missing folder denies the call instead of stopping startup.
fs::path canonical_root_or_throw(const std::string& root) {
    std::error_code error;
    const fs::path canonical = fs::canonical(root, error);
    if (error) {
        throw std::runtime_error("filesystem.read: the allowed folder could not be opened.");
    }
    return canonical;
}

} // namespace

void register_filesystem_tools(ToolRegistry& registry, const std::string& root,
                               std::vector<std::string> private_files) {
    registry.register_tool(ToolDefinition{
        "filesystem.read",
        "Reads one text file under the allowed folder. Arguments are a relative path.",
        "filesystem",
        RiskTier::Monitor,
        SideEffect::ReadLocal,
        [root, private_files](const std::string& arguments) {
            const fs::path file = resolve_under_root(canonical_root_or_throw(root), arguments, private_files);
            if (fs::file_size(file) > MAX_READ_BYTES) {
                throw std::runtime_error("filesystem.read: file is larger than the 64 KiB limit.");
            }
            std::ifstream stream(file, std::ios::binary);
            if (!stream) {
                throw std::runtime_error("filesystem.read: file could not be opened.");
            }
            return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        },
        [root, private_files](const std::string& arguments) {
            return resolve_under_root(canonical_root_or_throw(root), arguments, private_files).string();
        }});
}

} // namespace cipher
