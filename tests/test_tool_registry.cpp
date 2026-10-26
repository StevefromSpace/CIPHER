#include "cipher/memory.hpp"
#include "cipher/tool_registry.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

// ---------------------------------------------------------------------------
// test_tool_registry.cpp  —  v0.5 Phase 1 and 2 tool primitives
//
// Covers registration and its rejections, lookup, the permission gate on
// "<tool> <target>", zero handler calls for denied actions, the executed flag,
// dry-run, and the session-only audit log. Same plain pass/fail bookkeeping
// as the other CIPHER suites.
// ---------------------------------------------------------------------------

namespace {

int g_failures = 0;
int g_passes   = 0;

void pass(const std::string& name) {
    std::cout << "PASS: " << name << '\n';
    ++g_passes;
}

void fail(const std::string& name, const std::string& reason) {
    std::cerr << "FAIL: " << name << "\n  " << reason << '\n';
    ++g_failures;
}

void check(bool condition, const std::string& name, const std::string& reason) {
    if (condition) pass(name);
    else           fail(name, reason);
}

// Builds a tool whose handler counts calls and returns the given output.
cipher::ToolDefinition counting_tool(const std::string& id, cipher::RiskTier tier, int& calls) {
    return cipher::ToolDefinition{
        id,
        "test tool",
        "test",
        tier,
        cipher::SideEffect::None,
        [&calls](const std::string& args) {
            ++calls;
            return "ran:" + args;
        }};
}

bool throws_invalid_argument(const std::function<void()>& action) {
    try {
        action();
    } catch (const std::invalid_argument&) {
        return true;
    } catch (...) {
    }
    return false;
}

// ---------------------------------------------------------------------------
void test_registration_accepts_valid_tool() {
    cipher::ToolRegistry registry;
    int calls = 0;
    bool ok = true;
    try {
        registry.register_tool(counting_tool("time.now", cipher::RiskTier::Safe, calls));
    } catch (...) {
        ok = false;
    }
    check(ok, "registration: valid tool accepted", "valid tool was rejected");
    check(registry.tool_ids().size() == 1, "registration: tool listed after registration",
          "tool_ids() did not include the new tool");
}

void test_registration_rejects_invalid_definitions() {
    int calls = 0;
    {
        cipher::ToolRegistry registry;
        check(throws_invalid_argument([&] {
                  registry.register_tool(counting_tool("Bad Id", cipher::RiskTier::Safe, calls));
              }),
              "registration: invalid id rejected", "uppercase/space id was accepted");
        check(throws_invalid_argument([&] {
                  registry.register_tool(counting_tool(".leading", cipher::RiskTier::Safe, calls));
              }),
              "registration: leading dot rejected", "leading-dot id was accepted");
    }
    {
        cipher::ToolRegistry registry;
        registry.register_tool(counting_tool("time.now", cipher::RiskTier::Safe, calls));
        check(throws_invalid_argument([&] {
                  registry.register_tool(counting_tool("time.now", cipher::RiskTier::Safe, calls));
              }),
              "registration: duplicate id rejected", "duplicate id was accepted");
    }
    {
        cipher::ToolRegistry registry;
        cipher::ToolDefinition no_handler{"x.y", "", "t", cipher::RiskTier::Safe, cipher::SideEffect::None, nullptr};
        check(throws_invalid_argument([&] { registry.register_tool(no_handler); }),
              "registration: null handler rejected", "null handler was accepted");
    }
    {
        cipher::ToolRegistry registry;
        cipher::ToolDefinition process{"proc.run", "", "t", cipher::RiskTier::Ask, cipher::SideEffect::Process,
                                       [](const std::string&) { return std::string{}; }};
        cipher::ToolDefinition network{"net.get", "", "t", cipher::RiskTier::Ask, cipher::SideEffect::Network,
                                       [](const std::string&) { return std::string{}; }};
        check(throws_invalid_argument([&] { registry.register_tool(process); }),
              "registration: process side effect rejected", "process tool was accepted");
        check(throws_invalid_argument([&] { registry.register_tool(network); }),
              "registration: network side effect rejected", "network tool was accepted");
    }
    {
        cipher::ToolRegistry registry;
        cipher::ToolDefinition safe_write{"file.w", "", "t", cipher::RiskTier::Safe, cipher::SideEffect::WriteLocal,
                                          [](const std::string&) { return std::string{}; }};
        check(throws_invalid_argument([&] { registry.register_tool(safe_write); }),
              "registration: Safe write tool rejected", "write tool defaulting to Safe was accepted");
    }
}

void test_lookup_and_not_found() {
    cipher::ToolRegistry registry;
    int calls = 0;
    registry.register_tool(counting_tool("time.now", cipher::RiskTier::Safe, calls));
    const cipher::ToolResult result = registry.invoke("missing.tool", "", "");
    check(result.outcome == cipher::ToolOutcome::NotFound, "lookup: unknown tool is NotFound",
          "unknown tool did not return NotFound");
    check(!result.executed && calls == 0, "lookup: NotFound executes nothing", "NotFound touched a handler");
}

void test_safe_and_monitor_run() {
    cipher::ToolRegistry registry;
    int safe_calls = 0;
    int monitor_calls = 0;
    registry.register_tool(counting_tool("safe.op", cipher::RiskTier::Safe, safe_calls));
    registry.register_tool(counting_tool("mon.op", cipher::RiskTier::Monitor, monitor_calls));

    const cipher::ToolResult safe = registry.invoke("safe.op", "", "a");
    check(safe.outcome == cipher::ToolOutcome::Completed && safe.executed && safe.output == "ran:a",
          "Safe: runs and reports Completed with executed=true",
          "Safe tool did not complete as expected");

    const cipher::ToolResult monitor = registry.invoke("mon.op", "", "b");
    check(monitor.outcome == cipher::ToolOutcome::Completed && monitor.executed,
          "Monitor: runs and reports Completed",
          "Monitor tool did not complete as expected");
}

void test_ask_and_block_are_denied_without_handler_call() {
    cipher::ToolRegistry registry;
    int ask_calls = 0;
    int block_calls = 0;
    registry.register_tool(counting_tool("ask.op", cipher::RiskTier::Ask, ask_calls));
    registry.register_tool(counting_tool("block.op", cipher::RiskTier::Block, block_calls));

    const cipher::ToolResult ask = registry.invoke("ask.op", "", "");
    check(ask.outcome == cipher::ToolOutcome::Denied && !ask.executed && ask_calls == 0,
          "Ask: denied, zero handler calls", "Ask tool ran or was not denied");

    const cipher::ToolResult block = registry.invoke("block.op", "", "");
    check(block.outcome == cipher::ToolOutcome::Denied && !block.executed && block_calls == 0,
          "Block: denied, zero handler calls", "Block tool ran or was not denied");
}

void test_target_specific_permissions() {
    cipher::ToolRegistry registry;
    int calls = 0;
    registry.register_tool(counting_tool("file.read", cipher::RiskTier::Monitor, calls));
    registry.policy().add_blacklist_pattern("file.read secret*");

    const cipher::ToolResult blocked = registry.invoke("file.read", "secret.txt", "");
    check(blocked.outcome == cipher::ToolOutcome::Denied && blocked.tier == cipher::RiskTier::Block && calls == 0,
          "target: blacklisted target denied with zero calls",
          "blacklisted target was not denied");

    const cipher::ToolResult allowed = registry.invoke("file.read", "notes.txt", "");
    check(allowed.outcome == cipher::ToolOutcome::Completed && calls == 1,
          "target: non-matching target still runs", "non-matching target was denied");
}

void test_dry_run_does_not_execute() {
    cipher::ToolRegistry registry;
    int calls = 0;
    registry.register_tool(counting_tool("safe.op", cipher::RiskTier::Safe, calls));
    registry.set_dry_run(true);
    const cipher::ToolResult result = registry.invoke("safe.op", "", "");
    check(result.outcome == cipher::ToolOutcome::DryRun && !result.executed && calls == 0,
          "dry-run: Safe tool reports DryRun with zero calls", "dry-run executed the handler");
}

void test_failed_handler_is_distinct_from_denied() {
    cipher::ToolRegistry registry;
    registry.register_tool(cipher::ToolDefinition{
        "boom.op", "", "t", cipher::RiskTier::Safe, cipher::SideEffect::None,
        [](const std::string&) -> std::string { throw std::runtime_error("disk exploded"); }});
    const cipher::ToolResult result = registry.invoke("boom.op", "", "");
    check(result.outcome == cipher::ToolOutcome::Failed && result.executed,
          "failed: handler exception yields Failed with executed=true",
          "handler exception was not reported as Failed+executed");
    check(result.output == "disk exploded", "failed: exception message surfaced",
          "exception message not surfaced");
}

void test_audit_records_denial_and_execution_without_payload() {
    cipher::ToolRegistry registry;
    int calls = 0;
    registry.register_tool(counting_tool("safe.op", cipher::RiskTier::Safe, calls));
    registry.register_tool(counting_tool("ask.op", cipher::RiskTier::Ask, calls));

    registry.invoke("safe.op", "t1", "secret-argument");
    registry.invoke("ask.op", "t2", "other-argument");

    const auto& log = registry.audit_log();
    check(log.size() == 2, "audit: one record per attempt", "audit log size was not 2");
    if (log.size() == 2) {
        check(log[0].executed && log[0].outcome == cipher::ToolOutcome::Completed,
              "audit: execution recorded as executed", "execution record wrong");
        check(!log[1].executed && log[1].outcome == cipher::ToolOutcome::Denied,
              "audit: denial recorded as not executed", "denial record wrong");
        check(log[0].argument_bytes == std::string("secret-argument").size(),
              "audit: argument size recorded, not contents", "argument size wrong");
        check(log[0].sequence + 1 == log[1].sequence && !log[0].correlation_id.empty(),
              "audit: sequence and correlation id assigned", "sequence or correlation id missing");
    }
}

void test_builtin_tools_registered_and_safe() {
    cipher::ToolRegistry registry;
    cipher::register_builtin_tools(registry);
    const cipher::ToolResult time = registry.invoke("time.now", "", "");
    const cipher::ToolResult info = registry.invoke("system.info", "", "");
    check(time.outcome == cipher::ToolOutcome::Completed && !time.output.empty(),
          "builtins: time.now completes with output", "time.now did not complete");
    check(info.outcome == cipher::ToolOutcome::Completed && info.output.rfind("CIPHER v", 0) == 0,
          "builtins: system.info reports version only", "system.info output wrong");
}

std::string temp_memory_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / ("cipher_memory_list_" + name + ".txt")).string();
}

void test_memory_list_returns_ids_only() {
    const std::string path = temp_memory_path("ids");
    cipher::MemoryStore store;
    store.set("alpha", "SECRETCONTENT", "general");
    const cipher::MemoryResult saved = store.save(path);

    cipher::ToolRegistry registry;
    cipher::register_memory_tools(registry, path);
    const cipher::ToolResult result = registry.invoke("memory.list", "", "");
    check(saved.is_ok() && result.outcome == cipher::ToolOutcome::Completed && result.executed &&
              result.output.find("alpha") != std::string::npos,
          "memory.list: lists entry IDs", "ID missing from listing");
    check(result.output.find("SECRETCONTENT") == std::string::npos,
          "memory.list: contents never returned", "entry content leaked into listing");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_memory_list_missing_file_completes_empty() {
    const std::string path = temp_memory_path("missing");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    cipher::ToolRegistry registry;
    cipher::register_memory_tools(registry, path);
    const cipher::ToolResult result = registry.invoke("memory.list", "", "");
    check(result.outcome == cipher::ToolOutcome::Completed && result.output == "No memory entries.",
          "memory.list: missing file completes with empty listing", "missing file did not complete empty");
}

void test_memory_list_corrupt_file_fails_not_denied() {
    const std::string path = temp_memory_path("corrupt");
    {
        std::ofstream out(path);
        out << "this is not a memory record\n";
    }

    cipher::ToolRegistry registry;
    cipher::register_memory_tools(registry, path);
    const cipher::ToolResult result = registry.invoke("memory.list", "", "");
    check(result.outcome == cipher::ToolOutcome::Failed && result.executed,
          "memory.list: corrupt file is Failed with executed=true", "corrupt file did not fail as executed");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_memory_list_blacklist_denies_without_reading() {
    const std::string path = temp_memory_path("blacklist");
    cipher::MemoryStore store;
    store.set("alpha", "content", "general");
    store.save(path);

    cipher::ToolRegistry registry;
    cipher::register_memory_tools(registry, path);
    registry.policy().add_blacklist_pattern("memory.list secret*");
    const cipher::ToolResult result = registry.invoke("memory.list", "secret-area", "");
    check(result.outcome == cipher::ToolOutcome::Denied && !result.executed && result.output.find("alpha") == std::string::npos,
          "memory.list: blacklisted target denied, nothing listed", "blacklisted memory.list was not denied");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

} // namespace

int main() {
    test_registration_accepts_valid_tool();
    test_registration_rejects_invalid_definitions();
    test_lookup_and_not_found();
    test_safe_and_monitor_run();
    test_ask_and_block_are_denied_without_handler_call();
    test_target_specific_permissions();
    test_dry_run_does_not_execute();
    test_failed_handler_is_distinct_from_denied();
    test_audit_records_denial_and_execution_without_payload();
    test_builtin_tools_registered_and_safe();
    test_memory_list_returns_ids_only();
    test_memory_list_missing_file_completes_empty();
    test_memory_list_corrupt_file_fails_not_denied();
    test_memory_list_blacklist_denies_without_reading();

    std::cout << "\n" << g_passes << " passed, " << g_failures << " failed\n";
    return g_failures == 0 ? 0 : 1;
}
