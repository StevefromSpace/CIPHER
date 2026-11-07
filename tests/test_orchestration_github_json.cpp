// Tests for the strict JSON GitHub read backend. Uses the sanitized PR #49 fixture; no live GitHub access.

#include "cipher/orchestration/github_adapter.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef CIPHER_FIXTURE_DIR
#error "CIPHER_FIXTURE_DIR must be defined by the build"
#endif

namespace {

using namespace cipher::orchestration;

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

template <typename F>
bool throws(F&& f) {
    try {
        f();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

const std::string kHead = "0123456789abcdef0123456789abcdef01234567";
const std::string kBase = "abcdef0123456789abcdef0123456789abcdef01";

std::string fixture() {
    std::ifstream in(std::string(CIPHER_FIXTURE_DIR) + "/github_pr49.json", std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string pr_json(const std::string& state = "OPEN", const std::string& mergeable = "MERGEABLE",
                    const std::string& rollup = "[]", const std::string& reviews = "[]",
                    const std::string& head = kHead) {
    return "{\"number\":7,\"state\":\"" + state + "\",\"headRefOid\":\"" + head + "\",\"baseRefOid\":\"" + kBase +
           "\",\"mergeable\":\"" + mergeable + "\",\"statusCheckRollup\":" + rollup + ",\"reviews\":" + reviews + "}";
}

std::string run(const std::string& status, const std::string& conclusion) {
    return "{\"__typename\":\"CheckRun\",\"name\":\"x\",\"status\":\"" + status + "\",\"conclusion\":\"" + conclusion +
           "\"}";
}

std::string review(const std::string& author, const std::string& state, const std::string& assoc,
                   const std::string& oid = kHead) {
    return "{\"author\":{\"login\":\"" + author + "\"},\"authorAssociation\":\"" + assoc + "\",\"state\":\"" + state +
           "\",\"commit\":{\"oid\":\"" + oid + "\"}}";
}

class FakeRunner final : public GhRunner {
public:
    GhResult run(const std::vector<std::string>& argv) override {
        calls.push_back(argv);
        return next;
    }
    GhResult next;
    std::vector<std::vector<std::string>> calls;
};

void test_fixture() {
    const PullRequestFacts pr = parse_pull_request_json(fixture());
    check(pr.number == 49, "fixture: number");
    check(pr.merged && !pr.open, "fixture: merged, not open");
    check(pr.head_sha == "c6b9a9c3796a4b2b2bbd2b386e98760f3514d183", "fixture: head sha");
    check(pr.base_sha == "557e77d25a9781925b2aa96491e2ffc2d2d5b967", "fixture: base sha");
    check(!pr.mergeable, "fixture: UNKNOWN mergeable is not mergeable");
    check(pr.checks == CheckState::Passed, "fixture: all five checks passed");
    check(pr.review == ReviewState::None, "fixture: no reviews");
}

void test_checks() {
    auto checks = [](const std::string& rollup) { return parse_pull_request_json(pr_json("OPEN", "MERGEABLE", rollup)).checks; };
    check(checks("[]") == CheckState::Pending, "checks: empty rollup is pending");
    check(checks("[" + run("COMPLETED", "SUCCESS") + "," + run("COMPLETED", "SKIPPED") + "]") == CheckState::Passed, "checks: success+skipped passed");
    check(checks("[" + run("COMPLETED", "SUCCESS") + "," + run("IN_PROGRESS", "") + "]") == CheckState::Pending, "checks: running is pending");
    check(checks("[" + run("IN_PROGRESS", "") + "," + run("COMPLETED", "FAILURE") + "]") == CheckState::Failed, "checks: failure beats pending");
    check(checks("[" + run("COMPLETED", "CANCELLED") + "]") == CheckState::Failed, "checks: cancelled failed");
    check(checks("[{\"__typename\":\"StatusContext\",\"state\":\"ERROR\"}]") == CheckState::Failed, "checks: status context error failed");
    check(checks("[{\"__typename\":\"StatusContext\",\"state\":\"SUCCESS\"}]") == CheckState::Passed, "checks: status context success");
    check(throws([&] { checks("[" + run("COMPLETED", "MYSTERY") + "]"); }), "checks: unknown conclusion rejected");
    check(throws([&] { checks("[{\"__typename\":\"Other\"}]"); }), "checks: unknown type rejected");
    check(throws([&] { checks("{}"); }), "checks: non-array rejected");
}

void test_reviews() {
    auto reviews = [](const std::string& list) { return parse_pull_request_json(pr_json("OPEN", "MERGEABLE", "[]", list)); };
    PullRequestFacts a = reviews("[" + review("alice", "APPROVED", "OWNER") + "]");
    check(a.review == ReviewState::Approved && a.reviewed_head_sha == kHead, "reviews: owner approval names head");
    check(reviews("[" + review("mallory", "APPROVED", "NONE") + "]").review == ReviewState::None, "reviews: untrusted association ignored");
    check(reviews("[" + review("alice", "APPROVED", "OWNER") + "," + review("bob", "CHANGES_REQUESTED", "MEMBER") + "]").review == ReviewState::ChangesRequested, "reviews: changes requested wins");
    check(reviews("[" + review("bob", "CHANGES_REQUESTED", "MEMBER") + "," + review("bob", "APPROVED", "MEMBER") + "]").review == ReviewState::Approved, "reviews: later review supersedes");
    check(reviews("[" + review("bob", "APPROVED", "MEMBER") + "," + review("bob", "DISMISSED", "MEMBER") + "]").review == ReviewState::None, "reviews: dismissed clears approval");
    check(reviews("[" + review("bob", "COMMENTED", "MEMBER") + "]").review == ReviewState::None, "reviews: comment is not a verdict");
    check(throws([&] { reviews("[" + review("alice", "APPROVED", "OWNER", "abc123") + "]"); }), "reviews: approval needs full commit sha");
}

void test_rejections() {
    check(throws([] { parse_pull_request_json(""); }), "reject: empty");
    check(throws([] { parse_pull_request_json("{not json"); }), "reject: malformed");
    check(throws([] { parse_pull_request_json("[]"); }), "reject: non-object");
    check(throws([] { parse_pull_request_json(pr_json("OPEN", "MERGEABLE", "[]", "[]", "abc123")); }), "reject: short sha");
    check(throws([] { parse_pull_request_json(pr_json("OPEN", "MERGEABLE", "[]", "[]", "0123456789ABCDEF0123456789ABCDEF01234567")); }), "reject: uppercase sha");
    check(throws([] { parse_pull_request_json(pr_json("WEIRD")); }), "reject: unknown state");
    check(throws([] { parse_pull_request_json(pr_json("OPEN", "MAYBE")); }), "reject: unknown mergeable");
    check(throws([] { parse_pull_request_json("{\"number\":7}"); }), "reject: missing fields");
    check(throws([] { parse_pull_request_json("{\"number\":\"7\",\"state\":\"OPEN\"}"); }), "reject: number is a string");
    check(throws([] { parse_pull_request_json("{\"number\":0,\"state\":\"OPEN\"}"); }), "reject: zero number");
    check(parse_pull_request_json(pr_json("CLOSED", "CONFLICTING")).open == false, "accept: closed conflicting");
}

void test_adapter() {
    FakeRunner ok;
    ok.next = {0, fixture()};
    GhAdapter adapter(ok);
    check(adapter.read_pull_request_detail(49).head_sha == "c6b9a9c3796a4b2b2bbd2b386e98760f3514d183", "adapter: parses fixture");
    check(ok.calls.size() == 1 && ok.calls[0][1] == "pr" && ok.calls[0][2] == "view" && ok.calls[0][3] == "49", "adapter: one read argv");
    for (const std::string& token : ok.calls[0]) {
        check(token != "comment" && token != "merge" && token != "edit", "adapter: read argv has no mutating verb");
    }
    check(throws([&] { adapter.read_pull_request_detail(50); }), "adapter: number mismatch rejected");
    FakeRunner failing;
    failing.next = {1, ""};
    GhAdapter bad(failing);
    check(throws([&] { bad.read_pull_request_detail(49); }), "adapter: command failure fails closed");
    FakeRunner garbage;
    garbage.next = {0, "oops"};
    GhAdapter g(garbage);
    check(throws([&] { g.read_pull_request_detail(49); }), "adapter: malformed output fails closed");
    check(throws([&] { build_gh_argv(GhOperation::ReadPullRequestJson, 0, ""); }), "adapter: bad number rejected");
}

}  // namespace

int main() {
    test_fixture();
    test_checks();
    test_reviews();
    test_rejections();
    test_adapter();
    std::cout << g_passes << " passed, " << g_failures << " failed\n";
    return g_failures == 0 ? 0 : 1;
}
