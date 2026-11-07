#ifndef CIPHER_ORCHESTRATION_JOB_COMMANDS_HPP
#define CIPHER_ORCHESTRATION_JOB_COMMANDS_HPP

#include "cipher/orchestration/event_log.hpp"
#include "cipher/orchestration/job_store_file.hpp"
#include "cipher/orchestration/orchestrator.hpp"

#include <filesystem>
#include <string>

namespace cipher {

/// Same contract as ToolCommandResult: when handled is false the line belongs to the next handler.
struct JobCommandResult {
    bool handled{false};
    std::string text;
};

/**
 * @brief Owner-typed REPL commands that drive the orchestration state machine:
 *        `job new <issue> [research] [design] [security]`, `job status <issue>`, `job list`,
 *        `job step <issue>`, `job recover <issue>`, `job log`.
 *
 * State lives in the orchestration directory (FileJobStore + EventLog), so jobs survive a restart.
 * The state machine stays the only source of truth; this class only forwards to Orchestrator.
 * Until a real tool-enabled executor is supplied, the default executor escalates every role to the
 * owner (RoleNotSupported). It never fabricates a result.
 */
class JobCommands {
public:
    /// @p executor must outlive this object; null selects the escalating default.
    explicit JobCommands(std::filesystem::path directory, orchestration::RoleExecutor* executor = nullptr);

    JobCommands(const JobCommands&) = delete;
    JobCommands& operator=(const JobCommands&) = delete;

    JobCommandResult execute(const std::string& input);

private:
    std::string create(const std::string& arguments);
    std::string status(const std::string& arguments) const;
    std::string list() const;
    std::string step(const std::string& arguments);
    std::string recover(const std::string& arguments);
    std::string log() const;

    std::filesystem::path directory_;
    orchestration::FileJobStore store_;
    orchestration::EventLog log_;
    orchestration::RoleExecutor* executor_;
};

}  // namespace cipher

#endif  // CIPHER_ORCHESTRATION_JOB_COMMANDS_HPP
