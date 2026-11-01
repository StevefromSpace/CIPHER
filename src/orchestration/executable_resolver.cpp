#include "cipher/orchestration/executable_resolver.hpp"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace cipher::orchestration {

namespace {

namespace fs = std::filesystem;

#ifdef _WIN32
constexpr char kPathListSeparator = ';';
#else
constexpr char kPathListSeparator = ':';
#endif

// A bare name has no directory part, so it cannot point outside the PATH search.
bool is_bare_name(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\:") == std::string::npos;
}

// Must be a regular file. On POSIX it also needs the execute bit for the caller.
bool is_runnable(const fs::path& candidate) {
    std::error_code ec;
    if (!fs::is_regular_file(candidate, ec) || ec) return false;
#ifdef _WIN32
    return true;
#else
    return access(candidate.c_str(), X_OK) == 0;
#endif
}

}  // namespace

std::string resolve_executable(const std::string& name, const std::string& path_list) {
    if (!is_bare_name(name)) throw std::invalid_argument("executable name must be a bare file name: " + name);

    std::vector<std::string> file_names{name};
#ifdef _WIN32
    if (fs::path(name).extension().empty()) file_names = {name + ".exe"};
#endif

    for (std::size_t start = 0; start <= path_list.size();) {
        std::size_t end = path_list.find(kPathListSeparator, start);
        if (end == std::string::npos) end = path_list.size();
        const fs::path entry(path_list.substr(start, end - start));
        start = end + 1;
        if (entry.empty() || !entry.is_absolute()) continue;
        for (const std::string& file_name : file_names) {
            const fs::path candidate = entry / file_name;
            if (is_runnable(candidate)) return candidate.string();
        }
    }
    throw std::runtime_error("executable not found on PATH: " + name);
}

std::string resolve_executable_on_path(const std::string& name) {
#ifdef _WIN32
    char* path = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&path, &length, "PATH") != 0 || path == nullptr) return resolve_executable(name, "");
    const std::string path_list(path);
    std::free(path);
    return resolve_executable(name, path_list);
#else
    const char* path = std::getenv("PATH");
    return resolve_executable(name, path ? path : "");
#endif
}

}  // namespace cipher::orchestration
