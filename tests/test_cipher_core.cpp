#include "cipher/cipher_core.hpp"

#include <iostream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Minimal test framework
// No external dependencies. Each test case specifies:
//   name             - label shown in output
//   input            - raw string sent to CipherCore::execute()
//   expected_substr  - substring that must appear in response.text
//                      (empty string means we only check exit_requested)
//   expected_exit    - expected value of response.exit_requested
// ---------------------------------------------------------------------------

namespace {

struct TestCase {
    std::string name;
    std::string input;
    std::string expected_substr;
    bool        expected_exit = false;
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
         false},

        {"about command",
         "about",
         "CIPHER v0.1.0",
         false},

        {"status command",
         "status",
         "External AI-provider dependency: none",
         false},

        {"echo with argument",
         "echo hello CIPHER",
         "hello CIPHER",
         false},

        {"unknown command",
         "not-a-command",
         "Unknown command:",
         false},

        {"exit command",
         "exit",
         "Session ended.",
         true},

        {"quit command",
         "quit",
         "Session ended.",
         true},

        // --- New: empty and whitespace-only input ---------------------------
        // Empty input must return an empty text and NOT request exit.
        {"empty input - text is empty",
         "",
         "",         // expected_substr="" means any text (including empty) is OK
         false},

        // Whitespace-only input trims to an empty command and behaves the
        // same as truly empty input.
        {"whitespace-only input - text is empty",
         "   \t  ",
         "",
         false},

        // --- New: echo edge cases -------------------------------------------
        // "echo" with no argument is a valid command; it returns empty text
        // and does not request exit.
        {"echo with no argument - empty response",
         "echo",
         "",
         false},

        // echo preserves its argument including internal spaces.
        {"echo with multiple words",
         "echo one two three",
         "one two three",
         false},

        // --- New: time command ----------------------------------------------
        // The time command must return a non-empty string that begins with
        // the documented prefix.  We cannot predict the exact time value, so
        // we check the prefix and a plausible year.
        {"time command - has prefix",
         "time",
         "Local system time:",
         false},

        // The year in the output must start with "20" (covers 2000-2099,
        // which is the expected operating lifetime of this software).
        {"time command - year looks modern",
         "time",
         "20",
         false},

        // --- New: case sensitivity ------------------------------------------
        // Commands are matched literally.  Mixed-case variants must fall
        // through to the unknown-command handler.
        {"Help (capital H) is unknown",
         "Help",
         "Unknown command:",
         false},

        {"EXIT (all caps) is unknown",
         "EXIT",
         "Unknown command:",
         false},

        // --- New: unknown command details ------------------------------------
        // The error message must echo the unrecognised token so the user can
        // see what was rejected.
        {"unknown command echoes the token",
         "frobulate",
         "frobulate",
         false},

        // --- New: leading whitespace stripped before dispatch ---------------
        {"help with leading spaces",
         "   help",
         "Available commands:",
         false},

        {"exit with leading tab",
         "\texit",
         "Session ended.",
         true},
    };

    int failures = 0;
    for (const TestCase& test : cases) {
        const cipher::CommandResponse response = core.execute(test.input);

        // If expected_substr is empty, we only verify the exit flag.
        const bool text_ok =
            test.expected_substr.empty() ||
            response.text.find(test.expected_substr) != std::string::npos;

        const bool exit_ok = (response.exit_requested == test.expected_exit);

        if (!text_ok || !exit_ok) {
            ++failures;
            std::cerr << "FAIL: " << test.name << '\n';
            if (!text_ok) {
                std::cerr << "  Expected substring : \"" << test.expected_substr << "\"\n";
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

    std::cout << "\nAll " << cases.size() << " tests passed.\n";
    return 0;
}
