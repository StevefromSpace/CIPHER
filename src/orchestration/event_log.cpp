#include "cipher/orchestration/event_log.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <stdexcept>
#include <string>

namespace cipher::orchestration {
namespace fs = std::filesystem;

EventLog::EventLog(fs::path file) : file_(std::move(file)) {}

namespace {
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
    std::ofstream out(file_, std::ios::app | std::ios::binary);
    out << "issue=" << issue_number;
    if (!run_id.empty()) out << "\trun=" << run_id;
    out << "\tevent=" << event << "\tdetail=" << detail;
    if (!run_id.empty()) out << "\tts=" << utc_now();
    out << '\n';
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
