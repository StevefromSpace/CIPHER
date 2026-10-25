#include "cipher/config.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
// NOGDI keeps wingdi.h's ERROR macro from clashing with LogLevel::ERROR.
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// test_config.cpp  —  Tests for the configuration subsystem
//
// No external testing framework is used.  The same pattern as
// test_cipher_core.cpp: each case is a struct; we iterate, run, and count
// failures.
//
// Tests are organised into named sections.  Each section tests one aspect
// of the parser.  Sections are run sequentially; a failure in one section
// does not stop the others.
//
// Temporary files:
//   Some tests need to write real files.  We use a local helper that writes
//   a file, runs a test, and deletes the file.  The names are chosen to be
//   unlikely to clash with any real CIPHER file.
// ---------------------------------------------------------------------------

namespace {

// ---------------------------------------------------------------------------
// Simple test bookkeeping
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
// write_temp_file / delete_temp_file
// Creates and removes a temporary config file for tests that need real I/O.
// ---------------------------------------------------------------------------
const char* TEMP_CONFIG = "test_cipher_config_tmp.conf";

bool write_temp_file(const std::string& content) {
    std::ofstream f(TEMP_CONFIG, std::ios::trunc);
    if (!f.is_open()) return false;
    f << content;
    return true;
}

void delete_temp_file() {
    std::remove(TEMP_CONFIG);
}

} // namespace

// ---------------------------------------------------------------------------
// Test sections
// ---------------------------------------------------------------------------

static void test_defaults() {
    const cipher::Config cfg = cipher::ConfigParser::defaults();
    check(cfg.log_level == cipher::LogLevel::INFO,
          "defaults: log_level is INFO",
          "Expected LogLevel::INFO");
    check(cfg.log_file.empty(),
          "defaults: log_file is empty",
          "Expected empty log_file");
}

static void test_load_from_string_empty() {
    const cipher::ConfigResult r = cipher::ConfigParser::load_from_string("");
    check(r.ok(), "empty string: no errors", "Got unexpected errors");
    check(r.config.log_level == cipher::LogLevel::INFO,
          "empty string: default log_level",
          "Expected INFO");
}

static void test_load_from_string_comments_and_blanks() {
    const std::string text =
        "# This is a comment\n"
        "\n"
        "   # Indented comment\n"
        "   \n";   // whitespace-only line
    const cipher::ConfigResult r = cipher::ConfigParser::load_from_string(text);
    check(r.ok(), "comments and blanks: no errors", "Got unexpected errors");
    check(r.config.log_level == cipher::LogLevel::INFO,
          "comments and blanks: default preserved",
          "Expected INFO");
}

static void test_load_from_string_valid_log_level() {
    const struct { std::string text; cipher::LogLevel expected; } cases[] = {
        {"log_level = debug\n", cipher::LogLevel::DEBUG},
        {"log_level = info\n",  cipher::LogLevel::INFO},
        {"log_level = warn\n",  cipher::LogLevel::WARN},
        {"log_level = error\n", cipher::LogLevel::ERROR},
        // Whitespace around '=' should be stripped.
        {"log_level=debug\n",       cipher::LogLevel::DEBUG},
        {"  log_level  =  warn  \n", cipher::LogLevel::WARN},
    };
    for (const auto& c : cases) {
        const cipher::ConfigResult r = cipher::ConfigParser::load_from_string(c.text);
        const std::string name = "valid log_level '" + c.text.substr(0, c.text.size()-1) + "'";
        check(r.ok(), name + ": no errors", "Got unexpected errors");
        check(r.config.log_level == c.expected,
              name + ": level matches",
              "Level did not match expected");
    }
}

static void test_load_from_string_log_file() {
    const cipher::ConfigResult r =
        cipher::ConfigParser::load_from_string("log_file = /tmp/cipher.log\n");
    check(r.ok(), "log_file: no errors", "Got unexpected errors");
    check(r.config.log_file == "/tmp/cipher.log",
          "log_file: value stored",
          "Expected '/tmp/cipher.log'");
}

static void test_load_from_string_log_file_empty_value() {
    // An empty value for log_file means "no file" (same as the default).
    const cipher::ConfigResult r =
        cipher::ConfigParser::load_from_string("log_file =\n");
    check(r.ok(), "log_file empty value: no errors", "Got unexpected errors");
    check(r.config.log_file.empty(),
          "log_file empty value: log_file is empty",
          "Expected empty string");
}

static void test_load_from_string_unknown_key() {
    const cipher::ConfigResult r =
        cipher::ConfigParser::load_from_string("unknown_key = something\n");
    check(!r.ok(), "unknown key: reports error", "Expected an error");
    check(!r.errors.empty() && r.errors[0].line == 1,
          "unknown key: error on line 1",
          "Expected line 1 error");
}

static void test_load_from_string_invalid_log_level() {
    const cipher::ConfigResult r =
        cipher::ConfigParser::load_from_string("log_level = verbose\n");
    check(!r.ok(), "invalid log_level: reports error", "Expected an error");
    // On failure, config must be reset to safe defaults.
    check(r.config.log_level == cipher::LogLevel::INFO,
          "invalid log_level: config reset to defaults",
          "Expected default INFO after error");
}

static void test_load_from_string_no_equals() {
    const cipher::ConfigResult r =
        cipher::ConfigParser::load_from_string("thisisnotvalid\n");
    check(!r.ok(), "missing '=': reports error", "Expected an error");
    check(r.errors[0].line == 1,
          "missing '=': error on line 1",
          "Expected line 1 error");
}

static void test_load_from_string_line_number_tracking() {
    // Two lines: first is a comment, second is invalid.
    const cipher::ConfigResult r = cipher::ConfigParser::load_from_string(
        "# comment\n"
        "bad_line_no_equals\n");
    check(!r.ok(), "line tracking: reports error", "Expected an error");
    check(!r.errors.empty() && r.errors[0].line == 2,
          "line tracking: error on line 2",
          "Expected error on line 2");
}

static void test_load_from_string_multiple_errors() {
    const cipher::ConfigResult r = cipher::ConfigParser::load_from_string(
        "log_level = debug\n"
        "bad_key = x\n"          // error on line 2
        "another_bad = y\n");    // error on line 3
    check(!r.ok(), "multiple errors: not ok", "Expected errors");
    check(r.errors.size() == 2,
          "multiple errors: two errors reported",
          "Expected exactly 2 errors, got " + std::to_string(r.errors.size()));
}

static void test_load_missing_file() {
    // A path that should not exist on any reasonable system.
    const cipher::ConfigResult r = cipher::ConfigParser::load(
        "__cipher_does_not_exist_xyz__.conf");
    check(r.ok(), "missing file: no errors", "Expected no errors for absent file");
    check(r.config.log_level == cipher::LogLevel::INFO,
          "missing file: defaults returned",
          "Expected default INFO");
}

// An existing file that cannot be opened must be a configuration error, not defaults.
// Platform hooks make the file unreadable with real OS mechanisms:
//   POSIX:   chmod 000 (skipped when running as root, which can read anything).
//   Windows: hold the file open with no sharing, so the parser's open fails.
static void test_load_unreadable_file_is_error() {
    if (!write_temp_file("log_level = error\n")) {
        fail("unreadable file: setup", "Could not write temp file");
        return;
    }
#if defined(_WIN32)
    HANDLE locked = CreateFileA(TEMP_CONFIG, GENERIC_READ, 0, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (locked == INVALID_HANDLE_VALUE) {
        fail("unreadable file: lock", "Could not lock temp file");
        delete_temp_file();
        return;
    }
    const cipher::ConfigResult r = cipher::ConfigParser::load(TEMP_CONFIG);
    CloseHandle(locked);
#else
    if (geteuid() == 0) {
        std::cout << "SKIP: unreadable file (running as root; permissions not enforced)\n";
        delete_temp_file();
        return;
    }
    chmod(TEMP_CONFIG, 0);
    const cipher::ConfigResult r = cipher::ConfigParser::load(TEMP_CONFIG);
    chmod(TEMP_CONFIG, S_IRUSR | S_IWUSR);
#endif
    delete_temp_file();
    check(!r.ok(), "unreadable file: reported as error, not missing",
          "Unreadable existing file was treated as missing");
    check(r.config.log_level == cipher::LogLevel::INFO,
          "unreadable file: defaults kept alongside the error",
          "Expected default INFO");
}

static void test_load_real_file_valid() {
    if (!write_temp_file("log_level = warn\n")) {
        fail("real file valid: setup", "Could not write temp file");
        return;
    }
    const cipher::ConfigResult r = cipher::ConfigParser::load(TEMP_CONFIG);
    delete_temp_file();
    check(r.ok(), "real file valid: no errors", "Got unexpected errors");
    check(r.config.log_level == cipher::LogLevel::WARN,
          "real file valid: level is WARN",
          "Expected WARN");
}

static void test_load_real_file_with_comments() {
    if (!write_temp_file(
        "# comment\n"
        "\n"
        "log_level = error\n"
        "# another comment\n"))
    {
        fail("real file comments: setup", "Could not write temp file");
        return;
    }
    const cipher::ConfigResult r = cipher::ConfigParser::load(TEMP_CONFIG);
    delete_temp_file();
    check(r.ok(), "real file comments: no errors", "Got unexpected errors");
    check(r.config.log_level == cipher::LogLevel::ERROR,
          "real file comments: level is ERROR",
          "Expected ERROR");
}

static void test_load_real_file_invalid() {
    if (!write_temp_file("typo_key = oops\n")) {
        fail("real file invalid: setup", "Could not write temp file");
        return;
    }
    const cipher::ConfigResult r = cipher::ConfigParser::load(TEMP_CONFIG);
    delete_temp_file();
    check(!r.ok(), "real file invalid: reports error", "Expected an error");
}

static void test_log_level_name() {
    check(std::string(cipher::log_level_name(cipher::LogLevel::DEBUG)) == "debug",
          "log_level_name DEBUG", "Expected 'debug'");
    check(std::string(cipher::log_level_name(cipher::LogLevel::INFO))  == "info",
          "log_level_name INFO",  "Expected 'info'");
    check(std::string(cipher::log_level_name(cipher::LogLevel::WARN))  == "warn",
          "log_level_name WARN",  "Expected 'warn'");
    check(std::string(cipher::log_level_name(cipher::LogLevel::ERROR)) == "error",
          "log_level_name ERROR", "Expected 'error'");
}

static void test_parse_log_level_roundtrip() {
    const cipher::LogLevel levels[] = {
        cipher::LogLevel::DEBUG,
        cipher::LogLevel::INFO,
        cipher::LogLevel::WARN,
        cipher::LogLevel::ERROR,
    };
    for (const auto& level : levels) {
        cipher::LogLevel parsed{};
        const bool ok = cipher::parse_log_level(cipher::log_level_name(level), parsed);
        check(ok, std::string("parse_log_level roundtrip ") + cipher::log_level_name(level),
              "parse_log_level returned false");
        check(parsed == level,
              std::string("parse_log_level value matches ") + cipher::log_level_name(level),
              "Parsed level did not match original");
    }
}

static void test_parse_log_level_unknown() {
    cipher::LogLevel out{};
    check(!cipher::parse_log_level("verbose", out),
          "parse_log_level unknown 'verbose'",
          "Expected false");
    check(!cipher::parse_log_level("", out),
          "parse_log_level empty string",
          "Expected false");
    check(!cipher::parse_log_level("INFO", out),   // uppercase must fail
          "parse_log_level 'INFO' uppercase",
          "Expected false (names are lower-case only)");
}

static void test_crlf_line_endings() {
    // Windows-style CRLF should parse correctly.
    const cipher::ConfigResult r = cipher::ConfigParser::load_from_string(
        "log_level = warn\r\n"
        "# comment\r\n");
    check(r.ok(), "CRLF endings: no errors", "Got unexpected errors");
    check(r.config.log_level == cipher::LogLevel::WARN,
          "CRLF endings: level is WARN",
          "Expected WARN");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "=== Config tests ===\n";

    test_defaults();
    test_load_from_string_empty();
    test_load_from_string_comments_and_blanks();
    test_load_from_string_valid_log_level();
    test_load_from_string_log_file();
    test_load_from_string_log_file_empty_value();
    test_load_from_string_unknown_key();
    test_load_from_string_invalid_log_level();
    test_load_from_string_no_equals();
    test_load_from_string_line_number_tracking();
    test_load_from_string_multiple_errors();
    test_load_missing_file();
    test_load_unreadable_file_is_error();
    test_load_real_file_valid();
    test_load_real_file_with_comments();
    test_load_real_file_invalid();
    test_log_level_name();
    test_parse_log_level_roundtrip();
    test_parse_log_level_unknown();
    test_crlf_line_endings();

    std::cout << '\n';
    if (g_failures != 0) {
        std::cerr << g_failures << " config test(s) failed.\n";
        return 1;
    }
    std::cout << "All " << g_passes << " config tests passed.\n";
    return 0;
}
