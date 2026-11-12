#ifndef CIPHER_ORCHESTRATION_JOB_LOCK_HPP
#define CIPHER_ORCHESTRATION_JOB_LOCK_HPP

#include <cstdint>
#include <filesystem>
#include <optional>

namespace cipher::orchestration {

// Exclusive per-job lock across processes. The OS holds it for as long as the handle is open and releases it
// when the process exits, even after a crash, so a stale lock file never blocks `job recover`.
// Windows: CreateFileW with share mode 0. POSIX: flock(LOCK_EX | LOCK_NB).
// shortcut: locks one job file only; events.log is shared across jobs and its appends are not serialized,
// upgrade to a directory-wide lock when a second job-writing process is expected.
class JobLock final {
public:
    // Returns nullopt when another handle holds the lock. Throws std::runtime_error on other I/O failures.
    static std::optional<JobLock> try_acquire(const std::filesystem::path& lock_file);

    JobLock(JobLock&& other) noexcept;
    JobLock& operator=(JobLock&& other) noexcept;
    JobLock(const JobLock&) = delete;
    JobLock& operator=(const JobLock&) = delete;
    ~JobLock();

private:
    explicit JobLock(std::intptr_t handle) noexcept;
    void release() noexcept;

    std::intptr_t handle_;  // HANDLE on Windows, file descriptor on POSIX; -1 when empty
};

}  // namespace cipher::orchestration

#endif  // CIPHER_ORCHESTRATION_JOB_LOCK_HPP
