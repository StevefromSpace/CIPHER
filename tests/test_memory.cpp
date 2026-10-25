// ---------------------------------------------------------------------------
// tests/test_memory.cpp  —  Unit and Regression Tests for CIPHER Memory
// ---------------------------------------------------------------------------

#include "cipher/memory.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif


namespace {

int g_passes = 0;
int g_failures = 0;

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
    else fail(name, reason);
}

const std::string TEMP_TEST_DIR = "test_memory_tmp_dir";
const std::string TEMP_MEM_FILE = "test_memory_tmp_dir/cipher_memory.v1";

void cleanup_temp() {
    std::error_code ec;
    std::filesystem::remove_all(TEMP_TEST_DIR, ec);
}

void setup_temp() {
    cleanup_temp();
    std::error_code ec;
    std::filesystem::create_directories(TEMP_TEST_DIR, ec);
}

// ---------------------------------------------------------------------------
// 1. Missing storage file
// ---------------------------------------------------------------------------
void test_missing_storage_file() {
    cipher::MemoryStore store;
    auto res = store.load("test_memory_tmp_dir/does_not_exist.mem");
    check(res.is_ok(), "missing storage: returns ok", "Expected ok=true for missing storage file");
    check(store.empty(), "missing storage: store is empty", "Expected store to be empty");
    check(store.schema_version() == cipher::MemoryStore::CURRENT_SCHEMA_VERSION,
          "missing storage: default schema version set", "Expected schema_version to be CURRENT_SCHEMA_VERSION");
}

// ---------------------------------------------------------------------------
// 2. Empty storage
// ---------------------------------------------------------------------------
void test_empty_storage() {
    setup_temp();
    {
        std::ofstream f(TEMP_MEM_FILE);
        // empty file
    }

    cipher::MemoryStore store;
    auto res = store.load(TEMP_MEM_FILE);
    check(res.is_ok(), "empty storage: returns ok", "Expected ok=true for empty storage file");
    check(store.size() == 0, "empty storage: 0 entries", "Expected 0 entries");
    check(store.empty(), "empty storage: empty() is true", "Expected empty() == true");
    cleanup_temp();
}

// ---------------------------------------------------------------------------
// 3. Save-and-load round trips
// ---------------------------------------------------------------------------
void test_save_and_load_round_trip() {
    setup_temp();
    cipher::MemoryStore store;
    store.set("user_alias", "Steve", "preference");
    store.set("system_rule", "Never run cloud APIs.", "guideline");

    auto save_res = store.save(TEMP_MEM_FILE);
    check(save_res.is_ok(), "round trip: save succeeded", "Failed to save memory file");

    cipher::MemoryStore loaded;
    auto load_res = loaded.load(TEMP_MEM_FILE);
    check(load_res.is_ok(), "round trip: load succeeded", "Failed to load memory file");
    check(loaded.size() == 2, "round trip: size matches 2", "Expected size to be 2");

    auto alias = loaded.get("user_alias");
    check(alias.has_value(), "round trip: user_alias exists", "user_alias not found");
    if (alias) {
        check(alias->content == "Steve", "round trip: content matches", "Content mismatch: " + alias->content);
        check(alias->category == "preference", "round trip: category matches", "Category mismatch: " + alias->category);
        check(alias->created_at > 0, "round trip: created_at valid", "Expected valid timestamp");
        check(alias->updated_at >= alias->created_at, "round trip: updated_at valid", "Invalid updated_at timestamp");
    }

    auto rule = loaded.get("system_rule");
    check(rule.has_value(), "round trip: system_rule exists", "system_rule not found");
    if (rule) {
        check(rule->content == "Never run cloud APIs.", "round trip: rule content matches", "Rule content mismatch");
        check(rule->category == "guideline", "round trip: rule category matches", "Rule category mismatch");
    }
    cleanup_temp();
}

// ---------------------------------------------------------------------------
// 4. Multiple entries and update behavior
// ---------------------------------------------------------------------------
void test_multiple_entries_and_update() {
    cipher::MemoryStore store;
    store.set("item_1", "Alpha", "cat_a");
    store.set("item_2", "Beta", "cat_b");
    store.set("item_3", "Gamma", "cat_c");

    check(store.size() == 3, "multiple entries: initial size is 3", "Expected 3 entries");

    auto entry_before = store.get("item_2");
    check(entry_before.has_value(), "multiple entries: item_2 exists", "item_2 not found");
    int64_t created_orig = entry_before ? entry_before->created_at : 0;

    // Update item_2
    bool updated = store.set("item_2", "Beta Updated Content", "cat_updated");
    check(updated, "update entry: set returns true", "Expected update to succeed");
    check(store.size() == 3, "update entry: size remains 3", "Size changed after update");

    auto entry_after = store.get("item_2");
    check(entry_after.has_value(), "update entry: item_2 found after update", "item_2 missing");
    if (entry_after) {
        check(entry_after->content == "Beta Updated Content", "update entry: content updated", "Content was not updated");
        check(entry_after->category == "cat_updated", "update entry: category updated", "Category was not updated");
        check(entry_after->created_at == created_orig, "update entry: created_at preserved", "created_at was mutated");
        check(entry_after->updated_at >= created_orig, "update entry: updated_at updated", "updated_at invalid");
    }
}

// ---------------------------------------------------------------------------
// 5. Delete behavior
// ---------------------------------------------------------------------------
void test_delete_behavior() {
    setup_temp();
    cipher::MemoryStore store;
    store.set("doc_1", "Val 1");
    store.set("doc_2", "Val 2");

    check(store.has("doc_1"), "delete: doc_1 exists before deletion", "doc_1 missing");
    bool removed = store.remove("doc_1");
    check(removed, "delete: remove returns true for existing entry", "remove failed");
    check(!store.has("doc_1"), "delete: doc_1 gone after deletion", "doc_1 still exists");
    check(store.size() == 1, "delete: size decremented to 1", "Expected size 1");

    bool removed_again = store.remove("doc_1");
    check(!removed_again, "delete: remove returns false for nonexistent entry", "Expected false on second remove");

    // Persist and reload to ensure deletion is durable on disk
    store.save(TEMP_MEM_FILE);
    cipher::MemoryStore loaded;
    loaded.load(TEMP_MEM_FILE);
    check(!loaded.has("doc_1"), "delete durable: doc_1 absent after reload", "doc_1 reappeared after reload");
    check(loaded.has("doc_2"), "delete durable: doc_2 remains after reload", "doc_2 missing after reload");
    cleanup_temp();
}

// ---------------------------------------------------------------------------
// 6. Unicode and special-character preservation
// ---------------------------------------------------------------------------
void test_unicode_and_special_character_preservation() {
    setup_temp();
    cipher::MemoryStore store;

    std::string complex_content =
        "Line 1 with quotes: \"Hello, world!\"\n"
        "Line 2 with backslashes: C:\\Program Files\\CIPHER\\path\n"
        "Line 3 with tabs:\tcol1\tcol2\tcol3\r\n"
        "Line 4 with Multilingual UTF-8: 日本語, Español, Français (Café & Naïve), and emoji 🧠 🚀.";

    store.set("complex_entry", complex_content, "unicode_test");
    auto save_res = store.save(TEMP_MEM_FILE);
    check(save_res.is_ok(), "unicode: save ok", "Failed to save complex unicode content");

    cipher::MemoryStore loaded;
    auto load_res = loaded.load(TEMP_MEM_FILE);
    check(load_res.is_ok(), "unicode: load ok", "Failed to load complex unicode content");

    auto entry = loaded.get("complex_entry");
    check(entry.has_value(), "unicode: complex_entry found", "complex_entry missing");
    if (entry) {
        check(entry->content == complex_content,
              "unicode: exact content byte preservation",
              "Complex content mismatch across save/load cycle");
    }
    cleanup_temp();
}

// ---------------------------------------------------------------------------
// 7. Invalid input and duplicate-entry handling
// ---------------------------------------------------------------------------
void test_invalid_input_and_duplicates() {
    cipher::MemoryStore store;

    // Invalid ID cases
    std::string err;
    check(!store.set("", "content", "cat", &err), "invalid input: empty ID rejected", "Empty ID should be rejected");
    check(!err.empty(), "invalid input: error message populated", "Empty ID should produce error message");

    check(!store.set("id with spaces", "content", "cat"), "invalid input: spaces rejected", "ID with spaces should fail");
    check(!store.set("id[with]brackets", "content", "cat"), "invalid input: brackets rejected", "ID with brackets should fail");
    check(!store.set("id=equals", "content", "cat"), "invalid input: equals rejected", "ID with equals should fail");
    check(!store.set("id\nnewline", "content", "cat"), "invalid input: newlines rejected", "ID with newlines should fail");

    // Valid ID cases
    check(store.set("valid_id-1.test", "content"), "valid input: alphanumeric with _-. accepted", "Valid ID failed");

    // Duplicate entries in serialized text
    std::string duplicate_text =
        "schema_version = 1\n"
        "[entry:dup_key]\n"
        "content = first occurrence\n\n"
        "[entry:dup_key]\n"
        "content = second occurrence\n";

    cipher::MemoryStore dup_store;
    auto res = dup_store.load_from_string(duplicate_text);
    check(!res.is_ok(), "duplicates: duplicate section rejected", "Duplicate entry ID was not rejected");
    check(res.errors.size() >= 1, "duplicates: error reported", "Expected duplicate error");
}

// ---------------------------------------------------------------------------
// 8. Corrupted storage
// ---------------------------------------------------------------------------
void test_corrupted_storage() {
    cipher::MemoryStore store;
    store.set("clean_key", "Clean original value");

    // Case A: Syntax error (no '=' and not a section)
    std::string corrupt_syntax =
        "schema_version = 1\n"
        "this is completely corrupted syntax line\n"
        "[entry:k1]\n"
        "content = val1\n";

    auto res1 = store.load_from_string(corrupt_syntax);
    check(!res1.is_ok(), "corrupt storage: syntax error rejected", "Syntax error was not rejected");
    check(res1.errors.size() >= 1, "corrupt storage: error count > 0", "Expected errors");
    check(res1.errors[0].line == 2, "corrupt storage: line 2 reported", "Reported line: " + std::to_string(res1.errors[0].line));

    // Case B: Non-numeric timestamp
    std::string corrupt_ts =
        "schema_version = 1\n"
        "[entry:k1]\n"
        "created_at = NOT_A_NUMBER\n"
        "content = val1\n";

    auto res2 = store.load_from_string(corrupt_ts);
    check(!res2.is_ok(), "corrupt storage: invalid timestamp rejected", "Invalid timestamp accepted");

    // Case C: Unknown entry attribute
    std::string corrupt_attr =
        "schema_version = 1\n"
        "[entry:k1]\n"
        "unsupported_attribute = oops\n"
        "content = val1\n";

    auto res3 = store.load_from_string(corrupt_attr);
    check(!res3.is_ok(), "corrupt storage: unknown attribute rejected", "Unknown attribute accepted");

    // Verify in-memory state remained uncorrupted after failed parses
    check(store.has("clean_key"), "corrupt storage: previous valid store preserved", "Store corrupted by failed load");
    auto clean_entry = store.get("clean_key");
    if (clean_entry) {
        check(clean_entry->content == "Clean original value", "corrupt storage: clean value intact", "Clean value mutated");
    }
}

// ---------------------------------------------------------------------------
// 9. Unsupported schema versions
// ---------------------------------------------------------------------------
void test_unsupported_schema_versions() {
    cipher::MemoryStore store;

    // Future version 999
    std::string future_version =
        "schema_version = 999\n"
        "[entry:test]\n"
        "content = val\n";

    auto res_future = store.load_from_string(future_version);
    check(!res_future.is_ok(), "unsupported schema: version 999 rejected", "Future schema version was accepted");
    bool found_msg = false;
    for (const auto& err : res_future.errors) {
        if (err.message.find("Unsupported schema version") != std::string::npos) {
            found_msg = true;
            break;
        }
    }
    check(found_msg, "unsupported schema: descriptive error message", "Expected 'Unsupported schema version' message");

    // Negative version -1
    std::string neg_version =
        "schema_version = -1\n"
        "[entry:test]\n"
        "content = val\n";

    auto res_neg = store.load_from_string(neg_version);
    check(!res_neg.is_ok(), "unsupported schema: negative version rejected", "Negative schema version was accepted");
}

// ---------------------------------------------------------------------------
// 10. Known supported schema migration (Schema 0 -> Schema 1)
// ---------------------------------------------------------------------------
void test_schema_migration_v0_to_v1() {
    // Defined prior schema: Schema Version 0 is a flat key=value format without [entry:...] blocks
    std::string legacy_v0_text =
        "# Legacy CIPHER v0 Memory Format\n"
        "schema_version = 0\n"
        "user_alias = Steve\n"
        "preferred_mode = offline\n";

    cipher::MemoryStore store;
    auto res = store.load_from_string(legacy_v0_text);
    check(res.is_ok(), "migration: v0 load succeeded", "Failed to load legacy v0 format");
    check(store.schema_version() == 1, "migration: auto-migrated to schema version 1",
          "Expected schema version 1 after migration, got: " + std::to_string(store.schema_version()));
    check(store.size() == 2, "migration: 2 entries migrated", "Expected 2 entries");

    auto alias = store.get("user_alias");
    check(alias.has_value(), "migration: user_alias migrated", "user_alias missing");
    if (alias) {
        check(alias->content == "Steve", "migration: content preserved", "Content mismatch: " + alias->content);
        check(alias->category == "general", "migration: default category assigned", "Category mismatch: " + alias->category);
        check(alias->created_at > 0, "migration: timestamps generated", "Expected valid timestamp");
    }

    auto mode = store.get("preferred_mode");
    check(mode.has_value(), "migration: preferred_mode migrated", "preferred_mode missing");
    if (mode) {
        check(mode->content == "offline", "migration: mode content preserved", "Mode content mismatch");
    }

    // Verify serializing produces current Schema 1 format
    std::string migrated_serialized = store.serialize();
    check(migrated_serialized.find("schema_version = 1") != std::string::npos,
          "migration: serialized output is schema 1", "Serialized text did not contain schema 1");
    check(migrated_serialized.find("[entry:user_alias]") != std::string::npos,
          "migration: serialized output contains [entry:user_alias]", "Missing entry section header");
}

// ---------------------------------------------------------------------------
// 11. Failed writes preserving the last valid data (Atomic Recoverable Write)
// ---------------------------------------------------------------------------
void test_failed_writes_preserve_valid_data() {
    setup_temp();
    cipher::MemoryStore store;
    store.set("anchor_key", "Original valid uncorrupted text");

    auto save_res = store.save(TEMP_MEM_FILE);
    check(save_res.is_ok(), "atomic write: initial save ok", "Failed initial save");

    // Verify file exists on disk with original content
    {
        cipher::MemoryStore verify_store;
        verify_store.load(TEMP_MEM_FILE);
        auto entry = verify_store.get("anchor_key");
        check(entry.has_value() && entry->content == "Original valid uncorrupted text",
              "atomic write: verified on disk", "Disk verification failed");
    }

    // Now attempt to save to an impossible/invalid path (e.g. non-existent nested directory)
    std::string invalid_path = "test_memory_tmp_dir/non_existent_folder_abc/nested/fail.mem";
    cipher::MemoryStore mutated_store;
    mutated_store.set("anchor_key", "Mutated garbage that should fail");

    auto fail_res = mutated_store.save(invalid_path);
    check(!fail_res.is_ok(), "atomic write: invalid path save failed", "Expected save to invalid path to fail");

    // Verify original target file was NOT corrupted or modified
    {
        cipher::MemoryStore reload_store;
        auto reload_res = reload_store.load(TEMP_MEM_FILE);
        check(reload_res.is_ok(), "atomic write: original file still loads ok", "Original file unreadable");
        auto entry = reload_store.get("anchor_key");
        check(entry.has_value() && entry->content == "Original valid uncorrupted text",
              "atomic write: original data preserved exactly", "Original data was corrupted by failed write");
    }
    cleanup_temp();
}

// ---------------------------------------------------------------------------
// 12. Repeatable save/load operations
// ---------------------------------------------------------------------------
void test_repeatable_save_load() {
    setup_temp();
    cipher::MemoryStore store1;
    store1.set("k1", "Content 1", "cat1");
    store1.set("k2", "Content 2", "cat2");

    store1.save(TEMP_MEM_FILE);

    // Read back into store2
    cipher::MemoryStore store2;
    store2.load(TEMP_MEM_FILE);

    std::string serial1 = store1.serialize();
    std::string serial2 = store2.serialize();
    check(serial1 == serial2, "repeatable: serialization 1 == 2", "Serializations differ on first load");

    // Save store2 and load into store3
    store2.save(TEMP_MEM_FILE);
    cipher::MemoryStore store3;
    store3.load(TEMP_MEM_FILE);

    std::string serial3 = store3.serialize();
    check(serial2 == serial3, "repeatable: serialization 2 == 3", "Serializations differ on second load");
    cleanup_temp();
}

// ---------------------------------------------------------------------------
// 13. Replacement of existing file and locked failure preservation
// ---------------------------------------------------------------------------
void test_existing_file_replacement_and_lock_failure() {
    setup_temp();
    cipher::MemoryStore store_v1;
    store_v1.set("version_key", "version_1_data");
    auto res1 = store_v1.save(TEMP_MEM_FILE);
    check(res1.is_ok(), "replace existing: initial file write ok", "Failed initial write");

    // Replace existing file on disk with updated content
    cipher::MemoryStore store_v2;
    store_v2.set("version_key", "version_2_data_replaced");
    auto res2 = store_v2.save(TEMP_MEM_FILE);
    check(res2.is_ok(), "replace existing: overwrite existing destination succeeds", "Failed overwriting existing destination");

    cipher::MemoryStore verify_store;
    verify_store.load(TEMP_MEM_FILE);
    auto entry = verify_store.get("version_key");
    check(entry.has_value() && entry->content == "version_2_data_replaced",
          "replace existing: content successfully updated on disk", "Content not updated");

#if defined(_WIN32)
    // Test locked destination failure: open exclusively with CreateFileW
    std::wstring target_w = std::filesystem::path(TEMP_MEM_FILE).wstring();
    HANDLE hFile = CreateFileW(target_w.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        cipher::MemoryStore store_v3;
        store_v3.set("version_key", "version_3_should_fail");
        auto res3 = store_v3.save(TEMP_MEM_FILE);
        check(!res3.is_ok(), "locked destination: save correctly fails when file is locked", "Save should have failed on locked file");

        CloseHandle(hFile);

        // Verify valid version 2 content was NOT destroyed
        cipher::MemoryStore verify_after_lock;
        auto load_res = verify_after_lock.load(TEMP_MEM_FILE);
        check(load_res.is_ok(), "locked destination: file remains readable", "File unreadable after failed lock write");
        auto entry_after = verify_after_lock.get("version_key");
        check(entry_after.has_value() && entry_after->content == "version_2_data_replaced",
              "locked destination: previous valid content intact", "Content was corrupted by failed locked write");
    }
#endif
    cleanup_temp();
}

// ---------------------------------------------------------------------------
// 14. Whitespace, category, entry count, and overflow validation
// ---------------------------------------------------------------------------
void test_whitespace_category_and_entry_count_validation() {
    setup_temp();

    // 1. Lossless whitespace round-trip
    {
        cipher::MemoryStore store;
        std::string padded = "   leading and trailing spaces   ";
        std::string empty = "";
        std::string multiline_padded = "  line 1  \n  line 2  ";

        std::string err;
        check(store.set("padded_id", padded, "general", &err), "lossless whitespace: set padded", err);
        check(store.set("empty_id", empty, "general", &err), "lossless whitespace: set empty", err);
        check(store.set("multiline_id", multiline_padded, "general", &err), "lossless whitespace: set multiline", err);

        auto save_res = store.save(TEMP_MEM_FILE);
        check(save_res.is_ok(), "lossless whitespace: save succeeds", "Save failed");

        cipher::MemoryStore loaded;
        auto load_res = loaded.load(TEMP_MEM_FILE);
        check(load_res.is_ok(), "lossless whitespace: load succeeds", "Load failed");

        auto p = loaded.get("padded_id");
        check(p.has_value() && p->content == padded,
              "lossless whitespace: padded content strictly preserved",
              "Expected '" + padded + "', got '" + (p ? p->content : "null") + "'");

        auto e = loaded.get("empty_id");
        check(e.has_value() && e->content == empty,
              "lossless whitespace: empty content strictly preserved",
              "Expected empty content");

        auto m = loaded.get("multiline_id");
        check(m.has_value() && m->content == multiline_padded,
              "lossless whitespace: multiline padded content strictly preserved",
              "Expected '" + multiline_padded + "'");
    }

    // 2. Category validation
    {
        cipher::MemoryStore store;
        std::string err;

        // Valid custom category
        check(store.set("cat_ok", "content", "custom.category_1", &err),
              "category validation: valid custom category accepted", "Failed: " + err);

        // Control characters in category
        check(!store.set("cat_newline", "content", "bad\ncategory", &err),
              "category validation: newline in category rejected", "Newline category should fail");
        check(!store.set("cat_return", "content", "bad\rcategory", &err),
              "category validation: carriage return in category rejected", "CR category should fail");
        check(!store.set("cat_tab", "content", "bad\tcategory", &err),
              "category validation: tab in category rejected", "Tab category should fail");

        // Leading/trailing whitespace in category
        check(!store.set("cat_lead_space", "content", " leading", &err),
              "category validation: leading space in category rejected", "Leading space should fail");
        check(!store.set("cat_trail_space", "content", "trailing ", &err),
              "category validation: trailing space in category rejected", "Trailing space should fail");
    }

    // 3. Entry count mismatch in storage file
    {
        std::string mismatched =
            "# CIPHER Persistent Memory Store\n"
            "schema_version = 1\n"
            "entry_count = 5\n\n"
            "[entry:test_1]\n"
            "category = general\n"
            "content = only_one_entry\n";

        cipher::MemoryStore store;
        auto res = store.load_from_string(mismatched);
        check(!res.is_ok(), "entry count validation: mismatched entry count rejected", "Expected failure on count mismatch");
        bool found_msg = false;
        for (const auto& error : res.errors) {
            if (error.message.find("Mismatched entry count") != std::string::npos) {
                found_msg = true;
                break;
            }
        }
        check(found_msg, "entry count validation: error message mentions mismatched entry count", "Missing expected error msg");
    }

    // 4. Schema version overflow
    {
        std::string overflow =
            "# CIPHER Persistent Memory Store\n"
            "schema_version = 999999999999999999999999999\n"
            "entry_count = 0\n";

        cipher::MemoryStore store;
        auto res = store.load_from_string(overflow);
        check(!res.is_ok(), "schema version overflow: stoi overflow rejected gracefully without exception", "Expected rejection");
    }

    cleanup_temp();
}

// ---------------------------------------------------------------------------
// 15. Filesystem inspection failure is not a missing file (POSIX only).
//     A directory with no permissions makes exists() fail with EACCES.
//     Windows ACL denial is not reproduced here (no portable failure hook).
// ---------------------------------------------------------------------------
#if !defined(_WIN32)
void test_inspection_failure_is_not_missing_file() {
    const std::string locked_dir = TEMP_TEST_DIR + "/locked";
    const std::string locked_file = locked_dir + "/cipher_memory.v1";

    setup_temp();
    std::error_code ec;
    std::filesystem::create_directories(locked_dir, ec);

    cipher::MemoryStore seed;
    seed.set("seed_item", "Persisted", "general");
    seed.save(locked_file);
    std::filesystem::permissions(locked_dir, std::filesystem::perms::none, ec);

    std::error_code probe_ec;
    static_cast<void>(std::filesystem::exists(locked_file, probe_ec));
    if (!probe_ec) {
        std::cout << "SKIP: test_inspection_failure_is_not_missing_file (permission denial not enforced, e.g. running as root)\n";
        std::filesystem::permissions(locked_dir, std::filesystem::perms::owner_all, ec);
        cleanup_temp();
        return;
    }

    cipher::MemoryStore store;
    store.set("in_memory_item", "Must survive", "general");
    auto res = store.load(locked_file);
    check(!res.ok, "load reports error when filesystem inspection fails",
          "load treated an inspection failure as a missing file");
    check(store.has("in_memory_item"), "failed load does not clear in-memory store",
          "failed load silently replaced the store with an empty one");

    std::filesystem::permissions(locked_dir, std::filesystem::perms::owner_all, ec);
    cleanup_temp();
}
#endif

} // anonymous namespace

int main() {
    std::cout << "Running CIPHER Memory Subsystem Test Suite...\n\n";

    test_missing_storage_file();
    test_empty_storage();
    test_save_and_load_round_trip();
    test_multiple_entries_and_update();
    test_delete_behavior();
    test_unicode_and_special_character_preservation();
    test_invalid_input_and_duplicates();
    test_corrupted_storage();
    test_unsupported_schema_versions();
    test_schema_migration_v0_to_v1();
    test_failed_writes_preserve_valid_data();
    test_repeatable_save_load();
    test_existing_file_replacement_and_lock_failure();
    test_whitespace_category_and_entry_count_validation();
#if !defined(_WIN32)
    test_inspection_failure_is_not_missing_file();
#endif

    std::cout << "\n-------------------------------------------------------------\n";
    std::cout << "Tests passed: " << g_passes << '\n';
    std::cout << "Tests failed: " << g_failures << '\n';
    std::cout << "-------------------------------------------------------------\n";

    return (g_failures == 0) ? 0 : 1;
}
