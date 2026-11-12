// Cross-handle job lock: a held lock refuses writers before any load, and releases on destruction.

#include "cipher/orchestration/job_commands.hpp"
#include "cipher/orchestration/job_lock.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

namespace fs = std::filesystem;
using cipher::JobCommands;
using cipher::orchestration::JobLock;

int failures = 0;

void check(bool condition, const std::string& name) {
    if (condition) {
        std::cerr << "PASS: " << name << '\n';
    } else {
        std::cout << "FAIL: " << name << '\n';
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

}  // namespace

int main() {
    test_second_handle_is_refused_while_first_is_held();
    test_writer_is_refused_before_any_state_change();
    std::cout << (failures == 0 ? "ALL PASSED" : "SOME FAILED") << '\n';
    return failures == 0 ? 0 : 1;
}
