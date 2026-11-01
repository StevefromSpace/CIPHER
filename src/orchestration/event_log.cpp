#include "cipher/orchestration/event_log.hpp"

#include <fstream>
#include <stdexcept>
#include <string>

namespace cipher::orchestration {
namespace fs = std::filesystem;

EventLog::EventLog(fs::path file) : file_(std::move(file)) {}

void EventLog::append(int issue_number, const std::string& event, const std::string& detail) {
    if (event.find_first_of("\t\r\n") != std::string::npos || detail.find_first_of("\t\r\n") != std::string::npos) {
        throw std::invalid_argument("event log fields must not contain tabs or newlines");
    }
    fs::create_directories(file_.parent_path());
    std::ofstream out(file_, std::ios::app | std::ios::binary);
    out << "issue=" << issue_number << "\tevent=" << event << "\tdetail=" << detail << '\n';
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
