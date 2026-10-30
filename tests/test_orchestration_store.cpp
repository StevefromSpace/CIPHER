// Tests for the Stage 8 file-backed JobStore: round trip, reload after restart, corrupt and interrupted
// writes, schema refusal, and an orchestrator resuming from disk. Uses a scratch directory under the
// system temp path only. No network, model or GitHub dependency.

#include "cipher/orchestration/job_store_file.hpp"
#include "cipher/orchestration/orchestrator.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace cipher::orchestration;
namespace fs = std::filesystem;

int g_failures = 0;
int g_passes = 0;

void check(bool condition, const std::string& name) {
    if (condition) {
        ++g_passes;
        std::cout << "PASS: " << name << "\n";
    } else {
        ++g_failures;
        std::cout << "FAIL: " << name << "\n";
    }
}

// Throws if it is ever asked to run a role: the store tests must not execute work.
class NoRoleExecutor final : public RoleExecutor {
public:
    RoleResult run(Role, const JobRecord&) override { throw std::logic_error("no role may run in store tests"); }
};

JobRecord sample_job() {
    JobRecord job;
    job.state = State::Review;
    job.resume_state = State::Ci;
    job.escalation = EscalationReason::BlockedRepeated;
    job.implementation_executions = 2;
    job.review_rounds = 1;
    job.blocked_count = 1;
    job.reevaluation_used = true;
    job.reevaluation_pending = false;
    job.pull_request_open = true;
    job.qa_approved = true;
    job.ci_passed = true;
    job.review_approved = false;
    job.head_sha = "0123456789abcdef0123456789abcdef01234567";
    job.reviewed_head_sha = "0123456789abcdef0123456789abcdef01234567";
    job.implementation_family = "claude";
    job.execution_in_flight = false;
    job.facts.research = true;
    job.facts.needs_design = false;
    job.facts.security_sensitive = true;
    return job;
}

bool same_job(const JobRecord& a, const JobRecord& b) { return serialize_job(a) == serialize_job(b); }

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

fs::path fresh_directory(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / ("cipher-orch-store-" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void test_round_trip_and_reload() {
    const fs::path dir = fresh_directory("round-trip");
    const JobRecord job = sample_job();
    {
        FileJobStore store(dir);
        store.save(7, job);
    }
    // A new store object stands in for a process restart.
    FileJobStore reopened(dir);
    const auto loaded = reopened.load(7);
    check(loaded.has_value(), "store: saved job is found after reopening");
    check(loaded && same_job(*loaded, job), "store: every field survives save and reload");
    check(loaded && loaded->implementation_family == "claude", "store: implementation family is persisted");
    check(loaded && loaded->implementation_executions == 2, "store: implementation counter is preserved");
    check(!reopened.load(8).has_value(), "store: missing issue loads as nothing, not as an error");
}

void test_rename_replaces_existing_file() {
    const fs::path dir = fresh_directory("rename");
    FileJobStore store(dir);
    JobRecord first = sample_job();
    store.save(3, first);
    JobRecord second = sample_job();
    second.implementation_executions = 1;
    second.head_sha = "fedcba9876543210fedcba9876543210fedcba98";
    store.save(3, second);
    const auto loaded = store.load(3);
    check(loaded && same_job(*loaded, second), "rename: second save replaces the existing target on Windows");
    check(!fs::exists(store.path_for(3).string() + ".tmp"), "rename: no temp file is left after a save");
}

void test_corrupt_file_throws_and_is_kept() {
    const fs::path dir = fresh_directory("corrupt");
    FileJobStore store(dir);
    write_text(store.path_for(4), "this is not a job record\n");
    bool threw = false;
    try {
        store.load(4);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "corrupt: unparseable record throws instead of loading as missing");
    check(fs::exists(store.path_for(4)), "corrupt: the damaged file is kept for the owner to inspect");
}

void test_wrong_schema_is_refused() {
    const fs::path dir = fresh_directory("schema");
    FileJobStore store(dir);
    store.save(5, sample_job());
    std::string text = serialize_job(sample_job());
    text.replace(text.find("schema=1"), 8, "schema=0");
    write_text(store.path_for(5), text);
    bool threw = false;
    try {
        store.load(5);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "schema: an older schema version is refused, not upgraded silently");
}

void test_missing_duplicate_and_unknown_keys_throw() {
    const std::string good = serialize_job(sample_job());

    std::string missing = good;
    missing.erase(missing.find("review_rounds="), std::string("review_rounds=1\n").size());
    bool threw_missing = false;
    try {
        parse_job(missing);
    } catch (const std::runtime_error&) {
        threw_missing = true;
    }
    check(threw_missing, "parse: a record missing a key is refused");

    bool threw_duplicate = false;
    try {
        parse_job(good + "state=0\n");
    } catch (const std::runtime_error&) {
        threw_duplicate = true;
    }
    check(threw_duplicate, "parse: a duplicate key is refused");

    bool threw_unknown = false;
    try {
        parse_job(good + "surprise=1\n");
    } catch (const std::runtime_error&) {
        threw_unknown = true;
    }
    check(threw_unknown, "parse: an unknown key is refused");
}

void test_out_of_range_enum_throws() {
    std::string text = serialize_job(sample_job());
    text.replace(text.find("\nstate=7\n") + 1, std::string("state=7").size(), "state=99");
    bool threw = false;
    try {
        parse_job(text);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "parse: an out-of-range state value is refused");
}

void test_interrupted_write_keeps_previous_record() {
    const fs::path dir = fresh_directory("interrupted");
    FileJobStore store(dir);
    const JobRecord good = sample_job();
    store.save(6, good);
    // Simulate a crash after the temp file was partly written but before the rename.
    write_text(store.path_for(6).string() + ".tmp", "schema=1\nstate=");
    const auto loaded = store.load(6);
    check(loaded && same_job(*loaded, good), "interrupted: the previous valid record is still loaded");

    // The next save overwrites the stale temp file and completes normally.
    JobRecord next = sample_job();
    next.review_rounds = 2;
    store.save(6, next);
    const auto after = store.load(6);
    check(after && same_job(*after, next), "interrupted: the next save completes over the stale temp file");
}

void test_newline_in_value_is_refused_and_keeps_file() {
    const fs::path dir = fresh_directory("newline");
    FileJobStore store(dir);
    const JobRecord good = sample_job();
    store.save(9, good);
    JobRecord bad = sample_job();
    bad.head_sha = "abc\nstate=0";
    bool threw = false;
    try {
        store.save(9, bad);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "newline: a value that would forge a key is refused");
    const auto loaded = store.load(9);
    check(loaded && same_job(*loaded, good), "newline: the previous record is untouched after a refused save");
}

void test_orchestrator_resumes_from_disk() {
    const fs::path dir = fresh_directory("resume");
    NoRoleExecutor executor;
    IssueFacts facts;
    {
        FileJobStore store(dir);
        Orchestrator first(11, facts, executor, store, Policy{});
        check(first.start(), "resume: start applies once and is persisted");
    }
    FileJobStore reopened(dir);
    Orchestrator second(11, facts, executor, reopened, Policy{});
    check(second.job().state == State::Implementation, "resume: a restarted orchestrator reloads the same state");
    check(second.job().implementation_executions == 1, "resume: the reloaded counter is not reset to zero");
    check(!second.recover(), "resume: nothing is in flight, so recovery does nothing");
    check(second.job().implementation_executions == 1, "resume: recovery with nothing in flight leaves the counter alone");
}

}  // namespace

int main() {
    test_round_trip_and_reload();
    test_rename_replaces_existing_file();
    test_corrupt_file_throws_and_is_kept();
    test_wrong_schema_is_refused();
    test_missing_duplicate_and_unknown_keys_throw();
    test_out_of_range_enum_throws();
    test_interrupted_write_keeps_previous_record();
    test_newline_in_value_is_refused_and_keeps_file();
    test_orchestrator_resumes_from_disk();

    std::cout << g_passes << " passed, " << g_failures << " failed\n";
    return g_failures == 0 ? 0 : 1;
}
