#ifndef CIPHER_ORCHESTRATION_EVENT_LOG_HPP
#define CIPHER_ORCHESTRATION_EVENT_LOG_HPP

#include <filesystem>
#include <string>
#include <vector>

namespace cipher::orchestration {

// Stage 8 observability: append-only, one line per transition or decision. The log is for humans and recovery
// review; it is not read back to drive state (the job store is the authority).
//
// Line format: issue=<n>\tevent=<kind>\tdetail=<text>
// Detail must not contain tabs or newlines and must not contain tokens, credentials or private memory.
class EventLog {
public:
    explicit EventLog(std::filesystem::path file);

    void append(int issue_number, const std::string& event, const std::string& detail);
    std::vector<std::string> read_all() const;

private:
    std::filesystem::path file_;
};

}  // namespace cipher::orchestration

#endif  // CIPHER_ORCHESTRATION_EVENT_LOG_HPP
