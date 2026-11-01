// GhProcessRunner tests with a fake ProcessRunner. No process is launched and no GitHub call is made.

#include "cipher/orchestration/gh_process_runner.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace cipher::orchestration;

int failures = 0;

void check(bool condition, const char* name) {
    if (condition) {
        std::cout << "PASS: " << name << "\n";
    } else {
        std::cout << "FAIL: " << name << "\n";
        ++failures;
    }
}

class FakeProcessRunner final : public ProcessRunner {
public:
    ProcessResult next;
    std::vector<std::string> last_argv;
    int last_timeout_ms = 0;
    int calls = 0;

    ProcessResult run(const std::vector<std::string>& argv, int timeout_ms) override {
        last_argv = argv;
        last_timeout_ms = timeout_ms;
        ++calls;
        return next;
    }
};

// The runner requires an absolute path, which means a drive path on Windows and a root path elsewhere.
#ifdef _WIN32
const std::string kGhPath = R"J(C:\Program Files\GitHub CLI\gh.exe)J";
#else
const std::string kGhPath = "/usr/bin/gh";
#endif

void test_argv_zero_is_replaced_with_resolved_path() {
    FakeProcessRunner process;
    process.next.stdout_text = "1\tOPEN\tabc\tdef\tMERGEABLE\n";
    GhProcessRunner runner(process, kGhPath, 15000);
    GhAdapter adapter(runner);
    adapter.read_pull_request(1);
    check(!process.last_argv.empty() && process.last_argv.front() == kGhPath,
          "gh runner: argv[0] is replaced by the resolved absolute gh path");
    check(process.last_timeout_ms == 15000, "gh runner: configured timeout is passed to the process runner");
}

void test_relative_gh_path_rejected() {
    FakeProcessRunner process;
    bool rejected = false;
    try {
        GhProcessRunner runner(process, "gh.exe", 1000);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "gh runner: relative gh path rejected at construction");
}

void test_argv_not_starting_with_gh_rejected() {
    FakeProcessRunner process;
    GhProcessRunner runner(process, kGhPath, 1000);
    bool rejected = false;
    try {
        runner.run({"cmd.exe", "/c", "echo"});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected && process.calls == 0, "gh runner: non-gh argv rejected with zero process launches");
}

void test_timeout_is_a_failure() {
    FakeProcessRunner process;
    process.next.timed_out = true;
    process.next.exit_code = -1;
    process.next.stdout_text = "1\tOPEN\tabc\tdef\tMERGEABLE\n";
    GhProcessRunner runner(process, kGhPath, 1000);
    const GhResult result = runner.run({"gh", "pr", "view", "1"});
    check(result.exit_code != 0 && result.output.empty(), "gh runner: timeout is non-zero and drops stdout");
}

void test_output_overflow_is_a_failure() {
    FakeProcessRunner process;
    process.next.output_overflow = true;
    process.next.exit_code = 0;
    process.next.stdout_text = "partial";
    GhProcessRunner runner(process, kGhPath, 1000);
    const GhResult result = runner.run({"gh", "pr", "view", "1"});
    check(result.exit_code != 0 && result.output.empty(), "gh runner: output overflow is non-zero and drops stdout");
}

void test_stderr_is_never_returned() {
    FakeProcessRunner process;
    process.next.exit_code = 1;
    process.next.stderr_text = "token=secret-value";
    GhProcessRunner runner(process, kGhPath, 1000);
    const GhResult result = runner.run({"gh", "pr", "view", "1"});
    check(result.exit_code == 1 && result.output.find("secret") == std::string::npos,
          "gh runner: stderr is discarded and never reaches the adapter output");
}

void test_merge_pins_head_through_runner() {
    FakeProcessRunner process;
    process.next.exit_code = 0;
    GhProcessRunner runner(process, kGhPath, 1000);
    GhAdapter adapter(runner);
    const std::string sha(40, 'a');
    const bool merged = adapter.merge_pull_request(7, sha);
    const std::vector<std::string> expected = {kGhPath, "pr", "merge", "7", "--squash", "--match-head-commit", sha};
    check(merged && process.last_argv == expected, "gh runner: merge argv is pinned to the gated head SHA");
}

}  // namespace

int main() {
    test_argv_zero_is_replaced_with_resolved_path();
    test_relative_gh_path_rejected();
    test_argv_not_starting_with_gh_rejected();
    test_timeout_is_a_failure();
    test_output_overflow_is_a_failure();
    test_stderr_is_never_returned();
    test_merge_pins_head_through_runner();
    std::cout << (failures == 0 ? "ALL PASSED" : "FAILURES") << "\n";
    return failures == 0 ? 0 : 1;
}
