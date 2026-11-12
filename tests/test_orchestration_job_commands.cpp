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

        const std::string shown = jobs.execute("job show 7").text;
        check(contains(shown, "job 7: NEEDS_HUMAN") && contains(shown, "events:") && contains(shown, "issue=7"),
              "show prints state and that job's events");
        const std::string json = jobs.execute("job show 7 --json").text;
        check(json.front() == '{' && json.back() == '}' && contains(json, "\"issue\":7") &&
                  contains(json, "\"state\":\"NEEDS_HUMAN\""), "show --json is a flat object");
        check(contains(shown, "run_id=run-7-") && contains(json, "\"run_id\":\"run-7-"), "job carries a run id");
        check(contains(jobs.execute("job log 7").text, "run=run-7-") && contains(jobs.execute("job log 7").text, "ts="),
              "events carry run id and timestamp");
        check(contains(jobs.execute("job show 99").text, "No job"), "show missing job");
        check(contains(jobs.execute("job show x").text, "Usage"), "show rejects bad issue");
        check(contains(jobs.execute("job log abc").text, "positive issue"), "log rejects bad issue");
        check(contains(jobs.execute("job log 99").text, "No job events"), "log filters by issue");
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

    {
        // `job ci`: result is tied to the job head; pending changes nothing; a stale head is rejected.
        using namespace cipher::orchestration;
        const std::string head(40, 'a');
        const std::string other(40, 'b');
        const fs::path cdir = fresh_dir();
        FileJobStore store(cdir);
        JobRecord record;
        record.state = State::Ci;
        record.head_sha = head;
        record.qa_approved = true;
        store.save(21, record);
        JobRecord idle;
        store.save(22, idle);

        std::optional<Event> next;
        int polled = 0;
        JobCommands jobs(cdir, nullptr, nullptr, [&](const JobRecord&, int) {
            ++polled;
            return next;
        });
        check(contains(JobCommands(cdir).execute("job ci 21 5").text, "unavailable"), "ci unavailable without observer");
        check(contains(jobs.execute("job ci 21").text, "Usage"), "ci needs a pr number");
        check(contains(jobs.execute("job ci 99 5").text, "No job"), "ci for missing job rejected");
        check(contains(jobs.execute("job ci 22 5").text, "not waiting for CI") && polled == 0, "ci outside CI state");
        check(contains(jobs.execute("job ci 21 5").text, "still pending"), "pending reported");
        check(contains(jobs.execute("job status 21").text, "CI"), "pending leaves state");

        next = Event{EventKind::CiResult, other, Verdict::None, Verdict::None, true};
        check(contains(jobs.execute("job ci 21 5").text, "rejected"), "stale head rejected");
        check(!store.load(21)->ci_passed, "stale CI not recorded");

        next = Event{EventKind::CiResult, head, Verdict::None, Verdict::None, true};
        check(contains(jobs.execute("job ci 21 5").text, "CI recorded"), "matching head accepted");
        check(store.load(21)->ci_passed && store.load(21)->state != State::Ci, "CI passed moves on");
        check(contains(jobs.execute("job log").text, "21"), "ci evidence in event log");
        std::error_code cec;
        fs::remove_all(cdir, cec);
    }

    {
        // A failed log write after a successful save is a warning, not a job error: the record is authoritative.
        using namespace cipher::orchestration;
        const fs::path ldir = fresh_dir();
        fs::create_directories(ldir / "events.log");  // a directory where the log file should be: every append fails
        JobCommands jobs(ldir);
        const std::string created = jobs.execute("job new 31 research").text;
        check(contains(created, "job 31:") && contains(created, "WARNING") && !contains(created, "job error"),
              "log failure on new: reported as a warning, not an error");
        check(FileJobStore(ldir).load(31).has_value() && !FileJobStore(ldir).load(31)->run_id.empty(),
              "log failure on new: the job and its run id were still saved");
        const std::string stepped = jobs.execute("job step 31").text;
        check(contains(stepped, "NEEDS_HUMAN") && contains(stepped, "WARNING") && !contains(stepped, "job error"),
              "log failure on step: the step is kept and the failure is warned about");
        check(!contains(jobs.execute("job recover 31").text, "WARNING"), "nothing in flight: no log write, no warning");

        // A flagged record outside Implementation cannot come from a real crash: refuse and leave it for the owner.
        JobRecord odd;
        odd.state = State::Qa;
        odd.execution_in_flight = true;
        FileJobStore(ldir).save(32, odd);
        const std::string refused = jobs.execute("job recover 32").text;
        check(contains(refused, "inconsistent record") && FileJobStore(ldir).load(32)->execution_in_flight,
              "recover of an inconsistent record is refused and the record stays flagged");
        std::error_code lec;
        fs::remove_all(ldir, lec);
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    std::cout << (failures == 0 ? "ALL PASSED\n" : "FAILURES\n");
    return failures == 0 ? 0 : 1;
}
