// Fake-process tests for the Claude executor. No real claude.exe is launched here.
// Fixtures mirror the one observed live response (claude 2.1.316, 2026-10-30) and the budget-overrun error shape.

#include "cipher/orchestration/claude_executor.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

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

class FakeRunner final : public ProcessRunner {
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

const char* kSuccessJson =
    R"J({"type":"result","subtype":"success","is_error":false,"result":"OK","stop_reason":"end_turn",)J"
    R"J("total_cost_usd":0.0235484,"num_turns":1,"session_id":"fb7a41b1"})J";

const char* kBudgetJson =
    R"J({"type":"result","subtype":"error_max_budget_usd","is_error":true,)J"
    R"J("errors":["Reached maximum budget ($0.05)"],"total_cost_usd":0.138068})J";

ProcessResult make_process(int exit_code, const std::string& out, bool timed_out = false) {
    ProcessResult r;
    r.exit_code = exit_code;
    r.stdout_text = out;
    r.timed_out = timed_out;
    return r;
}

void test_argv_is_fixed_and_bounded() {
    const auto argv = build_claude_argv("claude.exe", "claude-sonnet-5-5", "0.50", "Reply with OK.");
    check(argv.size() == 12, "argv: fixed length, prompt is one element");
    check(argv[1] == "-p" && argv[2] == "--tools" && argv[3].empty(), "argv: tools disabled with empty value");
    check(argv.back() == "Reply with OK.", "argv: prompt is last, passed as a single element");
    bool budget_ok = false;
    for (std::size_t i = 0; i + 1 < argv.size(); ++i) {
        if (argv[i] == "--max-budget-usd" && argv[i + 1] == "0.50") budget_ok = true;
    }
    check(budget_ok, "argv: budget flag carries the given amount");
}

void test_argv_rejects_unsafe_input() {
    bool empty_model = false;
    bool flag_prompt = false;
    bool empty_prompt = false;
    try { build_claude_argv("c", "", "0.5", "hi"); } catch (const std::invalid_argument&) { empty_model = true; }
    try { build_claude_argv("c", "m", "0.5", "--dangerously-skip"); } catch (const std::invalid_argument&) { flag_prompt = true; }
    try { build_claude_argv("c", "m", "0.5", ""); } catch (const std::invalid_argument&) { empty_prompt = true; }
    check(empty_model, "argv: empty model rejected");
    check(flag_prompt, "argv: prompt starting with '-' rejected");
    check(empty_prompt, "argv: empty prompt rejected");
}

void test_success_returns_text_and_cost() {
    FakeRunner runner;
    runner.next = make_process(0, kSuccessJson);
    const ClaudeResult r = run_claude(runner, "claude.exe", "claude-sonnet-5-5", "0.50", "Reply with OK.", 60000);
    check(r.status == ClaudeStatus::Ok, "success: status OK");
    check(r.text == "OK", "success: assistant text read from result field");
    check(r.total_cost_usd > 0.023 && r.total_cost_usd < 0.024, "success: cost read from total_cost_usd");
    check(runner.calls == 1 && runner.last_timeout_ms == 60000, "success: one bounded call with timeout");
}

void test_budget_overrun_is_model_error_with_exit_one() {
    FakeRunner runner;
    runner.next = make_process(1, kBudgetJson);
    const ClaudeResult r = run_claude(runner, "c", "m", "0.05", "hi", 60000);
    check(r.status == ClaudeStatus::NonZeroExit, "budget overrun: exit 1 maps to NON_ZERO_EXIT");
    check(r.subtype == "error_max_budget_usd", "budget overrun: subtype kept for diagnostics");
    check(r.text.empty(), "budget overrun: no text returned");
}

void test_timeout_is_reported() {
    FakeRunner runner;
    runner.next = make_process(-1, "", true);
    const ClaudeResult r = run_claude(runner, "c", "m", "0.5", "hi", 1000);
    check(r.status == ClaudeStatus::Timeout, "timeout: status TIMEOUT");
    check(r.text.empty(), "timeout: no text");
}

void test_malformed_json_is_reported() {
    check(parse_claude_output(make_process(0, "not json")).status == ClaudeStatus::MalformedOutput,
          "malformed: non-JSON stdout on exit 0 is MALFORMED_OUTPUT");
    check(parse_claude_output(make_process(0, "[1,2]")).status == ClaudeStatus::MalformedOutput,
          "malformed: JSON array is MALFORMED_OUTPUT");
    check(parse_claude_output(make_process(0, R"J({"subtype":"success","is_error":false})J")).status ==
              ClaudeStatus::MalformedOutput,
          "malformed: success without result text is MALFORMED_OUTPUT");
    check(parse_claude_output(make_process(1, "garbage")).status == ClaudeStatus::NonZeroExit,
          "malformed: non-JSON on non-zero exit is NON_ZERO_EXIT");
}

void test_non_zero_exit_with_success_json_is_not_ok() {
    check(parse_claude_output(make_process(2, kSuccessJson)).status == ClaudeStatus::NonZeroExit,
          "non-zero exit: success JSON on exit 2 is NON_ZERO_EXIT, not OK");
}

void test_is_error_true_is_model_error() {
    const ClaudeResult r = parse_claude_output(make_process(0, R"J({"subtype":"success","is_error":true,"result":"x"})J"));
    check(r.status == ClaudeStatus::ModelError, "model error: is_error true is MODEL_ERROR");
}

}  // namespace

int main() {
    test_argv_is_fixed_and_bounded();
    test_argv_rejects_unsafe_input();
    test_success_returns_text_and_cost();
    test_budget_overrun_is_model_error_with_exit_one();
    test_timeout_is_reported();
    test_malformed_json_is_reported();
    test_non_zero_exit_with_success_json_is_not_ok();
    test_is_error_true_is_model_error();
    std::cout << (failures == 0 ? "ALL PASSED" : "FAILURES") << "\n";
    return failures == 0 ? 0 : 1;
}
