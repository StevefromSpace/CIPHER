#include "cipher/filesystem_tools.hpp"
#include "cipher/tool_registry.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

// ---------------------------------------------------------------------------
// test_filesystem_tools.cpp  —  v0.5 Phase 4 read-only filesystem.read
//
// Covers reads under the allowlisted root, traversal and absolute-path
// denial with zero handler calls, missing files and missing roots, the size
// bound, and symlink or junction rejection. Links use a temp directory and
// are skipped (not failed) where the OS refuses to create them.
// ---------------------------------------------------------------------------

namespace {

namespace fs = std::filesystem;

int g_failures = 0;
int g_passes   = 0;

void pass(const std::string& name) {
    std::cout << "PASS: " << name << '\n';
    ++g_passes;
}

void fail(const std::string& name, const std::string& reason) {
    std::cerr << "FAIL: " << name << "\n  " << reason << '\n';
    ++g_failures;
}

void check(bool condition, const std::string& name, const std::string& reason) {
    if (condition) pass(name);
    else           fail(name, reason);
}

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// Creates a fresh temp layout: <base>/allowed (root) and <base>/outside (never allowed).
struct Sandbox {
    fs::path base;
    fs::path root;
    fs::path outside;

    Sandbox() {
        base = fs::temp_directory_path() / "cipher_fs_tools_test";
        fs::remove_all(base);
        root = base / "allowed";
        outside = base / "outside";
        fs::create_directories(root);
        fs::create_directories(outside);
        write_text(root / "notes.txt", "hello from allowed");
        write_text(root / "sub" / "deep.txt", "deep file");
        write_text(outside / "secret.txt", "SECRET-OUTSIDE");
    }

    ~Sandbox() {
        std::error_code ignored;
        fs::remove_all(base, ignored);
    }
};

cipher::ToolResult call(cipher::ToolRegistry& registry, const std::string& arguments) {
    return registry.invoke("filesystem.read", "", arguments);
}

void test_reads_file_under_root(const Sandbox& sandbox) {
    cipher::ToolRegistry registry;
    cipher::register_filesystem_tools(registry, sandbox.root.string());

    const cipher::ToolResult result = call(registry, "notes.txt");
    check(result.outcome == cipher::ToolOutcome::Completed && result.executed &&
              result.output == "hello from allowed",
          "filesystem.read: reads a file under the root",
          "Expected Completed with file contents");

    const cipher::ToolResult nested = call(registry, "sub/deep.txt");
    check(nested.outcome == cipher::ToolOutcome::Completed && nested.output == "deep file",
          "filesystem.read: reads a nested file under the root",
          "Expected Completed with nested file contents");
}

void test_denies_traversal_without_running_handler(const Sandbox& sandbox) {
    cipher::ToolRegistry registry;
    cipher::register_filesystem_tools(registry, sandbox.root.string());

    const cipher::ToolResult result = call(registry, "../outside/secret.txt");
    check(result.outcome == cipher::ToolOutcome::Denied && !result.executed &&
              result.output.find("SECRET-OUTSIDE") == std::string::npos,
          "filesystem.read: '..' traversal is denied and never runs",
          "Expected Denied, executed=false and no file contents");

    const cipher::ToolResult dotted = call(registry, "sub/../../outside/secret.txt");
    check(dotted.outcome == cipher::ToolOutcome::Denied && !dotted.executed,
          "filesystem.read: embedded '..' is denied",
          "Expected Denied for an embedded traversal");
}

void test_denies_absolute_paths(const Sandbox& sandbox) {
    cipher::ToolRegistry registry;
    cipher::register_filesystem_tools(registry, sandbox.root.string());

    const std::string absolute = (sandbox.outside / "secret.txt").string();
    const cipher::ToolResult result = call(registry, absolute);
    check(result.outcome == cipher::ToolOutcome::Denied && !result.executed,
          "filesystem.read: absolute path is denied",
          "Expected Denied for an absolute path");

    const cipher::ToolResult stream = call(registry, "notes.txt:hidden");
    check(stream.outcome == cipher::ToolOutcome::Denied && !stream.executed,
          "filesystem.read: alternate data stream syntax is denied",
          "Expected Denied for a ':' path");
}

void test_missing_file_and_empty_argument_are_denied(const Sandbox& sandbox) {
    cipher::ToolRegistry registry;
    cipher::register_filesystem_tools(registry, sandbox.root.string());

    const cipher::ToolResult missing = call(registry, "no-such-file.txt");
    check(missing.outcome == cipher::ToolOutcome::Denied && !missing.executed,
          "filesystem.read: missing file is denied before the handler",
          "Expected Denied for a missing file");

    const cipher::ToolResult empty = call(registry, "   ");
    check(empty.outcome == cipher::ToolOutcome::Denied && !empty.executed,
          "filesystem.read: empty argument is denied",
          "Expected Denied for an empty argument");

    const cipher::ToolResult directory = call(registry, "sub");
    check(directory.outcome == cipher::ToolOutcome::Denied && !directory.executed,
          "filesystem.read: a directory is not a regular file",
          "Expected Denied for a directory");
}

void test_missing_root_denies_instead_of_crashing(const Sandbox& sandbox) {
    const fs::path absent = sandbox.base / "does-not-exist";
    cipher::ToolRegistry registry;
    cipher::register_filesystem_tools(registry, absent.string());

    const cipher::ToolResult result = call(registry, "notes.txt");
    check(result.outcome == cipher::ToolOutcome::Denied && !result.executed,
          "filesystem.read: missing root denies the call",
          "Expected Denied when the allowed folder does not exist");
}

void test_size_bound_fails_after_permission(const Sandbox& sandbox) {
    write_text(sandbox.root / "big.txt", std::string(64 * 1024 + 1, 'x'));
    cipher::ToolRegistry registry;
    cipher::register_filesystem_tools(registry, sandbox.root.string());

    const cipher::ToolResult result = call(registry, "big.txt");
    check(result.outcome == cipher::ToolOutcome::Failed && result.executed &&
              result.output.find("64 KiB") != std::string::npos,
          "filesystem.read: file over 64 KiB fails with the size message",
          "Expected Failed with the size limit message");
}

void test_symlink_is_rejected(const Sandbox& sandbox) {
    std::error_code error;
    fs::create_directory_symlink(sandbox.outside, sandbox.root / "link-dir", error);
    if (error) {
        std::cout << "SKIP: symlink rejection (cannot create directory symlink: " << error.message() << ")\n";
        return;
    }
    cipher::ToolRegistry registry;
    cipher::register_filesystem_tools(registry, sandbox.root.string());

    const cipher::ToolResult result = call(registry, "link-dir/secret.txt");
    check(result.outcome == cipher::ToolOutcome::Denied && !result.executed,
          "filesystem.read: symlinked directory is rejected",
          "Expected Denied for a path through a symlink");
}

} // namespace

int main() {
    std::cout << "=== Filesystem tool tests ===\n";

    const Sandbox sandbox;
    test_reads_file_under_root(sandbox);
    test_denies_traversal_without_running_handler(sandbox);
    test_denies_absolute_paths(sandbox);
    test_missing_file_and_empty_argument_are_denied(sandbox);
    test_missing_root_denies_instead_of_crashing(sandbox);
    test_size_bound_fails_after_permission(sandbox);
    test_symlink_is_rejected(sandbox);

    std::cout << "\nResults: " << g_passes << " passed, " << g_failures << " failed\n";
    return g_failures == 0 ? 0 : 1;
}
