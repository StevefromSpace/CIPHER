// JobCommands: REPL-facing orchestration commands over the real FileJobStore in a temp directory.

#include "cipher/orchestration/job_commands.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

namespace fs = std::filesystem;
using cipher::JobCommands;

int failures = 0;

void check(bool ok, const char* what) {
    std::cout << (ok ? "PASS: " : "FAIL: ") << what << "\n";
    if (!ok) ++failures;
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

fs::path fresh_dir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("cipher-jobcmd-" + std::to_string(stamp));
    fs::remove_all(dir);
    return dir;
}

// Counts how many times a role was run, and reports a fixed event.
class CountingExecutor final : public cipher::orchestration::RoleExecutor {
public:
    int calls = 0;
    cipher::orchestration::RoleResult run(cipher::orchestration::Role, const cipher::orchestration::JobRecord&) override {
        ++calls;
        using namespace cipher::orchestration;
        return {Event{EventKind::ResearchComplete}, "fake"};
    }
};

}  // namespace

int main() {
    const fs::path dir = fresh_dir();
    {
        JobCommands jobs(dir);
        check(!jobs.execute("help").handled, "non-job line is not handled");
        check(jobs.execute("job").handled && contains(jobs.execute("job").text, "Usage"), "bare job prints usage");
        check(contains(jobs.execute("job list").text, "No jobs"), "empty list");

        const std::string created = jobs.execute("job new 7 research").text;
        check(contains(created, "job 7: RESEARCH") || contains(created, "job 7:"), "new creates job 7");
        check(contains(jobs.execute("job new 7").text, "already exists"), "duplicate rejected");
        check(contains(jobs.execute("job new abc").text, "positive issue number"), "non-numeric issue rejected");
        check(contains(jobs.execute("job new 0").text, "positive issue number"), "zero issue rejected");
        check(contains(jobs.execute("job new -3").text, "positive issue number"), "negative issue rejected");
        check(contains(jobs.execute("job new 8 bogus").text, "Unknown flag"), "unknown flag rejected");
        check(contains(jobs.execute("job status 9").text, "No job"), "missing job reported");

        // Default executor never fabricates results: stepping escalates to the owner.
        const std::string stepped = jobs.execute("job step 7").text;
        check(contains(stepped, "NEEDS_HUMAN") && contains(stepped, "role not supported"), "default executor escalates");
        check(contains(jobs.execute("job log").text, "7"), "events recorded in log");
    }
    {
        // A new instance on the same directory sees the persisted job (restart).
        JobCommands again(dir);
        check(contains(again.execute("job status 7").text, "NEEDS_HUMAN"), "state survives restart");
        check(contains(again.execute("job list").text, "job 7"), "list finds persisted job");
        check(contains(again.execute("job recover 7").text, "nothing in flight"), "recover with nothing in flight");
    }
    {
        // A supplied executor is the one that runs.
        CountingExecutor executor;
        JobCommands jobs(dir, &executor);
        jobs.execute("job new 11 research");
        jobs.execute("job step 11");
        check(executor.calls == 1, "supplied executor runs exactly once per step");
    }

    {
        // `job task` is refused without a factory, and with one the task-specific executor is used.
        check(contains(JobCommands(dir).execute("job task 11 do it").text, "unavailable"), "task refused without factory");
        int built = 0;
        std::string seen;
        CountingExecutor task_executor;
        JobCommands jobs(dir, nullptr, [&](int, const std::string& task) {
            ++built;
            seen = task;
            return std::unique_ptr<cipher::orchestration::RoleExecutor>(new CountingExecutor());
        });
        jobs.execute("job new 12 research");
        check(contains(jobs.execute("job step 12").text, "NEEDS_HUMAN"), "no task file: default executor");
        check(built == 0, "factory unused before a task exists");
        jobs.execute("job new 13 research");
        check(contains(jobs.execute("job task 13 write docs").text, "task saved"), "task saved");
        check(contains(jobs.execute("job task 99 x").text, "No job"), "task for missing job rejected");
        check(contains(jobs.execute("job task 13").text, "Usage"), "task without text rejected");
        jobs.execute("job step 13");
        check(built == 1 && seen == "write docs", "factory built once with the saved task");
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    std::cout << (failures == 0 ? "ALL PASSED\n" : "FAILURES\n");
    return failures == 0 ? 0 : 1;
}
