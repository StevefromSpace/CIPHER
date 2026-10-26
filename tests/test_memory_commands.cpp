#include "cipher/memory_commands.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// test_memory_commands.cpp  —  Tests for the REPL memory command layer
//
// Uses the same plain pass/fail bookkeeping as the other CIPHER test
// suites. Every test works on its own file under the system temp directory
// so runs never touch a real memory file.
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

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

std::filesystem::path test_dir() {
    const auto dir = std::filesystem::temp_directory_path() / "cipher_memory_commands_tests";
    std::filesystem::create_directories(dir);
    return dir;
}

// Returns a path for one test that does not exist yet.
std::string fresh_path(const std::string& name) {
    const auto path = test_dir() / (name + ".txt");
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".tmp");
    return path.string();
}

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// Opens a fresh command layer on `path`, as a new CIPHER session would.
cipher::MemoryCommands open_session(const std::string& path) {
    cipher::MemoryCommands session(path);
    session.open();
    return session;
}

// ---------------------------------------------------------------------------
// Test sections
// ---------------------------------------------------------------------------

void test_disabled_when_no_path() {
    cipher::MemoryCommands session("");
    const auto open_result = session.open();
    check(open_result.is_ok(), "disabled: open succeeds without a path",
          "open() must succeed when memory is disabled");

    const auto result = session.execute("remember note hello");
    check(result.handled && contains(result.text, "Memory is disabled"),
          "disabled: remember explains that memory is disabled",
          "got: " + result.text);
}

void test_help_addendum_only_when_enabled() {
    cipher::MemoryCommands disabled("");
    disabled.open();
    check(disabled.execute("help").text.empty(),
          "help: no addendum when memory is disabled",
          "help addendum must be empty when disabled");

    cipher::MemoryCommands enabled(fresh_path("help_enabled"));
    enabled.open();
    const auto result = enabled.execute("help");
    check(!result.handled && contains(result.text, "remember") && contains(result.text, "forget"),
          "help: addendum lists memory commands when enabled",
          "got: " + result.text);
}

void test_non_memory_input_is_not_handled() {
    cipher::MemoryCommands session(fresh_path("not_ours"));
    session.open();
    check(!session.execute("time").handled, "routing: time is not a memory command",
          "time must fall through to the core");
    check(!session.execute("   ").handled, "routing: blank input is not a memory command",
          "blank input must fall through to the core");
}

void test_remember_recall_across_sessions() {
    const std::string path = fresh_path("round_trip");
    {
        auto session = open_session(path);
        const auto saved = session.execute("remember favourite_colour  deep blue, with   spaces ");
        check(saved.handled && contains(saved.text, "Remembered 'favourite_colour'"),
              "round trip: remember confirms the id",
              "got: " + saved.text);
    }
    auto reopened = open_session(path);
    const auto recalled = reopened.execute("recall favourite_colour");
    check(recalled.handled && recalled.text == "deep blue, with   spaces",
          "round trip: recall after reload returns the saved text",
          "got: '" + recalled.text + "'");
}

void test_update_keeps_existing_category() {
    const std::string path = fresh_path("keeps_category");
    cipher::MemoryStore seed;
    seed.set("pref_tea", "green", "preference");
    seed.save(path);

    auto session = open_session(path);
    const auto result = session.execute("remember pref_tea oolong");
    check(contains(result.text, "Updated 'pref_tea'"),
          "update: reports that an existing id was updated",
          "got: " + result.text);

    cipher::MemoryStore reloaded;
    reloaded.load(path);
    const auto entry = reloaded.get("pref_tea");
    check(entry && entry->content == "oolong" && entry->category == "preference",
          "update: new text is saved and the hand-set category is kept",
          "category or content was not preserved on update");
}

void test_forget_persists_across_sessions() {
    const std::string path = fresh_path("forget_persists");
    auto session = open_session(path);
    session.execute("remember temp_note keep until deleted");

    const auto forgotten = session.execute("forget temp_note");
    check(forgotten.handled && contains(forgotten.text, "Forgot 'temp_note'"),
          "forget: confirms removal",
          "got: " + forgotten.text);

    auto reopened = open_session(path);
    check(contains(reopened.execute("recall temp_note").text, "No memory with id 'temp_note'"),
          "forget: removal survives a reload",
          "entry still recalled after reload");
}

void test_forget_unknown_id_reports_not_found() {
    cipher::MemoryCommands session(fresh_path("forget_unknown"));
    session.open();
    const auto result = session.execute("forget missing");
    check(contains(result.text, "No memory with id 'missing'"),
          "forget: unknown id is reported, not silently accepted",
          "got: " + result.text);
}

void test_invalid_id_is_rejected_and_not_stored() {
    const std::string path = fresh_path("invalid_id");
    auto session = open_session(path);
    const auto result = session.execute("remember bad/id value");
    check(contains(result.text, "Invalid memory entry ID"),
          "validation: id with a slash is rejected with the store message",
          "got: " + result.text);
    check(!std::filesystem::exists(path),
          "validation: rejected remember writes nothing to disk",
          "a file was created for a rejected entry");
}

void test_usage_messages() {
    cipher::MemoryCommands session(fresh_path("usage"));
    session.open();
    check(contains(session.execute("remember").text, "Usage: remember <id> <text>"),
          "usage: remember without arguments shows usage",
          "remember with no arguments did not show usage");
    check(contains(session.execute("remember only_id").text, "Usage: remember <id> <text>"),
          "usage: remember without text shows usage",
          "remember without text did not show usage");
    check(contains(session.execute("recall").text, "Usage: recall <id>"),
          "usage: recall without id shows usage",
          "recall with no id did not show usage");
    check(contains(session.execute("recall two words").text, "Usage: recall <id>"),
          "usage: recall with extra words shows usage",
          "recall with extra words did not show usage");
}

void test_memories_lists_ids() {
    cipher::MemoryCommands session(fresh_path("list_ids"));
    session.open();
    check(contains(session.execute("memories").text, "No memories stored."),
          "list: empty store says so",
          "empty store listing was wrong");
    session.execute("remember alpha one");
    session.execute("remember beta two");
    const auto listing = session.execute("memories").text;
    check(contains(listing, "alpha") && contains(listing, "beta"),
          "list: stored ids are listed",
          "got: " + listing);
}

void test_missing_file_starts_empty() {
    cipher::MemoryCommands session(fresh_path("missing_file"));
    const auto open_result = session.open();
    check(open_result.is_ok(), "open: a missing file is not an error",
          "open() reported an error for a missing file");
    check(contains(session.execute("memories").text, "No memories stored."),
          "open: a missing file starts with an empty store",
          "missing file did not start empty");
}

void test_corrupt_file_is_never_overwritten() {
    const std::string path = fresh_path("corrupt_guard");
    const std::string original = "this is not a cipher memory file\n";
    {
        std::ofstream file(path, std::ios::binary);
        file << original;
    }

    cipher::MemoryCommands session(path);
    const auto open_result = session.open();
    check(!open_result.is_ok(), "corrupt: open reports the broken file",
          "open() accepted a corrupt memory file");

    const auto result = session.execute("remember note would overwrite");
    check(contains(result.text, "Memory is unavailable"),
          "corrupt: remember refuses to run after a failed load",
          "got: " + result.text);
    check(read_file(path) == original,
          "corrupt: the broken file is byte-for-byte unchanged",
          "the corrupt file was modified");
}

void test_failed_save_rolls_back_memory() {
    const auto missing_dir = test_dir() / "no_such_directory" / "memory.txt";
    cipher::MemoryCommands session(missing_dir.string());
    session.open();

    const auto result = session.execute("remember note cannot be saved");
    check(contains(result.text, "Could not save memory") && contains(result.text, "Nothing was changed"),
          "save failure: the user is told the save failed",
          "got: " + result.text);
    check(contains(session.execute("recall note").text, "No memory with id 'note'"),
          "save failure: the in-memory store does not keep the unsaved entry",
          "unsaved entry is still visible in memory");
}

// A session whose policy blacklists remember and forget on ids starting with "secret".
// evaluate() matches "<action> <id>", so these patterns are "<action> secret*".
cipher::MemoryCommands open_gated_session(const std::string& path) {
    cipher::PermissionPolicy policy;
    policy.add_blacklist_pattern("remember secret*");
    policy.add_blacklist_pattern("forget secret*");
    cipher::MemoryCommands session(path, policy);
    session.open();
    return session;
}

void test_gate_allows_monitor_remember_and_forget() {
    const std::string path = fresh_path("gate_allows");
    auto session = open_gated_session(path);
    const auto saved = session.execute("remember note allowed text");
    check(contains(saved.text, "Remembered 'note'"),
          "gate: allowed remember (Monitor default) succeeds",
          "got: " + saved.text);
    const auto forgotten = session.execute("forget note");
    check(contains(forgotten.text, "Forgot 'note'"),
          "gate: allowed forget (Monitor default) succeeds",
          "got: " + forgotten.text);
}

void test_gate_denied_remember_leaves_file_byte_identical() {
    const std::string path = fresh_path("gate_denied_remember");
    {
        cipher::MemoryCommands seed(path);
        seed.open();
        seed.execute("remember keep_me original text");
    }
    const std::string before = read_file(path);

    auto session = open_gated_session(path);
    const auto result = session.execute("remember secret_key new value");
    check(contains(result.text, "Denied by permission policy") && contains(result.text, "Nothing was changed"),
          "gate: denied remember tells the user it was denied",
          "got: " + result.text);
    check(read_file(path) == before,
          "gate: denied remember leaves the memory file byte-identical",
          "the memory file changed after a denied remember");
    check(contains(session.execute("recall keep_me").text, "original text"),
          "gate: existing contents remain after a denied remember",
          "existing entry was altered");
    check(contains(session.execute("recall secret_key").text, "No memory with id 'secret_key'"),
          "gate: denied remember does not change the in-memory store",
          "denied entry is visible in memory");
}

void test_gate_denied_forget_leaves_file_byte_identical() {
    const std::string path = fresh_path("gate_denied_forget");
    {
        cipher::MemoryCommands seed(path);
        seed.open();
        seed.execute("remember secret_key protected");
    }
    const std::string before = read_file(path);

    auto session = open_gated_session(path);
    const auto result = session.execute("forget secret_key");
    check(contains(result.text, "Denied by permission policy"),
          "gate: denied forget tells the user it was denied",
          "got: " + result.text);
    check(read_file(path) == before,
          "gate: denied forget leaves the memory file byte-identical",
          "the memory file changed after a denied forget");
    check(contains(session.execute("recall secret_key").text, "protected"),
          "gate: denied forget keeps the entry in memory",
          "entry was removed despite the denial");
}

void test_usage_and_routing_do_not_need_disk() {
    cipher::MemoryCommands session("");
    session.open();
    check(contains(session.execute("recall").text, "Memory is disabled"),
          "disabled: recall reports disabled before usage",
          "disabled check did not come first");
}

} // namespace

int main() {
    test_disabled_when_no_path();
    test_help_addendum_only_when_enabled();
    test_non_memory_input_is_not_handled();
    test_remember_recall_across_sessions();
    test_update_keeps_existing_category();
    test_forget_persists_across_sessions();
    test_forget_unknown_id_reports_not_found();
    test_invalid_id_is_rejected_and_not_stored();
    test_usage_messages();
    test_memories_lists_ids();
    test_missing_file_starts_empty();
    test_corrupt_file_is_never_overwritten();
    test_failed_save_rolls_back_memory();
    test_usage_and_routing_do_not_need_disk();
    test_gate_allows_monitor_remember_and_forget();
    test_gate_denied_remember_leaves_file_byte_identical();
    test_gate_denied_forget_leaves_file_byte_identical();

    std::cout << '\n';
    if (g_failures != 0) {
        std::cerr << g_failures << " memory command test(s) failed.\n";
        return 1;
    }
    std::cout << "All " << g_passes << " memory command tests passed.\n";
    return 0;
}
