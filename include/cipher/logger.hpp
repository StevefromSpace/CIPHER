#ifndef CIPHER_LOGGER_HPP
#define CIPHER_LOGGER_HPP

// ---------------------------------------------------------------------------
// cipher/logger.hpp  —  Diagnostic logging
//
// A small, self-contained logger.  No third-party libraries are used.
//
// Design decisions (explained for a C++ learner):
//
//   1. The Logger owns an optional file stream.  When a file path is
//      configured, the constructor opens that file once and keeps it open
//      throughout the program's lifetime.  Writing to an already-open file
//      is faster than reopening on every message.
//
//   2. Messages are filtered by level: only messages whose level >= the
//      configured minimum_level are emitted.  This lets you silence noisy
//      DEBUG messages in production while keeping them available for
//      development.
//
//   3. Stderr is always tried first (unless the Logger is constructed without
//      it).  If a file destination is configured but the file cannot be
//      opened, the Logger reports the problem to stderr and continues using
//      stderr only — it does not crash the application.
//
//   4. User input, passwords, and other sensitive data must never be passed
//      to the logger.  That rule is documented here, but the code cannot
//      enforce it automatically — it requires discipline from the caller.
//
//   5. The Logger is not thread-safe.  CIPHER is currently single-threaded
//      so this is acceptable.  A future version should add a mutex if
//      background threads are introduced.
//
// Typical usage:
//   cipher::Logger log(cipher::LogLevel::INFO, "cipher.log");
//   log.info("CIPHER started");
//   log.warn("Configuration file not found; using defaults");
//   log.error("Could not open log file: cipher.log");
// ---------------------------------------------------------------------------

#include "cipher/config.hpp"

#include <fstream>
#include <string>

namespace cipher {

class Logger {
public:
    // Constructs a Logger.
    //   minimum_level — messages below this level are silently discarded.
    //   log_file_path — optional file path; empty string = stderr only.
    //                   If the path is non-empty but cannot be opened, an
    //                   error is printed to stderr and file logging is
    //                   disabled for this session.
    explicit Logger(LogLevel minimum_level = LogLevel::INFO,
                    const std::string& log_file_path = "");

    // Non-copyable (the file stream cannot be shared).
    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

    // Movable.
    Logger(Logger&&)            = default;
    Logger& operator=(Logger&&) = default;

    ~Logger() = default;

    // -----------------------------------------------------------------------
    // Logging methods — one per level for readability.
    // Each appends a newline-terminated line to the active destination(s).
    // The format is:  YYYY-MM-DD HH:MM:SS [LEVEL] message
    // -----------------------------------------------------------------------
    void debug(const std::string& message);
    void info (const std::string& message);
    void warn (const std::string& message);
    void error(const std::string& message);

    // Generic emit — used internally and exposed for tests.
    void log(LogLevel level, const std::string& message);

    // -----------------------------------------------------------------------
    // Inspection (primarily for tests)
    // -----------------------------------------------------------------------
    LogLevel    minimum_level()    const noexcept { return minimum_level_; }
    bool        has_file_output()  const noexcept { return file_.is_open(); }
    std::string log_file_path()    const noexcept { return log_file_path_; }

private:
    // Writes a single formatted line to the appropriate destinations.
    void emit(LogLevel level, const std::string& message);

    LogLevel     minimum_level_;
    std::string  log_file_path_;
    std::ofstream file_;
};

} // namespace cipher

#endif // CIPHER_LOGGER_HPP
