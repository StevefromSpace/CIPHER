#include "cipher/orchestration/github_adapter.hpp"

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace cipher::orchestration {
namespace {

void require_number(int number) {
    if (number <= 0) throw GhRequestError("GitHub number must be positive");
}

bool is_full_sha(const std::string& sha) {
    if (sha.size() != 40) return false;
    for (const char c : sha) {
        const bool lower_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!lower_hex) return false;
    }
    return true;
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

GhAdapter::GhAdapter(GhRunner& runner) : runner_(runner) {}

PullRequestFacts GhAdapter::read_pull_request(int number) {
    const GhResult result = runner_.run(build_gh_argv(GhOperation::ReadPullRequest, number, ""));
    // The exit code is reported; output is not echoed because gh error text may include request details.
    if (result.exit_code != 0) throw std::runtime_error("gh pr view failed (exit " + std::to_string(result.exit_code) + ")");
    std::string line = result.output;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    return parse_pull_request_line(line);
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
