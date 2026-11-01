// Tests for the Stage 5 GitHub adapter, governed merge gates and merge lock, and the Stage 8 event log.
// Every GitHub call goes through a fake runner. No process, network or GitHub access happens here.

#include "cipher/orchestration/event_log.hpp"
#include "cipher/orchestration/github_adapter.hpp"
#include "cipher/orchestration/governed_merge.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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

const std::string kHeadSha = "0123456789abcdef0123456789abcdef01234567";

// Records every argv it is asked to run and returns a scripted result.
class FakeGhRunner final : public GhRunner {
public:
    GhResult run(const std::vector<std::string>& argv) override {
        calls.push_back(argv);
        return next;
    }
    std::vector<std::vector<std::string>> calls;
    GhResult next;
};

bool contains_arg(const std::vector<std::string>& argv, const std::string& value) {
    for (const auto& arg : argv) {
        if (arg == value) return true;
    }
    return false;
}

void test_argv_is_fixed_per_operation() {
    const auto read = build_gh_argv(GhOperation::ReadPullRequest, 42, "");
    check(read.size() >= 3 && read[0] == "gh" && read[1] == "pr" && read[2] == "view", "argv: read PR uses gh pr view");
    check(contains_arg(read, "42"), "argv: read PR carries the number as its own argument");

    const auto merge = build_gh_argv(GhOperation::MergePullRequest, 42, kHeadSha);
    check(contains_arg(merge, "--squash"), "argv: merge is squash");
    check(contains_arg(merge, "--match-head-commit"), "argv: merge pins the head with --match-head-commit");
    check(contains_arg(merge, kHeadSha), "argv: merge passes the gated head SHA as its own argument");
    check(!contains_arg(merge, "--admin") && !contains_arg(merge, "--auto"), "argv: merge never uses admin or auto flags");
}

void test_argv_rejects_bad_requests() {
    bool rejected_number = false;
    try {
        build_gh_argv(GhOperation::ReadIssue, 0, "");
    } catch (const GhRequestError&) {
        rejected_number = true;
    }
    check(rejected_number, "argv: zero number is rejected");

    bool rejected_sha = false;
    try {
        build_gh_argv(GhOperation::MergePullRequest, 42, "HEAD");
    } catch (const GhRequestError&) {
        rejected_sha = true;
    }
    check(rejected_sha, "argv: merge without a full SHA is rejected (no symbolic refs)");

    bool rejected_upper = false;
    try {
        build_gh_argv(GhOperation::MergePullRequest, 42, "0123456789ABCDEF0123456789ABCDEF01234567");
    } catch (const GhRequestError&) {
        rejected_upper = true;
    }
    check(rejected_upper, "argv: uppercase SHA is rejected");

    bool rejected_flag = false;
    try {
        build_gh_argv(GhOperation::CommentOnPullRequest, 42, "--repo=evil");
    } catch (const GhRequestError&) {
        rejected_flag = true;
    }
    check(rejected_flag, "argv: a body path that looks like a flag is rejected");
}

void test_parse_pull_request_line() {
    const PullRequestFacts pr = parse_pull_request_line("42\tOPEN\t" + kHeadSha + "\tbase1\tMERGEABLE");
    check(pr.number == 42 && pr.open && !pr.merged, "parse: open PR fields");
    check(pr.head_sha == kHeadSha && pr.mergeable, "parse: head SHA and mergeable flag");

    const PullRequestFacts conflicting = parse_pull_request_line("43\tOPEN\t" + kHeadSha + "\tbase1\tCONFLICTING");
    check(!conflicting.mergeable, "parse: CONFLICTING is not mergeable");

    const PullRequestFacts merged = parse_pull_request_line("44\tMERGED\t" + kHeadSha + "\tbase1\tUNKNOWN");
    check(merged.merged && !merged.open, "parse: MERGED is merged, not open");

    bool threw = false;
    try {
        parse_pull_request_line("42\tOPEN\tonly-three");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "parse: malformed output throws instead of guessing");
}

void test_read_failure_reports_exit_not_output() {
    FakeGhRunner runner;
    runner.next = {1, "token ghp_should_not_appear"};
    GhAdapter adapter(runner);
    std::string message;
    try {
        adapter.read_pull_request(42);
    } catch (const std::runtime_error& error) {
        message = error.what();
    }
    check(message.find("exit 1") != std::string::npos, "read: a failed read names the exit code");
    check(message.find("ghp_") == std::string::npos, "read: gh output is not echoed into the error");
}

void test_merge_returns_exit_success() {
    FakeGhRunner runner;
    GhAdapter adapter(runner);
    runner.next = {0, ""};
    check(adapter.merge_pull_request(42, kHeadSha), "merge: exit 0 reports success");
    runner.next = {1, ""};
    check(!adapter.merge_pull_request(42, kHeadSha), "merge: nonzero exit reports failure");
    check(runner.calls.size() == 2 && contains_arg(runner.calls[0], "--match-head-commit"),
          "merge: each call is the pinned merge argv");
}

// Each gate is flipped alone from a fully passing baseline, so each test names exactly one failing condition.
MergeGateInputs passing() {
    MergeGateInputs in;
    in.pr_open = true;
    in.head_matches_gated_sha = true;
    in.mergeable = true;
    in.ci_all_required_passed = true;
    in.qa_approved_or_not_applicable = true;
    in.reviewer_approved = true;
    in.security_not_applicable_or_approved = true;
    in.no_open_blocker_or_high = true;
    in.scope_accepted = true;
    in.no_protected_content = true;
    return in;
}

void test_gates_pass_when_all_true() {
    const MergeDecision decision = evaluate_merge_gates(passing());
    check(decision.allowed && decision.block == MergeBlock::None && !decision.needs_human, "gates: all true is allowed");
}

void test_each_gate_blocks_alone() {
    MergeGateInputs in = passing();
    in.pr_open = false;
    check(evaluate_merge_gates(in).block == MergeBlock::PrNotOpen, "gate: PR not open blocks");

    in = passing();
    in.head_matches_gated_sha = false;
    check(evaluate_merge_gates(in).block == MergeBlock::HeadChanged, "gate: changed head blocks and re-gates");

    in = passing();
    in.mergeable = false;
    check(evaluate_merge_gates(in).block == MergeBlock::NotMergeable, "gate: conflicting PR blocks");

    in = passing();
    in.ci_all_required_passed = false;
    check(evaluate_merge_gates(in).block == MergeBlock::CiNotPassed, "gate 1: required CI must pass");

    in = passing();
    in.qa_approved_or_not_applicable = false;
    check(evaluate_merge_gates(in).block == MergeBlock::QaNotApproved, "gate 2: QA must approve or be not applicable");

    in = passing();
    in.reviewer_approved = false;
    check(evaluate_merge_gates(in).block == MergeBlock::ReviewerNotApproved, "gate 3: reviewer must approve");

    in = passing();
    in.security_not_applicable_or_approved = false;
    const MergeDecision security = evaluate_merge_gates(in);
    check(security.block == MergeBlock::SecurityNotApproved && security.needs_human,
          "gate 4: missing independent security approval escalates to NEEDS_HUMAN");

    in = passing();
    in.no_open_blocker_or_high = false;
    check(evaluate_merge_gates(in).block == MergeBlock::BlockerOrHighOpen, "gate 5: open BLOCKER or HIGH blocks");

    in = passing();
    in.scope_accepted = false;
    const MergeDecision scope = evaluate_merge_gates(in);
    check(scope.block == MergeBlock::ScopeNotAccepted && scope.needs_human, "gate 6: scope expansion escalates");

    in = passing();
    in.no_protected_content = false;
    const MergeDecision protected_content = evaluate_merge_gates(in);
    check(protected_content.block == MergeBlock::ProtectedContent && protected_content.needs_human,
          "gate 7: protected content escalates");
}

void test_merge_lock_serializes_and_releases_only_for_holder() {
    const fs::path dir = fs::temp_directory_path() / "cipher-orch-merge-lock-test";
    fs::remove_all(dir);
    MergeLock lock(dir);

    check(lock.try_acquire(1), "lock: first job acquires");
    check(!lock.try_acquire(2), "lock: a second job is refused while held");
    check(lock.holder() == 1, "lock: the holder is recorded");

    lock.release(2);
    check(lock.holder() == 1, "lock: a non-holder cannot release");

    lock.release(1);
    check(!lock.holder().has_value(), "lock: the holder releases its own lock");
    check(lock.try_acquire(2), "lock: after release the next job acquires");
    fs::remove_all(dir);
}

void test_event_log_appends_and_rejects_control_characters() {
    const fs::path file = fs::temp_directory_path() / "cipher-orch-events-test" / "events.log";
    fs::remove_all(file.parent_path());
    EventLog log(file);
    log.append(5, "merge_blocked", "gate 4");
    log.append(5, "merge_done", "squash");
    const auto lines = log.read_all();
    check(lines.size() == 2, "events: each append adds one line");
    check(lines[0] == "issue=5\tevent=merge_blocked\tdetail=gate 4", "events: line format is issue, event, detail");

    bool threw = false;
    try {
        log.append(5, "bad", "two\nlines");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "events: a newline in detail is refused (cannot forge a second line)");
    check(log.read_all().size() == 2, "events: the refused append wrote nothing");
    fs::remove_all(file.parent_path());
}

}  // namespace

int main() {
    test_argv_is_fixed_per_operation();
    test_argv_rejects_bad_requests();
    test_parse_pull_request_line();
    test_read_failure_reports_exit_not_output();
    test_merge_returns_exit_success();
    test_gates_pass_when_all_true();
    test_each_gate_blocks_alone();
    test_merge_lock_serializes_and_releases_only_for_holder();
    test_event_log_appends_and_rejects_control_characters();

    std::cout << g_passes << " passed, " << g_failures << " failed\n";
    return g_failures == 0 ? 0 : 1;
}
