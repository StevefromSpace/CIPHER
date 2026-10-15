#include "cipher/cipher_core.hpp"

#include <iostream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Minimal test framework
// No external dependencies. Each test case specifies:
//   name             - label shown in output
//   input            - raw string sent to CipherCore::execute()
//   expected_substr  - substring that must appear in response.text;
//                      ignored when must_be_empty is true
//   must_be_empty    - when true, asserts response.text is exactly ""
//   expected_exit    - expected value of response.exit_requested
// ---------------------------------------------------------------------------

namespace {

struct TestCase {
    std::string name;
    std::string input;
    std::string expected_substr;
    bool        must_be_empty  = false;
    bool        expected_exit  = false;
};

} // namespace

int main() {
    const cipher::CipherCore core;

    // -----------------------------------------------------------------------
    // Test suite
    // Intentional behaviours documented inline:
    //   - Commands are CASE-SENSITIVE.  "Help", "HELP", "EXIT" are unknown.
    //   - Leading whitespace is stripped before parsing.
    //   - "echo" with no argument is valid and returns an empty string.
    //   - "echo" preserves its argument verbatim (no further trimming on the
    //     right, but leading whitespace after the command word IS trimmed).
    //   - "time" always returns a non-empty string prefixed with
    //     "Local system time: " followed by "YYYY-MM-DD HH:MM:SS".
    // -----------------------------------------------------------------------
    const std::vector<TestCase> cases = {
        // --- Original baseline tests (must remain passing) -----------------
        {"help command",
         "help",
         "Available commands:",
         false, false},

        {"about command",
         "about",
         "CIPHER v0.2.0",
         false, false},

        {"status command",
         "status",
         "External AI-provider dependency: none",
         false, false},

        {"echo with argument",
         "echo hello CIPHER",
         "hello CIPHER",
         false, false},

        {"unknown command",
         "not-a-command",
         "Unknown command:",
         false, false},

        {"exit command",
         "exit",
         "Session ended.",
         false, true},

        {"quit command",
         "quit",
         "Session ended.",
         false, true},

        // --- New: empty and whitespace-only input ---------------------------
        // Empty input must return exactly empty text and NOT request exit.
        // must_be_empty=true makes a non-empty response a test failure.
        {"empty input - text is empty",
         "",
         "", true, false},

        // Whitespace-only input trims to an empty command and behaves the
        // same as truly empty input.
        {"whitespace-only input - text is empty",
         "   \t  ",
         "", true, false},

        // --- New: echo edge cases -------------------------------------------
        // "echo" with no argument is a valid command; it must return exactly
        // empty text and must not request exit.
        {"echo with no argument - empty response",
         "echo",
         "", true, false},

        // echo preserves its argument including internal spaces.
        {"echo with multiple words",
         "echo one two three",
         "one two three",
         false, false},

        // --- New: time command ----------------------------------------------
        // The time command must return a non-empty string that begins with
        // the documented prefix.  We cannot predict the exact time value, so
        // we check the prefix and a plausible year.
        {"time command - has prefix",
         "time",
         "Local system time:",
         false, false},

        // The year in the output must start with "20" (covers 2000-2099,
        // which is the expected operating lifetime of this software).
        {"time command - year looks modern",
         "time",
         "20",
         false, false},

        // --- New: case sensitivity ------------------------------------------
        // Commands are matched literally.  Mixed-case variants must fall
        // through to the unknown-command handler.
        {"Help (capital H) is unknown",
         "Help",
         "Unknown command:",
         false, false},

        {"EXIT (all caps) is unknown",
         "EXIT",
         "Unknown command:",
         false, false},

        // --- New: unknown command details ------------------------------------
        // The error message must echo the unrecognised token so the user can
        // see what was rejected.
        {"unknown command echoes the token",
         "frobulate",
         "frobulate",
         false, false},

        // --- New: leading whitespace stripped before dispatch ---------------
        {"help with leading spaces",
         "   help",
         "Available commands:",
         false, false},

        {"exit with leading tab",
         "\texit",
         "Session ended.",
         false, true},

        // --- Version consistency tests --------------------------------------
        {"help command reports current version",
         "help",
         "v0.2.0",
         false, false},

        {"status command reports current version",
         "status",
         "Version: 0.2.0",
         false, false},
    };

    int failures = 0;

    // 1. Direct verification of generated version constants and version() API.
    const bool version_const_ok = (std::string(cipher::CIPHER_VERSION) == "0.2.0") &&
                                  (cipher::CIPHER_VERSION_MAJOR == 0) &&
                                  (cipher::CIPHER_VERSION_MINOR == 2) &&
                                  (cipher::CIPHER_VERSION_PATCH == 0);
    const bool version_fn_ok = (std::string(cipher::CipherCore::version()) == "0.2.0") &&
                               (std::string(cipher::CipherCore::version()) == cipher::CIPHER_VERSION);

    if (!version_const_ok || !version_fn_ok) {
        ++failures;
        std::cerr << "FAIL: version constants and CipherCore::version() consistency\n";
        if (!version_const_ok) {
            std::cerr << "  CIPHER_VERSION: \"" << cipher::CIPHER_VERSION << "\"\n";
            std::cerr << "  MAJOR: " << cipher::CIPHER_VERSION_MAJOR
                      << " MINOR: " << cipher::CIPHER_VERSION_MINOR
                      << " PATCH: " << cipher::CIPHER_VERSION_PATCH << "\n";
        }
        if (!version_fn_ok) {
            std::cerr << "  CipherCore::version(): \"" << cipher::CipherCore::version() << "\"\n";
        }
    } else {
        std::cout << "PASS: version constants and CipherCore::version() match 0.2.0\n";
    }

    // 2. Command test cases.
    for (const TestCase& test : cases) {
        const cipher::CommandResponse response = core.execute(test.input);

        // Text check: two modes.
        //   must_be_empty=true  -> response.text must be exactly "".
        //   must_be_empty=false -> response.text must contain expected_substr
        //                         (or expected_substr is "" meaning no check).
        bool text_ok = false;
        if (test.must_be_empty) {
            text_ok = response.text.empty();
        } else {
            text_ok = test.expected_substr.empty() ||
                      response.text.find(test.expected_substr) != std::string::npos;
        }

        const bool exit_ok = (response.exit_requested == test.expected_exit);

        if (!text_ok || !exit_ok) {
            ++failures;
            std::cerr << "FAIL: " << test.name << '\n';
            if (!text_ok) {
                if (test.must_be_empty) {
                    std::cerr << "  Expected text      : (exactly empty)\n";
                } else {
                    std::cerr << "  Expected substring : \"" << test.expected_substr << "\"\n";
                }
                std::cerr << "  Actual text        : \"" << response.text << "\"\n";
            }
            if (!exit_ok) {
                std::cerr << "  Expected exit      : " << test.expected_exit << '\n';
                std::cerr << "  Actual exit        : " << response.exit_requested << '\n';
            }
        } else {
            std::cout << "PASS: " << test.name << '\n';
        }
    }

    if (failures != 0) {
        std::cerr << '\n' << failures << " test(s) failed.\n";
        return 1;
    }

    std::cout << "\nAll " << cases.size() << " command tests and version consistency checks passed.\n";
    return 0;
}
