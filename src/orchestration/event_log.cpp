#include "cipher/orchestration/event_log.hpp"

#include "cipher/orchestration/job_lock.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace cipher::orchestration {
namespace fs = std::filesystem;

EventLog::EventLog(fs::path file) : file_(std::move(file)) {}

namespace {
// Long enough to ride out another append (a few microseconds); a stuck holder fails the append instead of hanging.
constexpr std::chrono::seconds kAppendLockWait{5};

std::string utc_now() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &parts);
    return buffer;
}
}  // namespace

void EventLog::append(int issue_number, const std::string& event, const std::string& detail,
                      const std::string& run_id) {
    const char* bad = "\t\r\n";
    if (event.find_first_of(bad) != std::string::npos || detail.find_first_of(bad) != std::string::npos ||
        run_id.find_first_of(bad) != std::string::npos) {
        throw std::invalid_argument("event log fields must not contain tabs or newlines");
    }
    fs::create_directories(file_.parent_path());
    // events.log is shared by every job, so appends from different processes are serialized by one OS lock.
    // The lock is held only around the write and is released by the OS if the process dies.
    fs::path lock_file = file_;
    lock_file += ".lock";
    const auto deadline = std::chrono::steady_clock::now() + kAppendLockWait;
    std::optional<JobLock> lock = JobLock::try_acquire(lock_file);
    while (!lock && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        lock = JobLock::try_acquire(lock_file);
    }
    if (!lock) throw std::runtime_error("event log: another process holds " + lock_file.string());

    // One write call per line, so the line cannot be split by a buffer boundary.
    std::string line = "issue=" + std::to_string(issue_number);
    if (!run_id.empty()) line += "\trun=" + run_id;
    line += "\tevent=" + event + "\tdetail=" + detail;
    if (!run_id.empty()) line += "\tts=" + utc_now();
    line += '\n';
    std::ofstream out(file_, std::ios::app | std::ios::binary);
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
    out.close();
    if (!out) throw std::runtime_error("event log: cannot append to " + file_.string());
}

std::vector<std::string> EventLog::read_all() const {
    std::vector<std::string> lines;
    std::ifstream in(file_);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

}  // namespace cipher::orchestration
