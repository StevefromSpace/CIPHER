// ---------------------------------------------------------------------------
// tests/fixtures/fake_cli.cpp  —  deterministic stand-in for the Claude Code CLI
//
// Built only for tests. It never contacts a service and never needs credentials.
//
//   fake_cli auth status --json   Prints auth JSON chosen by CIPHER_FAKE_AUTH:
//                                 "loggedin" (default), "loggedout", or anything
//                                 else for malformed output.
//   fake_cli --argv-check ...     Prints its arguments joined by '|'.
//   fake_cli --print ...          Reads the whole prompt from stdin. A prompt that
//                                 starts with "MODE:" selects a behaviour; any
//                                 other prompt is echoed back as "stdin:<prompt>".
// ---------------------------------------------------------------------------

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

namespace {

constexpr std::size_t BIG_OUTPUT_BYTES = 1024 * 1024;

void write_repeated(std::FILE* stream, std::size_t count) {
    const std::string chunk(4096, 'x');
    for (std::size_t written = 0; written < count; written += chunk.size()) {
        std::fwrite(chunk.data(), 1, chunk.size(), stream);
    }
}

int run_auth_status() {
    const char* mode = std::getenv("CIPHER_FAKE_AUTH");
    const std::string state = mode ? mode : "loggedin";
    if (state == "loggedout") {
        std::cout << "{\n  \"loggedIn\": false\n}\n";
    } else if (state == "loggedin") {
        std::cout << "{\n  \"loggedIn\": true,\n  \"authMethod\": \"claude.ai\",\n"
                     "  \"email\": \"redacted@example.invalid\"\n}\n";
    } else {
        std::cout << "not json\n";
    }
    return 0;
}

int run_argv_check(int argc, char** argv) {
    for (int index = 2; index < argc; ++index) {
        if (index > 2) std::cout << '|';
        std::cout << argv[index];
    }
    std::cout << '\n';
    return 0;
}

int run_print_mode() {
    const std::string prompt((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    const std::string prefix = "MODE:";
    if (prompt.compare(0, prefix.size(), prefix) != 0) {
        std::cout << "stdin:" << prompt << '\n';
        return 0;
    }
    const std::string mode = prompt.substr(prefix.size(), prompt.find('\n') - prefix.size());
    if (mode == "ok") {
        std::cout << "  answer text  \n";
    } else if (mode == "empty") {
        return 0;
    } else if (mode == "fail") {
        std::cerr << "error: rejected token sk-ant-api03-SECRETVALUE\n";
        return 2;
    } else if (mode == "big-stdout") {
        write_repeated(stdout, BIG_OUTPUT_BYTES);
    } else if (mode == "big-stderr") {
        write_repeated(stderr, BIG_OUTPUT_BYTES);
    } else if (mode == "sleep") {
        std::this_thread::sleep_for(std::chrono::seconds(60));
    } else if (mode == "cwd") {
        std::cout << std::filesystem::current_path().string() << '\n';
    } else {
        std::cerr << "unknown mode\n";
        return 3;
    }
    return 0;
}

} // anonymous namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "auth" && std::string(argv[2]) == "status") {
        return run_auth_status();
    }
    if (argc >= 2 && std::string(argv[1]) == "--argv-check") {
        return run_argv_check(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--print") {
        return run_print_mode();
    }
    std::cerr << "fake_cli: unsupported invocation\n";
    return 64;
}
