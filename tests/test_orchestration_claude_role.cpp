// Claude role executor tests with a fake ProcessRunner. No process is launched and no model is called.

#include "cipher/orchestration/claude_role_executor.hpp"
#include "cipher/orchestration/job_store_file.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
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
    int calls = 0;

    ProcessResult run(const std::vector<std::string>& /*argv*/, int /*timeout_ms*/) override {
        ++calls;
        return next;
    }
};

const std::string kSuccessJson =
    R"J({"type":"result","subtype":"success","is_error":false,"result":"OK","stop_reason":"end_turn",)J"
    R"J("total_cost_usd":0.0235484,"num_turns":1,"session_id":"fb7a41b1"})J";

ProcessResult success_result() {
    ProcessResult result;
    result.exit_code = 0;
    result.stdout_text = kSuccessJson;
    return result;
}

ClaudeRoleExecutor make_executor(ProcessRunner& runner) {
    return ClaudeRoleExecutor(runner, 42, R"J(C:\Users\me\.local\bin\claude.exe)J", "claude-sonnet-5-5", "0.05", 60000);
}

JobRecord empty_job() {
    return JobRecord{};
}

void test_security_escalates_with_no_process_call() {
    FakeProcessRunner process;
    ClaudeRoleExecutor executor = make_executor(process);
    const RoleResult result = executor.run(Role::Security, empty_job());
    check(process.calls == 0, "claude role: security makes zero process calls");
    check(result.event.kind == EventKind::Escalate && result.event.reason == EscalationReason::SecurityReviewUnavailable,
          "claude role: security escalates as SecurityReviewUnavailable, never a verdict");
}

void test_unsupported_roles_escalate_with_no_process_call() {
    const Role unsupported[] = {Role::Implementation, Role::Qa, Role::Reviewer};
    for (const Role role : unsupported) {
        FakeProcessRunner process;
        ClaudeRoleExecutor executor = make_executor(process);
        const RoleResult result = executor.run(role, empty_job());
        check(process.calls == 0, "claude role: unsupported role makes zero process calls");
        check(result.event.kind == EventKind::Escalate && result.event.reason == EscalationReason::RoleNotSupported,
              "claude role: unsupported role escalates as RoleNotSupported, not ImplementationFailed or Blocked");
    }
}

void test_research_success_completes_the_role() {
    FakeProcessRunner process;
    process.next = success_result();
    ClaudeRoleExecutor executor = make_executor(process);
    const RoleResult result = executor.run(Role::Research, empty_job());
    check(process.calls == 1, "claude role: research makes exactly one process call");
    check(result.event.kind == EventKind::ResearchComplete && result.model_family == "claude",
          "claude role: research success is ResearchComplete with model_family claude");
}

void test_architecture_success_completes_the_role() {
    FakeProcessRunner process;
    process.next = success_result();
    ClaudeRoleExecutor executor = make_executor(process);
    const RoleResult result = executor.run(Role::Architecture, empty_job());
    check(result.event.kind == EventKind::ArchitectureComplete, "claude role: architecture success is ArchitectureComplete");
}

void test_nonzero_exit_is_blocked_not_implementation() {
    FakeProcessRunner process;
    process.next.exit_code = 1;
    process.next.stdout_text = "";
    ClaudeRoleExecutor executor = make_executor(process);
    const RoleResult result = executor.run(Role::Research, empty_job());
    check(result.event.kind == EventKind::Blocked, "claude role: non-zero exit maps to Blocked (tooling failure)");
}

void test_timeout_is_blocked() {
    FakeProcessRunner process;
    process.next.timed_out = true;
    process.next.exit_code = -1;
    ClaudeRoleExecutor executor = make_executor(process);
    const RoleResult result = executor.run(Role::Architecture, empty_job());
    check(result.event.kind == EventKind::Blocked, "claude role: timeout maps to Blocked");
}

void test_model_error_is_blocked() {
    FakeProcessRunner process;
    process.next.exit_code = 1;
    process.next.stdout_text = R"J({"type":"result","subtype":"error_max_budget_usd","is_error":true})J";
    ClaudeRoleExecutor executor = make_executor(process);
    const RoleResult result = executor.run(Role::Research, empty_job());
    check(result.event.kind == EventKind::Blocked, "claude role: budget/model error maps to Blocked");
}

class ThrowingProcessRunner final : public ProcessRunner {
public:
    int calls = 0;
    ProcessResult run(const std::vector<std::string>&, int) override {
        ++calls;
        throw std::runtime_error("cannot launch");
    }
};

void test_runner_exception_is_blocked_after_one_call() {
    ThrowingProcessRunner process;
    ClaudeRoleExecutor executor = make_executor(process);
    const RoleResult result = executor.run(Role::Research, empty_job());
    check(result.event.kind == EventKind::Blocked, "claude role: runner exception maps to Blocked");
    check(process.calls == 1, "claude role: runner exception is not retried");
}

void test_invalid_budget_is_blocked_with_no_process_call() {
    FakeProcessRunner process;
    process.next = success_result();
    ClaudeRoleExecutor executor(process, 42, "claude.exe", "claude-sonnet-5-5", "-1", 60000);
    const RoleResult result = executor.run(Role::Research, empty_job());
    check(result.event.kind == EventKind::Blocked && process.calls == 0,
          "claude role: invalid budget is Blocked and nothing is launched");
}

void test_prompt_is_bounded_and_names_the_issue() {
    const std::string prompt = build_role_prompt(Role::Research, 42);
    check(prompt.find("#42") != std::string::npos, "claude role: prompt names the issue number");
    check(prompt.size() <= kMaxRolePromptChars, "claude role: prompt fits the bounded length");
}

void test_round_trip_through_file_store() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "cipher-claude-role-store-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    FileJobStore store(dir);

    JobRecord job;
    job.state = State::NeedsHuman;
    job.escalation = EscalationReason::RoleNotSupported;
    store.save(42, job);
    const std::optional<JobRecord> loaded = store.load(42);
    check(loaded.has_value() && loaded->escalation == EscalationReason::RoleNotSupported,
          "claude role: RoleNotSupported survives a FileJobStore round trip");

    std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
    test_security_escalates_with_no_process_call();
    test_unsupported_roles_escalate_with_no_process_call();
    test_research_success_completes_the_role();
    test_architecture_success_completes_the_role();
    test_nonzero_exit_is_blocked_not_implementation();
    test_timeout_is_blocked();
    test_model_error_is_blocked();
    test_runner_exception_is_blocked_after_one_call();
    test_invalid_budget_is_blocked_with_no_process_call();
    test_prompt_is_bounded_and_names_the_issue();
    test_round_trip_through_file_store();
    std::cout << (failures == 0 ? "ALL PASSED" : "FAILURES") << "\n";
    return failures == 0 ? 0 : 1;
}
