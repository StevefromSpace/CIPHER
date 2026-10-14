#include "cipher/cipher_core.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

struct TestCase {
    std::string name;
    std::string input;
    std::string expected_substring;
    bool expected_exit = false;
};

} // namespace

int main() {
    const cipher::CipherCore core;
    const std::vector<TestCase> cases = {
        {"help command", "help", "Available commands:", false},
        {"about command", "about", "CIPHER v0.1.0", false},
        {"status command", "status", "External AI-provider dependency: none", false},
        {"echo command", "echo hello CIPHER", "hello CIPHER", false},
        {"unknown command", "not-a-command", "Unknown command:", false},
        {"exit command", "exit", "Session ended.", true},
        {"quit command", "quit", "Session ended.", true},
    };

    int failures = 0;
    for (const TestCase& test : cases) {
        const cipher::CommandResponse response = core.execute(test.input);
        const bool text_matches = response.text.find(test.expected_substring) != std::string::npos;
        const bool exit_matches = response.exit_requested == test.expected_exit;

        if (!text_matches || !exit_matches) {
            ++failures;
            std::cerr << "FAIL: " << test.name << '\n';
            std::cerr << "  Expected substring: " << test.expected_substring << '\n';
            std::cerr << "  Actual output:      " << response.text << '\n';
            std::cerr << "  Expected exit:      " << test.expected_exit << '\n';
            std::cerr << "  Actual exit:        " << response.exit_requested << '\n';
        } else {
            std::cout << "PASS: " << test.name << '\n';
        }
    }

    if (failures != 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }

    std::cout << "All " << cases.size() << " tests passed.\n";
    return 0;
}
