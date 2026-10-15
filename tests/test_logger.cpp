#include "cipher/logger.hpp"
#include "cipher/config.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <sstream>

// ---------------------------------------------------------------------------
// test_logger.cpp  —  Tests for the Logger
//
// Challenges in testing a logger:
//   - The primary output goes to stderr, which tests cannot easily capture.
//   - We test file output because file contents can be read back and checked.
//   - Level filtering is tested by checking that a file-based logger does NOT
//     write a filtered message.
//   - The "bad file path" test verifies the Logger constructs successfully
//     even when the file cannot be opened (graceful degradation).
//
// Temporary files:
//   All test log files are created with names starting "test_logger_tmp_"
//   and are deleted after each test.
// ---------------------------------------------------------------------------

namespace {

// ---------------------------------------------------------------------------
// Simple test bookkeeping (same pattern as test_config.cpp)
// ---------------------------------------------------------------------------
int g_failures = 0;
int g_passes   = 0;

void pass(const std::string& name) {
    std::cout << "PASS: " << name << '\n';
    ++g_passes;
}

void fail(const std::string& name, const std::string& reason) {
    std::cerr << "FAIL: " << name << "\n  " << reason << '\n';
    ++g_failures;
}

void check(bool condition, const std::string& name, const std::string& reason) {
    if (condition) pass(name);
    else           fail(name, reason);
}

// ---------------------------------------------------------------------------
// Helper: read an entire file into a string.
// ---------------------------------------------------------------------------
std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------------------
// Helper: delete a file (ignores errors).
// ---------------------------------------------------------------------------
void delete_file(const char* path) {
    std::remove(path);
}

} // namespace

// ---------------------------------------------------------------------------
// Test sections
// ---------------------------------------------------------------------------

static void test_logger_construction_no_file() {
    // Must not throw; must have correct level; no file output.
    cipher::Logger log(cipher::LogLevel::INFO);
    check(log.minimum_level() == cipher::LogLevel::INFO,
          "construction no file: level is INFO",
          "Expected INFO");
    check(!log.has_file_output(),
          "construction no file: no file output",
          "Expected has_file_output() == false");
}

static void test_logger_construction_with_file() {
    const char* path = "test_logger_tmp_construct.log";
    delete_file(path); // ensure it doesn't already exist
    {
        cipher::Logger log(cipher::LogLevel::DEBUG, path);
        check(log.has_file_output(),
              "construction with file: file opened",
              "Expected has_file_output() == true");
    }
    delete_file(path);
}

static void test_logger_writes_to_file() {
    const char* path = "test_logger_tmp_write.log";
    delete_file(path);
    {
        cipher::Logger log(cipher::LogLevel::DEBUG, path);
        log.info("hello from test");
    } // Logger destructor flushes and closes file.

    const std::string contents = read_file(path);
    delete_file(path);

    check(!contents.empty(),
          "writes to file: file is not empty",
          "Expected non-empty log file");
    check(contents.find("hello from test") != std::string::npos,
          "writes to file: message appears in file",
          "Expected 'hello from test' in log contents");
    check(contents.find("[INFO ]") != std::string::npos,
          "writes to file: INFO label present",
          "Expected '[INFO ]' in log contents");
}

static void test_logger_level_labels() {
    const char* path = "test_logger_tmp_labels.log";
    delete_file(path);
    {
        cipher::Logger log(cipher::LogLevel::DEBUG, path);
        log.debug("debug msg");
        log.info("info msg");
        log.warn("warn msg");
        log.error("error msg");
    }

    const std::string contents = read_file(path);
    delete_file(path);

    check(contents.find("[DEBUG]") != std::string::npos,
          "level labels: DEBUG present", "Expected '[DEBUG]'");
    check(contents.find("[INFO ]") != std::string::npos,
          "level labels: INFO present", "Expected '[INFO ]'");
    check(contents.find("[WARN ]") != std::string::npos,
          "level labels: WARN present", "Expected '[WARN ]'");
    check(contents.find("[ERROR]") != std::string::npos,
          "level labels: ERROR present", "Expected '[ERROR]'");
}

static void test_logger_timestamp_in_output() {
    const char* path = "test_logger_tmp_ts.log";
    delete_file(path);
    {
        cipher::Logger log(cipher::LogLevel::INFO, path);
        log.info("timestamped message");
    }
    const std::string contents = read_file(path);
    delete_file(path);

    // Timestamp starts with "20" (covers years 2000–2099).
    check(contents.find("20") != std::string::npos,
          "timestamp: year looks modern",
          "Expected year starting with '20'");
    // Format contains hyphens in date and colons in time.
    check(contents.find('-') != std::string::npos,
          "timestamp: date separator present",
          "Expected '-' in timestamp");
    check(contents.find(':') != std::string::npos,
          "timestamp: time separator present",
          "Expected ':' in timestamp");
}

static void test_logger_level_filtering_below_threshold() {
    const char* path = "test_logger_tmp_filter.log";
    delete_file(path);
    {
        // Logger set to WARN — DEBUG and INFO must not appear in file.
        cipher::Logger log(cipher::LogLevel::WARN, path);
        log.debug("this should not appear");
        log.info("this should not appear either");
        log.warn("this should appear");
        log.error("this should also appear");
    }
    const std::string contents = read_file(path);
    delete_file(path);

    check(contents.find("this should not appear") == std::string::npos,
          "filtering: DEBUG/INFO suppressed at WARN level",
          "Found suppressed message in log");
    check(contents.find("this should appear") != std::string::npos,
          "filtering: WARN message present",
          "Expected WARN message in log");
    check(contents.find("this should also appear") != std::string::npos,
          "filtering: ERROR message present",
          "Expected ERROR message in log");
}

static void test_logger_level_filtering_error_only() {
    const char* path = "test_logger_tmp_filter_err.log";
    delete_file(path);
    {
        cipher::Logger log(cipher::LogLevel::ERROR, path);
        log.debug("no");
        log.info("no");
        log.warn("no");
        log.error("yes");
    }
    const std::string contents = read_file(path);
    delete_file(path);

    check(contents.find("yes") != std::string::npos,
          "filtering ERROR only: ERROR appears",
          "Expected 'yes' in log");
    // The file should contain exactly one line (the error).
    // Count newlines to verify only one message was written.
    int newlines = 0;
    for (char c : contents) {
        if (c == '\n') ++newlines;
    }
    check(newlines == 1,
          "filtering ERROR only: exactly one line written",
          "Expected 1 line, got " + std::to_string(newlines));
}

static void test_logger_debug_level_sees_all() {
    const char* path = "test_logger_tmp_debug_all.log";
    delete_file(path);
    {
        cipher::Logger log(cipher::LogLevel::DEBUG, path);
        log.debug("d");
        log.info("i");
        log.warn("w");
        log.error("e");
    }
    const std::string contents = read_file(path);
    delete_file(path);

    check(contents.find("d") != std::string::npos, "debug level: sees DEBUG", "");
    check(contents.find("i") != std::string::npos, "debug level: sees INFO",  "");
    check(contents.find("w") != std::string::npos, "debug level: sees WARN",  "");
    check(contents.find("e") != std::string::npos, "debug level: sees ERROR", "");
}

static void test_logger_bad_file_path_degrades_gracefully() {
    // An invalid/inaccessible path should not crash the Logger.
    // The constructor prints a warning to stderr and continues.
    // We verify by checking that the Logger constructs without throwing
    // and that has_file_output() returns false.
    const std::string bad_path =
        "Z:\\this\\path\\does\\not\\exist\\cipher_test.log";
    cipher::Logger log(cipher::LogLevel::INFO, bad_path);
    check(!log.has_file_output(),
          "bad file path: file output disabled",
          "Expected has_file_output() == false after bad path");
    // Must still be usable.
    log.info("this goes to stderr only");
    pass("bad file path: Logger still usable after failure");
}

static void test_logger_append_mode() {
    // Two Logger instances writing to the same file should both appear.
    const char* path = "test_logger_tmp_append.log";
    delete_file(path);
    {
        cipher::Logger log(cipher::LogLevel::INFO, path);
        log.info("first session");
    }
    {
        cipher::Logger log(cipher::LogLevel::INFO, path);
        log.info("second session");
    }
    const std::string contents = read_file(path);
    delete_file(path);

    check(contents.find("first session") != std::string::npos,
          "append mode: first session present",
          "Expected 'first session'");
    check(contents.find("second session") != std::string::npos,
          "append mode: second session present",
          "Expected 'second session'");
}

static void test_logger_generic_log_method() {
    const char* path = "test_logger_tmp_generic.log";
    delete_file(path);
    {
        cipher::Logger log(cipher::LogLevel::DEBUG, path);
        log.log(cipher::LogLevel::WARN, "via generic log()");
    }
    const std::string contents = read_file(path);
    delete_file(path);

    check(contents.find("via generic log()") != std::string::npos,
          "generic log(): message present",
          "Expected message via log()");
    check(contents.find("[WARN ]") != std::string::npos,
          "generic log(): WARN label",
          "Expected '[WARN ]'");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "=== Logger tests ===\n";

    test_logger_construction_no_file();
    test_logger_construction_with_file();
    test_logger_writes_to_file();
    test_logger_level_labels();
    test_logger_timestamp_in_output();
    test_logger_level_filtering_below_threshold();
    test_logger_level_filtering_error_only();
    test_logger_debug_level_sees_all();
    test_logger_bad_file_path_degrades_gracefully();
    test_logger_append_mode();
    test_logger_generic_log_method();

    std::cout << '\n';
    if (g_failures != 0) {
        std::cerr << g_failures << " logger test(s) failed.\n";
        return 1;
    }
    std::cout << "All " << g_passes << " logger tests passed.\n";
    return 0;
}
