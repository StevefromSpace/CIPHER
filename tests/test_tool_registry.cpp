#include "cipher/tool_registry.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

int g_failures = 0;
int g_passes   = 0;

void check(bool condition, const std::string& name, const std::string& reason) {
    if (condition) {
        std::cout << "PASS: " << name << '\n';
        ++g_passes;
    } else {
        std::cerr << "FAIL: " << name << "\n  " << reason << '\n';
        ++g_failures;
    }
}

cipher::ActionDescriptor make_action(const std::string& name, cipher::RiskTier tier) {
    cipher::ActionDescriptor action;
    action.name = name;
    action.default_tier = tier;
    return action;
}

// Each handler records how often it ran so tests can prove denial skipped it.
int g_calls = 0;
std::string echo_handler(const std::string& input) {
    ++g_calls;
    return "echo:" + input;
}

void test_safe_tool_executes() {
    cipher::ToolRegistry registry;
    g_calls = 0;
    registry.register_tool(make_action("read.safe", cipher::RiskTier::Safe), echo_handler);

    const auto result = registry.invoke("read.safe", "x");
    check(result.outcome == cipher::ToolOutcome::Executed && result.output == "echo:x",
          "safe tool executes and returns output",
          "outcome or output mismatch");
    check(g_calls == 1, "safe tool handler called exactly once", "handler call count wrong");
}

void test_monitor_tool_executes_and_logs() {
    cipher::ToolRegistry registry;
    g_calls = 0;
    registry.register_tool(make_action("read.monitor", cipher::RiskTier::Monitor), echo_handler);

    const auto result = registry.invoke("read.monitor", "y");
    check(result.outcome == cipher::ToolOutcome::Executed && result.tier == cipher::RiskTier::Monitor,
          "monitor tool executes with Monitor tier",
          "monitor tool was not executed");
    check(registry.log().size() == 1 && registry.log()[0].tool == "read.monitor",
          "monitor invocation is logged",
          "log entry missing");
}

void test_ask_and_block_are_denied_without_handler() {
    cipher::ToolRegistry registry;
    g_calls = 0;
    registry.register_tool(make_action("write.ask", cipher::RiskTier::Ask), echo_handler);
    registry.register_tool(make_action("write.block", cipher::RiskTier::Block), echo_handler);

    const auto ask = registry.invoke("write.ask", "z");
    const auto block = registry.invoke("write.block", "z");
    check(ask.outcome == cipher::ToolOutcome::Denied && block.outcome == cipher::ToolOutcome::Denied,
          "Ask and Block tools are denied",
          "a non-permitted tool was not denied");
    check(g_calls == 0, "denied tools never invoke the handler", "handler ran despite denial");
    check(registry.log().size() == 2, "denials are logged", "log size mismatch");
}

void test_blacklist_overrides_whitelist() {
    cipher::ToolRegistry registry;
    g_calls = 0;
    registry.policy().add_whitelist_pattern("shell.*");
    registry.policy().add_blacklist_pattern("shell.rm");
    registry.register_tool(make_action("shell.rm", cipher::RiskTier::Safe), echo_handler);

    const auto result = registry.invoke("shell.rm", "/");
    check(result.outcome == cipher::ToolOutcome::Denied && result.tier == cipher::RiskTier::Block,
          "blacklist overrides whitelist and denies",
          "blacklisted tool was permitted");
    check(g_calls == 0, "blacklisted tool handler never runs", "handler ran despite blacklist");
}

void test_unknown_tool_is_not_found() {
    cipher::ToolRegistry registry;
    const auto result = registry.invoke("missing", "");
    check(result.outcome == cipher::ToolOutcome::NotFound,
          "unknown tool reports NotFound",
          "unexpected outcome for unknown tool");
    check(registry.log().size() == 1, "unknown tool invocation is logged", "log entry missing");
}

void test_dry_run_skips_handler() {
    cipher::ToolRegistry registry;
    g_calls = 0;
    registry.set_dry_run(true);
    registry.register_tool(make_action("read.safe", cipher::RiskTier::Safe), echo_handler);

    const auto result = registry.invoke("read.safe", "q");
    check(result.outcome == cipher::ToolOutcome::DryRun && result.output.empty(),
          "dry-run reports DryRun without output",
          "dry-run did not skip execution");
    check(g_calls == 0, "dry-run never calls the handler", "handler ran in dry-run mode");
}

void test_handler_exception_is_failed_not_thrown() {
    cipher::ToolRegistry registry;
    registry.register_tool(make_action("read.boom", cipher::RiskTier::Safe),
        [](const std::string&) -> std::string { throw std::runtime_error("boom"); });

    const auto result = registry.invoke("read.boom", "");
    check(result.outcome == cipher::ToolOutcome::Failed && result.output == "boom",
          "throwing handler yields Failed with message",
          "exception escaped or message lost");
}

void test_registration_rejects_empty_duplicate_and_null() {
    cipher::ToolRegistry registry;
    check(!registry.register_tool(make_action("", cipher::RiskTier::Safe), echo_handler),
          "empty tool name is rejected",
          "empty name was accepted");
    check(!registry.register_tool(make_action("nohandler", cipher::RiskTier::Safe), nullptr),
          "null handler is rejected",
          "null handler was accepted");
    check(registry.register_tool(make_action("dup", cipher::RiskTier::Safe), echo_handler),
          "first registration succeeds",
          "first registration failed");
    check(!registry.register_tool(make_action("dup", cipher::RiskTier::Safe), echo_handler),
          "duplicate name is rejected",
          "duplicate registration was accepted");
}

} // namespace

int main() {
    std::cout << "Running CIPHER Tool Registry Test Suite...\n\n";

    test_safe_tool_executes();
    test_monitor_tool_executes_and_logs();
    test_ask_and_block_are_denied_without_handler();
    test_blacklist_overrides_whitelist();
    test_unknown_tool_is_not_found();
    test_dry_run_skips_handler();
    test_handler_exception_is_failed_not_thrown();
    test_registration_rejects_empty_duplicate_and_null();

    std::cout << "\nTests passed: " << g_passes << '\n';
    std::cout << "Tests failed: " << g_failures << '\n';

    return (g_failures == 0) ? 0 : 1;
}
