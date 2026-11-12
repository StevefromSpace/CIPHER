#include "cipher/orchestration/job_lock.hpp"

#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace cipher::orchestration {
namespace {
constexpr std::intptr_t kNoHandle = -1;
}

JobLock::JobLock(std::intptr_t handle) noexcept : handle_(handle) {}

JobLock::JobLock(JobLock&& other) noexcept : handle_(other.handle_) { other.handle_ = kNoHandle; }

JobLock& JobLock::operator=(JobLock&& other) noexcept {
    if (this != &other) {
        release();
        handle_ = other.handle_;
        other.handle_ = kNoHandle;
    }
    return *this;
}

JobLock::~JobLock() { release(); }

#ifdef _WIN32

std::optional<JobLock> JobLock::try_acquire(const std::filesystem::path& lock_file) {
    // Share mode 0: any second open of the same file fails while this handle is alive.
    HANDLE handle = CreateFileW(lock_file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_SHARING_VIOLATION) return std::nullopt;
        throw std::runtime_error("could not open job lock file " + lock_file.string());
    }
    return JobLock(reinterpret_cast<std::intptr_t>(handle));
}

void JobLock::release() noexcept {
    if (handle_ != kNoHandle) CloseHandle(reinterpret_cast<HANDLE>(handle_));
    handle_ = kNoHandle;
}

#else

std::optional<JobLock> JobLock::try_acquire(const std::filesystem::path& lock_file) {
    const int fd = ::open(lock_file.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0) throw std::runtime_error("could not open job lock file " + lock_file.string());
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const bool busy = errno == EWOULDBLOCK || errno == EAGAIN;
        ::close(fd);
        if (busy) return std::nullopt;
        throw std::runtime_error("could not lock job lock file " + lock_file.string());
    }
    return JobLock(static_cast<std::intptr_t>(fd));
}

void JobLock::release() noexcept {
    if (handle_ != kNoHandle) ::close(static_cast<int>(handle_));
    handle_ = kNoHandle;
}

#endif

}  // namespace cipher::orchestration
