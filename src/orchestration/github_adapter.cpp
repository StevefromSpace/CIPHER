#include "cipher/orchestration/github_adapter.hpp"
#include "cipher/orchestration/commit_sha.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace cipher::orchestration {
namespace {

void require_number(int number) {
    if (number <= 0) throw GhRequestError("GitHub number must be positive");
}

// argv entries are not shell-parsed, but a leading '-' would still be read as a gh flag.
bool is_safe_path_argument(const std::string& path) {
    return !path.empty() && path.front() != '-' && path.find('\n') == std::string::npos &&
           path.find('\r') == std::string::npos;
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    for (const char c : line) {
        if (c == '\t') {
            fields.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    fields.push_back(current);
    return fields;
}

int parse_positive_int(const std::string& text) {
    if (text.empty() || text.size() > 9) throw std::runtime_error("GitHub output: bad number field");
    int value = 0;
    for (const char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) throw std::runtime_error("GitHub output: bad number field");
        value = value * 10 + (c - '0');
    }
    if (value <= 0) throw std::runtime_error("GitHub output: bad number field");
    return value;
}

}  // namespace

std::vector<std::string> build_gh_argv(GhOperation operation, int number, const std::string& argument) {
    require_number(number);
    const std::string number_text = std::to_string(number);
    switch (operation) {
        case GhOperation::ReadPullRequest:
            return {"gh", "pr", "view", number_text, "--json", "number,state,headRefOid,baseRefOid,mergeable",
                    "--jq", "[.number,.state,.headRefOid,.baseRefOid,.mergeable]|@tsv"};
        case GhOperation::ReadPullRequestJson:
            return {"gh", "pr", "view", number_text, "--json",
                    "number,state,headRefOid,baseRefOid,mergeable,statusCheckRollup,reviews"};
        case GhOperation::ReadIssue:
            return {"gh", "issue", "view", number_text, "--json", "number,state,labels",
                    "--jq", "[.number,.state,(.labels|map(.name)|join(\",\"))]|@tsv"};
        case GhOperation::CommentOnPullRequest:
            if (!is_safe_path_argument(argument)) throw GhRequestError("comment body file path is unsafe");
            return {"gh", "pr", "comment", number_text, "--body-file", argument};
        case GhOperation::MergePullRequest:
            if (!is_full_sha(argument)) throw GhRequestError("merge requires a full lowercase head SHA");
            return {"gh", "pr", "merge", number_text, "--squash", "--match-head-commit", argument};
    }
    throw GhRequestError("unknown GitHub operation");
}

PullRequestFacts parse_pull_request_line(const std::string& line) {
    const std::vector<std::string> fields = split_tabs(line);
    if (fields.size() != 5) throw std::runtime_error("GitHub output: expected 5 pull request fields");
    PullRequestFacts pr;
    pr.number = parse_positive_int(fields[0]);
    pr.open = fields[1] == "OPEN";
    pr.merged = fields[1] == "MERGED";
    pr.head_sha = fields[2];
    pr.base_sha = fields[3];
    pr.mergeable = fields[4] == "MERGEABLE";
    return pr;
}

namespace {

using nlohmann::json;

[[noreturn]] void bad(const char* what) { throw std::runtime_error(std::string("GitHub JSON: ") + what); }

const json& field(const json& object, const char* key) {
    if (!object.is_object()) bad("expected object");
    const auto it = object.find(key);
    if (it == object.end()) bad("missing field");
    return *it;
}

std::string string_field(const json& object, const char* key) {
    const json& value = field(object, key);
    if (!value.is_string()) bad("field is not a string");
    return value.get<std::string>();
}

std::string sha_field(const json& object, const char* key) {
    std::string sha = string_field(object, key);
    if (!is_full_sha(sha)) bad("not a full lowercase SHA");
    return sha;
}

// Failure beats pending beats success, so one bad check can never be hidden by passing ones.
CheckState check_state(const json& rollup) {
    if (!rollup.is_array()) bad("statusCheckRollup is not an array");
    if (rollup.empty()) return CheckState::Pending;
    bool failed = false;
    bool pending = false;
    for (const json& entry : rollup) {
        const std::string type = string_field(entry, "__typename");
        if (type == "CheckRun") {
            if (string_field(entry, "status") != "COMPLETED") {
                pending = true;
                continue;
            }
            const std::string outcome = string_field(entry, "conclusion");
            if (outcome == "SUCCESS" || outcome == "NEUTRAL" || outcome == "SKIPPED") continue;
            if (outcome == "FAILURE" || outcome == "TIMED_OUT" || outcome == "CANCELLED" ||
                outcome == "ACTION_REQUIRED" || outcome == "STARTUP_FAILURE" || outcome == "STALE") {
                failed = true;
                continue;
            }
        } else if (type == "StatusContext") {
            const std::string outcome = string_field(entry, "state");
            if (outcome == "SUCCESS") continue;
            if (outcome == "PENDING" || outcome == "EXPECTED") {
                pending = true;
                continue;
            }
            if (outcome == "FAILURE" || outcome == "ERROR") {
                failed = true;
                continue;
            }
        }
        bad("unknown check type or outcome");
    }
    if (failed) return CheckState::Failed;
    return pending ? CheckState::Pending : CheckState::Passed;
}

bool is_trusted_association(const std::string& association) {
    return association == "OWNER" || association == "MEMBER" || association == "COLLABORATOR";
}

// Only repository-trusted reviewers count; the latest decisive review per author wins.
void apply_reviews(const json& reviews, PullRequestFacts& pr) {
    if (!reviews.is_array()) bad("reviews is not an array");
    std::vector<std::string> authors;
    std::vector<std::string> states;
    std::vector<std::string> heads;
    for (const json& review : reviews) {
        if (!is_trusted_association(string_field(review, "authorAssociation"))) continue;
        const std::string state = string_field(review, "state");
        if (state != "APPROVED" && state != "CHANGES_REQUESTED" && state != "DISMISSED") continue;
        const std::string author = string_field(field(review, "author"), "login");
        std::string head;
        if (state == "APPROVED") head = sha_field(field(review, "commit"), "oid");
        std::size_t i = 0;
        while (i < authors.size() && authors[i] != author) ++i;
        if (i == authors.size()) {
            authors.push_back(author);
            states.push_back(state);
            heads.push_back(head);
        } else {
            states[i] = state;
            heads[i] = head;
        }
    }
    for (const std::string& state : states) {
        if (state == "CHANGES_REQUESTED") {
            pr.review = ReviewState::ChangesRequested;
            return;
        }
    }
    for (std::size_t i = 0; i < states.size(); ++i) {
        if (states[i] == "APPROVED") {
            pr.review = ReviewState::Approved;
            pr.reviewed_head_sha = heads[i];
            return;
        }
    }
}

}  // namespace

PullRequestFacts parse_pull_request_json(const std::string& text) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded()) bad("malformed JSON");
    const json& number = field(root, "number");
    if (!number.is_number_integer() || number.get<long long>() <= 0 || number.get<long long>() > 999999999)
        bad("bad number");
    PullRequestFacts pr;
    pr.number = number.get<int>();
    const std::string state = string_field(root, "state");
    if (state != "OPEN" && state != "CLOSED" && state != "MERGED") bad("unknown state");
    pr.open = state == "OPEN";
    pr.merged = state == "MERGED";
    pr.head_sha = sha_field(root, "headRefOid");
    pr.base_sha = sha_field(root, "baseRefOid");
    const std::string mergeable = string_field(root, "mergeable");
    if (mergeable != "MERGEABLE" && mergeable != "CONFLICTING" && mergeable != "UNKNOWN") bad("unknown mergeable");
    pr.mergeable = mergeable == "MERGEABLE";
    pr.checks = check_state(field(root, "statusCheckRollup"));
    apply_reviews(field(root, "reviews"), pr);
    return pr;
}

GhAdapter::GhAdapter(GhRunner& runner) : runner_(runner) {}

PullRequestFacts GhAdapter::read_pull_request(int number) {
    const GhResult result = runner_.run(build_gh_argv(GhOperation::ReadPullRequest, number, ""));
    // The exit code is reported; output is not echoed because gh error text may include request details.
    if (result.exit_code != 0) throw std::runtime_error("gh pr view failed (exit " + std::to_string(result.exit_code) + ")");
    std::string line = result.output;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    return parse_pull_request_line(line);
}

PullRequestFacts GhAdapter::read_pull_request_detail(int number) {
    const GhResult result = runner_.run(build_gh_argv(GhOperation::ReadPullRequestJson, number, ""));
    if (result.exit_code != 0) throw std::runtime_error("gh pr view failed (exit " + std::to_string(result.exit_code) + ")");
    PullRequestFacts pr = parse_pull_request_json(result.output);
    if (pr.number != number) throw std::runtime_error("GitHub JSON: pull request number mismatch");
    return pr;
}

IssueObservation GhAdapter::read_issue(int number) {
    const GhResult result = runner_.run(build_gh_argv(GhOperation::ReadIssue, number, ""));
    if (result.exit_code != 0) throw std::runtime_error("gh issue view failed (exit " + std::to_string(result.exit_code) + ")");
    std::string line = result.output;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    const std::vector<std::string> fields = split_tabs(line);
    if (fields.size() != 3) throw std::runtime_error("GitHub output: expected 3 issue fields");
    IssueObservation issue;
    issue.number = parse_positive_int(fields[0]);
    issue.open = fields[1] == "OPEN";
    std::string label;
    for (const char c : fields[2] + ",") {
        if (c == ',') {
            if (!label.empty()) issue.labels.push_back(label);
            label.clear();
        } else {
            label.push_back(c);
        }
    }
    return issue;
}

void GhAdapter::comment_on_pull_request(int number, const std::string& body_file) {
    const GhResult result = runner_.run(build_gh_argv(GhOperation::CommentOnPullRequest, number, body_file));
    if (result.exit_code != 0) throw std::runtime_error("gh pr comment failed (exit " + std::to_string(result.exit_code) + ")");
}

bool GhAdapter::merge_pull_request(int number, const std::string& head_sha) {
    const GhResult result = runner_.run(build_gh_argv(GhOperation::MergePullRequest, number, head_sha));
    return result.exit_code == 0;
}

}  // namespace cipher::orchestration
