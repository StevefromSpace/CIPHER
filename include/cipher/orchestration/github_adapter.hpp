#ifndef CIPHER_ORCHESTRATION_GITHUB_ADAPTER_HPP
#define CIPHER_ORCHESTRATION_GITHUB_ADAPTER_HPP

#include "cipher/orchestration/github_facts.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace cipher::orchestration {

// Stage 5: typed GitHub boundary. Every operation is a closed enum value mapped to one fixed `gh` argv.
// There is no generic API passthrough and no shell string: the runner receives an argument vector.

enum class GhOperation {
    ReadPullRequest,     // read-only (TSV)
    ReadPullRequestJson, // read-only, strict JSON incl. checks and reviews
    ReadIssue,           // read-only
    CommentOnPullRequest,// write: comment body comes from a file, never inline
    MergePullRequest     // write: squash merge pinned to the head SHA that was gated
};

struct GhResult {
    int exit_code = 0;
    std::string output;   // stdout only; callers must not log it verbatim (it may contain tokens in error text)
};

// Process seam. The production runner is not wired in this build; tests inject a fake. Implementations must
// not invoke a shell.
class GhRunner {
public:
    virtual ~GhRunner() = default;
    virtual GhResult run(const std::vector<std::string>& argv) = 0;
};

// Thrown when a request violates the operation contract (bad number, non-hex SHA, unsafe path).
class GhRequestError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

// Builds the exact argv for an operation. Validates every field before it reaches argv.
std::vector<std::string> build_gh_argv(GhOperation operation, int number, const std::string& argument);

// argument: comment body file path for CommentOnPullRequest; head SHA for MergePullRequest; ignored for reads.
class GhAdapter {
public:
    explicit GhAdapter(GhRunner& runner);

    // Reads the PR and maps the tab-separated fields requested by build_gh_argv. Throws on malformed output.
    PullRequestFacts read_pull_request(int number);
    // Strict JSON read: fails closed on any missing, mistyped or unknown security-relevant field.
    PullRequestFacts read_pull_request_detail(int number);
    IssueObservation read_issue(int number);
    void comment_on_pull_request(int number, const std::string& body_file);
    // Returns true only when gh exits 0. The caller must re-read and re-gate before calling.
    bool merge_pull_request(int number, const std::string& head_sha);

    // PR creation (never merges). Open PRs whose head branch is `head_branch` (empty = none; >1 is the caller's problem).
    std::vector<int> find_open_pull_requests(const std::string& head_branch);
    // Creates a PR and returns its number, parsed from the URL gh prints. Throws on failure or an unparsable URL.
    int create_pull_request(const std::string& head_branch, const std::string& base_branch,
                            const std::string& title, const std::string& body_file);

private:
    GhRunner& runner_;
};

// Parses the TSV line produced by the read argv: number, state, headRefOid, baseRefOid, mergeable.
PullRequestFacts parse_pull_request_line(const std::string& line);

// Strict parser for `gh pr view --json number,state,headRefOid,baseRefOid,mergeable,statusCheckRollup,reviews`.
// Throws std::runtime_error on malformed JSON, missing/mistyped fields, non-full SHAs or unknown enum values.
PullRequestFacts parse_pull_request_json(const std::string& text);

}  // namespace cipher::orchestration

#endif  // CIPHER_ORCHESTRATION_GITHUB_ADAPTER_HPP
