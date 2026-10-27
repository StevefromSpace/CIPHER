#include "cipher/memory.hpp"
#include "cipher/tool_commands.hpp"

#include <filesystem>
#include <iostream>
#include <string>

// ---------------------------------------------------------------------------
// test_tool_commands.cpp  —  v0.5 Phase 3 owner `tool` REPL commands
//
// Covers input routing (non-tool input is not handled), usage, list, invoke
// through the permission gate, memory.list only when a memory file is set,
// and a log that never shows arguments or tool output. Same plain pass/fail
// bookkeeping as the other CIPHER suites.
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

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// Writes a real memory file with one entry so memory.list has content to hide.
std::string write_memory_file(const std::filesystem::path& path) {
    cipher::MemoryStore store;
    store.set("alpha", "SECRET-BODY-TEXT");
    store.save(path.string());
    return path.string();
}

} // namespace

int main() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "cipher_tool_commands_tests";
    std::filesystem::create_directories(dir);
    const std::string memory_path = write_memory_file(dir / "memory.jsonl");

    // --- routing -----------------------------------------------------------
    {
        cipher::ToolCommands tools("", "");
        check(!tools.execute("time").handled, "routing: non-tool word is not handled",
              "expected 'time' to pass through to the core");
        check(!tools.execute("toolbox list").handled, "routing: prefix of 'tool' is not handled",
              "expected 'toolbox' to pass through");
        check(tools.execute("  tool   ").handled, "routing: bare tool with spaces is handled",
              "expected trimmed bare 'tool' to be handled");
    }

    // --- usage and list ----------------------------------------------------
    {
        cipher::ToolCommands tools("", "");
        const cipher::ToolCommandResult usage = tools.execute("tool");
        check(usage.handled && contains(usage.text, "tool list") && contains(usage.text, "tool log"),
              "usage: bare tool shows usage", "usage text missing: " + usage.text);

        const cipher::ToolCommandResult listed = tools.execute("tool list");
        check(contains(listed.text, "time.now") && contains(listed.text, "[safe]"),
              "list: built-in time.now with tier", listed.text);
        check(contains(listed.text, "system.info"), "list: built-in system.info", listed.text);
        check(!contains(listed.text, "memory.list"), "list: no memory.list without memory_file",
              listed.text);
    }

    {
        cipher::ToolCommands tools(memory_path, "");
        check(contains(tools.execute("tool list").text, "memory.list  [monitor]"),
              "list: memory.list registered when memory_file is set", "memory.list missing");
    }

    // --- invocation --------------------------------------------------------
    {
        cipher::ToolCommands tools("", "");
        const cipher::ToolCommandResult timed = tools.execute("tool time.now");
        check(timed.handled && contains(timed.text, "time.now: Completed (tier safe, executed)"),
              "invoke: time.now completes and is marked executed", timed.text);

        const cipher::ToolCommandResult unknown = tools.execute("tool no.such.tool secret-arg");
        check(contains(unknown.text, "NotFound") && contains(unknown.text, "not executed"),
              "invoke: unknown tool is NotFound and not executed", unknown.text);
    }

    {
        cipher::ToolCommands tools(memory_path, "");
        const cipher::ToolCommandResult listed = tools.execute("tool memory.list");
        check(contains(listed.text, "alpha") && !contains(listed.text, "SECRET-BODY-TEXT"),
              "invoke: memory.list shows IDs only", listed.text);
    }

    // --- permission denial: handler must not run ----------------------------
    {
        cipher::ToolCommands tools("", "");
        tools.registry().policy().add_blacklist_pattern("time.now*");
        const cipher::ToolCommandResult denied = tools.execute("tool time.now");
        check(contains(denied.text, "Denied") && contains(denied.text, "not executed"),
              "permission: blacklisted tool is Denied and not executed", denied.text);
        check(!contains(denied.text, "Completed"), "permission: denial never reports Completed",
              denied.text);
    }

    // --- log: attempts recorded, arguments and output never shown -----------
    {
        cipher::ToolCommands tools("", "");
        check(contains(tools.execute("tool log").text, "No tool attempts this session."),
              "log: empty before any attempt", "expected empty-log message");

        tools.execute("tool system.info ARG-SECRET-VALUE");
        const cipher::ToolCommandResult logged = tools.execute("tool log");
        check(contains(logged.text, "system.info") && contains(logged.text, "argument_bytes=16"),
              "log: records tool id and argument length", logged.text);
        check(!contains(logged.text, "ARG-SECRET-VALUE") && !contains(logged.text, "CIPHER v"),
              "log: never shows arguments or output", logged.text);
    }

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);

    std::cout << '\n' << g_passes << " passed, " << g_failures << " failed\n";
    return g_failures == 0 ? 0 : 1;
}
