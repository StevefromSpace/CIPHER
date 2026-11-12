// Cross-handle job lock: a held lock refuses writers before any load, and releases on destruction.

#include "cipher/orchestration/job_commands.hpp"
#include "cipher/orchestration/job_lock.hpp"

#include "cipher/orchestration/event_log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using cipher::JobCommands;
using cipher::orchestration::JobLock;

int failures = 0;
std::string self_exe;

void check(bool condition, const std::string& name) {
    if (condition) {
        std::cerr << "PASS: " << name << '\n';
    } else {
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }
}

fs::path fresh_dir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void test_second_handle_is_refused_while_first_is_held() {
    const fs::path dir = fresh_dir("cipher-job-lock-basic");
    const fs::path lock_file = dir / "job-7.lock";
    {
        auto first = JobLock::try_acquire(lock_file);
        check(first.has_value(), "lock: first handle acquires a fresh lock file");
        auto second = JobLock::try_acquire(lock_file);
        check(!second.has_value(), "lock: a second handle in the same process is refused while the first is held");
    }
    {
        auto third = JobLock::try_acquire(lock_file);
        check(third.has_value(), "lock: the lock is free again after the holder is destroyed");
    }
    fs::remove_all(dir);  // the lock handle is closed first; an open handle would block deletion on Windows
}

void test_writer_is_refused_before_any_state_change() {
    const fs::path dir = fresh_dir("cipher-job-lock-writer");
    const fs::path lock_file = dir / "job-7.lock";
    {
        auto held = JobLock::try_acquire(lock_file);
        check(held.has_value(), "writer: test takes the lock first");
        JobCommands commands(dir);
        const std::string refused = commands.execute("job new 7").text;
        check(refused.find("in use by another process") != std::string::npos,
              "writer: job new is refused while another handle holds the lock");
        check(!fs::exists(dir / "job-7.state"), "writer: no job record is written while the lock is held");
    }
    JobCommands commands(dir);
    const std::string created = commands.execute("job new 7").text;
    check(created.find("in use") == std::string::npos, "writer: job new succeeds after the lock is released");
    check(fs::exists(dir / "job-7.state"), "writer: the job record exists after the retry");
    check(commands.execute("job list").text.find("job 7") != std::string::npos,
          "writer: job list shows the job and ignores the .lock file");
    fs::remove_all(dir);
}

// ---- Two-process tests: this executable re-runs itself in a helper mode, so the other process is real. ----

void publish(const fs::path& target, const std::string& text) {
    fs::path temp = target;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
    }
    fs::rename(temp, target);  // the reader only ever sees a complete file
}

bool wait_for(const fs::path& file, std::chrono::seconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!fs::exists(file)) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

long long own_pid() {
#ifdef _WIN32
    return static_cast<long long>(GetCurrentProcessId());
#else
    return static_cast<long long>(getpid());
#endif
}

void spawn_background(const std::string& exe, const std::string& args) {
#ifdef _WIN32
    std::system(("start \"\" /B \"" + exe + "\" " + args + " >nul 2>&1").c_str());
#else
    std::system(("\"" + exe + "\" " + args + " >/dev/null 2>&1 &").c_str());
#endif
}

void kill_process(long long pid) {
#ifdef _WIN32
    std::system(("taskkill /F /PID " + std::to_string(pid) + " >nul 2>&1").c_str());
#else
    kill(static_cast<pid_t>(pid), SIGKILL);
#endif
}

// Helper mode: take the job lock, announce it, and hold it until the parent writes the stop file.
int run_holder(const fs::path& lock_file, const fs::path& ready) {
    auto lock = JobLock::try_acquire(lock_file);
    if (!lock) return 2;
    publish(ready, std::to_string(own_pid()));
    while (!fs::exists(ready.string() + ".stop")) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    lock.reset();
    publish(ready.string() + ".done", "done");
    return 0;
}

// Helper mode: append `count` lines to the shared events.log through EventLog, the real write path.
int run_appender(const fs::path& log_file, const std::string& tag, int count) {
    cipher::orchestration::EventLog log(log_file);
    for (int i = 0; i < count; ++i) log.append(1, "tick", tag + "-" + std::to_string(i));
    publish(log_file.string() + "." + tag + ".done", "done");
    return 0;
}

std::string quoted(const fs::path& path) { return "\"" + path.string() + "\""; }

void test_lock_held_by_another_process() {
    const fs::path dir = fresh_dir("cipher-job-lock-xproc");
    const fs::path lock_file = dir / "job-7.lock";
    const fs::path ready = dir / "holder";
    spawn_background(self_exe, "--hold " + quoted(lock_file) + " " + quoted(ready));
    check(wait_for(ready, std::chrono::seconds(15)), "two-process: the other process acquires the lock");
    check(!JobLock::try_acquire(lock_file).has_value(),
          "two-process: this process is refused while the other process holds the lock");
    JobCommands commands(dir);
    check(commands.execute("job new 7").text.find("in use by another process") != std::string::npos,
          "two-process: job new is refused while the other process holds the lock");
    check(!fs::exists(dir / "job-7.state"), "two-process: a refused job new writes no record");
    publish(ready.string() + ".stop", "");
    check(wait_for(fs::path(ready.string() + ".done"), std::chrono::seconds(15)),
          "two-process: the other process releases and exits");
    check(JobLock::try_acquire(lock_file).has_value(), "two-process: the lock is free after the other process exits");
    fs::remove_all(dir);
}

void test_killed_holder_releases_lock() {
    const fs::path dir = fresh_dir("cipher-job-lock-kill");
    const fs::path lock_file = dir / "job-7.lock";
    const fs::path ready = dir / "holder";
    spawn_background(self_exe, "--hold " + quoted(lock_file) + " " + quoted(ready));
    check(wait_for(ready, std::chrono::seconds(15)), "crash: the holder process acquires the lock before it is killed");
    long long pid = 0;
    {
        std::ifstream in(ready);
        in >> pid;
    }
    kill_process(pid);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    bool acquired = false;
    while (!acquired && std::chrono::steady_clock::now() < deadline) {
        acquired = JobLock::try_acquire(lock_file).has_value();
        if (!acquired) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check(acquired, "crash: a killed holder releases the lock so job recover can proceed");
    fs::remove_all(dir);
}

void test_concurrent_event_appends_are_not_interleaved() {
    const fs::path dir = fresh_dir("cipher-events-xproc");
    const fs::path log_file = dir / "events.log";
    constexpr int kPerProcess = 200;
    spawn_background(self_exe, "--append " + quoted(log_file) + " A " + std::to_string(kPerProcess));
    spawn_background(self_exe, "--append " + quoted(log_file) + " B " + std::to_string(kPerProcess));
    const bool a_done = wait_for(fs::path(log_file.string() + ".A.done"), std::chrono::seconds(60));
    const bool b_done = wait_for(fs::path(log_file.string() + ".B.done"), std::chrono::seconds(60));
    check(a_done && b_done, "events: both appender processes finish");

    std::vector<std::string> expected;
    for (const std::string tag : {"A", "B"}) {
        for (int i = 0; i < kPerProcess; ++i) {
            expected.push_back("issue=1\tevent=tick\tdetail=" + tag + "-" + std::to_string(i));
        }
    }
    std::vector<std::string> actual = cipher::orchestration::EventLog(log_file).read_all();
    std::sort(expected.begin(), expected.end());
    std::sort(actual.begin(), actual.end());
    check(actual == expected, "events: 400 lines from two processes, each whole and none lost or interleaved");
    fs::remove_all(dir);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 0) self_exe = fs::absolute(argv[0]).string();
    if (argc >= 4 && std::string(argv[1]) == "--hold") return run_holder(argv[2], argv[3]);
    if (argc >= 5 && std::string(argv[1]) == "--append") return run_appender(argv[2], argv[3], std::stoi(argv[4]));

    test_second_handle_is_refused_while_first_is_held();
    test_writer_is_refused_before_any_state_change();
    test_lock_held_by_another_process();
    test_killed_holder_releases_lock();
    test_concurrent_event_appends_are_not_interleaved();
    std::cout << (failures == 0 ? "ALL PASSED" : "SOME FAILED") << '\n';
    return failures == 0 ? 0 : 1;
}
