// Process runner tests for Windows and POSIX. The test binary re-launches itself in child modes, so no shell is involved.

#include "cipher/orchestration/process_runner.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fstream>
#include <unistd.h>
#endif

namespace {

using namespace cipher::orchestration;

int failures = 0;

void check(bool condition, const char* name) {
    if (condition) {
        std::cout << "PASS: " << name << "\n";
    } else {
        std::cout << "FAIL: " << name << "\n";
        ++failures;
    }
}

// Child modes. Each returns the exit code the parent test expects.
int run_child(int argc, char** argv) {
    const std::string mode = argv[1];
    if (mode == "--child-exit") return argc > 2 ? std::stoi(argv[2]) : 0;
    if (mode == "--child-echo") {
        // Prints each argument from index 2 as [arg], so the parent can check the exact argv round-trip.
        for (int i = 2; i < argc; ++i) std::printf("[%s]\n", argv[i]);
        return 0;
    }
    if (mode == "--child-flood") {
        // Writes the same number of bytes to stdout and stderr, interleaved, to exercise both reader threads.
        const std::size_t bytes = argc > 2 ? static_cast<std::size_t>(std::stoul(argv[2])) : 0;
        const std::string chunk(4096, 'x');
        for (std::size_t written = 0; written < bytes; written += chunk.size()) {
            std::fwrite(chunk.data(), 1, chunk.size(), stdout);
            std::fwrite(chunk.data(), 1, chunk.size(), stderr);
        }
        return 0;
    }
    if (mode == "--child-sleep") {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
#ifndef _WIN32
    if (mode == "--child-setsid") {
        // Leaves a grandchild in its own session (outside the process group) that records its pid, then both sleep.
        if (fork() == 0) {
            setsid();
            std::ofstream(argv[2]) << getpid() << " ";
            std::this_thread::sleep_for(std::chrono::seconds(30));
            _exit(0);
        }
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
#endif
    return 2;
}

std::string self_path(const char* argv0) {
    return std::filesystem::absolute(argv0).string();
}

void test_exit_code_round_trip(const std::string& self) {
    SystemProcessRunner runner;
    const ProcessResult r = runner.run({self, "--child-exit", "7"}, 20000);
    check(r.exit_code == 7 && !r.timed_out, "runner: exit code 7 is returned, not timed out");
}

void test_argv_round_trip(const std::string& self) {
    SystemProcessRunner runner;
    // Empty element, spaces, embedded quotes, and a trailing backslash before the closing quote.
    const ProcessResult r = runner.run({self, "--child-echo", "", "a b", "say \"hi\"", "C:\\dir\\"}, 20000);
    // Text-mode stdout writes CRLF on Windows; drop the carriage returns before comparing the argv text.
    std::string normalized;
    for (const char c : r.stdout_text) {
        if (c != '\r') normalized.push_back(c);
    }
    const std::string expected = "[]\n[a b]\n[say \"hi\"]\n[C:\\dir\\]\n";
    check(r.exit_code == 0 && normalized == expected, "runner: empty, spaced, quoted and backslash argv round-trip");
}

void test_relative_argv0_is_rejected() {
    SystemProcessRunner runner;
    bool rejected = false;
    try {
        runner.run({"gh.exe", "pr", "view"}, 1000);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "runner: relative argv[0] rejected before any launch");
}

// An absolute path that does not exist must surface as an error, not a silent non-zero exit.
void test_missing_executable_throws() {
    SystemProcessRunner runner;
    bool threw = false;
    try {
        runner.run({std::filesystem::absolute("no-such-cipher-binary-xyz").string()}, 5000);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "runner: a missing absolute executable throws runtime_error");
}

void test_timeout_kills_child(const std::string& self) {
    SystemProcessRunner runner;
    const auto started = std::chrono::steady_clock::now();
    const ProcessResult r = runner.run({self, "--child-sleep"}, 500);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    check(r.timed_out && r.exit_code != 0, "runner: timeout reports timed_out and a non-zero exit");
    check(elapsed < std::chrono::seconds(15), "runner: timeout returns long before the child's 30 s sleep");
}

void test_large_output_on_both_streams(const std::string& self) {
    SystemProcessRunner runner;
    const std::size_t bytes = 100000;   // > 64 KiB on each stream
    const ProcessResult r = runner.run({self, "--child-flood", std::to_string(bytes)}, 20000);
    check(r.exit_code == 0 && !r.output_overflow, "runner: 100 KB on both streams is not an overflow");
    check(r.stdout_text.size() >= bytes && r.stderr_text.size() >= bytes,
          "runner: both streams are fully drained past 64 KiB");
}

void test_output_cap_is_a_failure(const std::string& self) {
    SystemProcessRunner runner(1000);
    const ProcessResult r = runner.run({self, "--child-flood", "100000"}, 20000);
    check(r.output_overflow, "runner: output past the cap sets output_overflow");
}

#ifndef _WIN32
// Alive means signalable and not a zombie (an unreaped orphan under a non-reaping init is already dead).
bool process_alive(pid_t pid) {
    if (kill(pid, 0) != 0) return false;
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(stat, line);
    const std::size_t close = line.rfind(')');
    return close == std::string::npos || close + 2 >= line.size() || line[close + 2] != 'Z';
}

void test_timeout_kills_setsid_grandchild(const std::string& self) {
    const std::string pid_file = (std::filesystem::temp_directory_path() / ("cipher_setsid_" + std::to_string(getpid()))).string();
    std::filesystem::remove(pid_file);
    SystemProcessRunner runner;
    const ProcessResult r = runner.run({self, "--child-setsid", pid_file}, 1500);
    std::ifstream in(pid_file);
    long pid = 0;
    in >> pid;
    std::filesystem::remove(pid_file);
    check(r.timed_out && pid > 0, "runner: setsid grandchild started and the run timed out");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    check(pid > 0 && !process_alive(static_cast<pid_t>(pid)), "runner: timeout kills a grandchild that called setsid");
    if (pid > 0) kill(static_cast<pid_t>(pid), SIGKILL);   // do not leave it behind if the check failed
}
#endif

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]).rfind("--child-", 0) == 0) return run_child(argc, argv);

    const std::string self = self_path(argv[0]);
    test_exit_code_round_trip(self);
    test_argv_round_trip(self);
    test_relative_argv0_is_rejected();
    test_missing_executable_throws();
    test_timeout_kills_child(self);
#ifndef _WIN32
    test_timeout_kills_setsid_grandchild(self);
#endif
    test_large_output_on_both_streams(self);
    test_output_cap_is_a_failure(self);
    std::cout << (failures == 0 ? "ALL PASSED" : "FAILURES") << "\n";
    return failures == 0 ? 0 : 1;
}
