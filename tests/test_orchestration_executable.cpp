// Executable resolver tests. Everything runs in fresh temp directories; no real tool is required.

#include "cipher/orchestration/executable_resolver.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using namespace cipher::orchestration;
namespace fs = std::filesystem;

int failures = 0;

void check(bool ok, const char* what) {
    std::cout << (ok ? "PASS: " : "FAIL: ") << what << "\n";
    if (!ok) ++failures;
}

#ifdef _WIN32
constexpr char kPathSeparator = ';';
const char* const kToolName = "cipher-probe-tool";
const char* const kToolFile = "cipher-probe-tool.exe";
#else
constexpr char kPathSeparator = ':';
const char* const kToolName = "cipher-probe-tool";
const char* const kToolFile = "cipher-probe-tool";
#endif

void write_file(const fs::path& path) {
    std::ofstream out(path);
    out << "probe\n";
}

void make_executable(const fs::path& path) {
#ifndef _WIN32
    fs::permissions(path, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add);
#else
    (void)path;
#endif
}

bool throws_runtime_error(const std::string& name, const std::string& path_list) {
    try {
        resolve_executable(name, path_list);
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

void test_finds_tool_in_path_entry(const fs::path& root) {
    const fs::path dir = root / "found";
    fs::create_directories(dir);
    write_file(dir / kToolFile);
    make_executable(dir / kToolFile);
    const std::string resolved = resolve_executable(kToolName, dir.string());
    check(resolved == (dir / kToolFile).string(), "resolver: finds the tool and returns its absolute path");
}

void test_skips_relative_and_empty_entries(const fs::path& root) {
    // The tool sits in a relative directory under the working directory: the resolver must not use it.
    const fs::path relative_dir = fs::path("cipher-resolver-probe-rel");
    fs::create_directories(relative_dir);
    write_file(relative_dir / kToolFile);
    make_executable(relative_dir / kToolFile);
    const std::string path_list = std::string() + kPathSeparator + relative_dir.string() + kPathSeparator;
    const bool threw = throws_runtime_error(kToolName, path_list);
    std::error_code ec;
    fs::remove_all(relative_dir, ec);
    check(threw, "resolver: relative and empty PATH entries are skipped");
    (void)root;
}

void test_missing_tool_throws(const fs::path& root) {
    const fs::path dir = root / "empty";
    fs::create_directories(dir);
    check(throws_runtime_error(kToolName, dir.string()), "resolver: a missing tool throws runtime_error");
}

void test_non_executable_is_rejected(const fs::path& root) {
    // On Windows the file lacks .exe, so it is not the tool. On POSIX it lacks the execute bit.
    const fs::path dir = root / "plain";
    fs::create_directories(dir);
    write_file(dir / kToolName);
    check(throws_runtime_error(kToolName, dir.string()),
          "resolver: a file without the .exe extension or execute bit is not the tool");
}

void test_rejects_path_like_name(const fs::path& root) {
    bool threw = false;
    try {
        resolve_executable("sub/" + std::string(kToolName), root.string());
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "resolver: a name with a directory part is rejected");
}

}  // namespace

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("cipher-resolver-test-" + std::to_string(stamp));
    fs::create_directories(root);

    test_finds_tool_in_path_entry(root);
    test_skips_relative_and_empty_entries(root);
    test_missing_tool_throws(root);
    test_non_executable_is_rejected(root);
    test_rejects_path_like_name(root);

    std::error_code ec;
    fs::remove_all(root, ec);
    std::cout << (failures == 0 ? "ALL PASSED" : "FAILURES") << "\n";
    return failures == 0 ? 0 : 1;
}
