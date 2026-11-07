#include "cipher/orchestration/pipeline_roles.hpp"

#include "cipher/orchestration/commit_sha.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace cipher::orchestration {
namespace {

constexpr int kGitTimeoutMs = 60000;
constexpr int kTestTimeoutMs = 600000;
const char* const kMechanicalFamily = "cipher-mechanical";

std::string trim(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

bool is_code_path(const std::string& p) { return starts_with(p, "src/") || starts_with(p, "include/"); }
bool is_test_path(const std::string& p) { return starts_with(p, "tests/"); }

// Paths an implementation job must never touch without the owner: governance, CI, pipeline tooling, dependencies, secrets.
bool is_protected_path(const std::string& p) {
    return starts_with(p, "tools/pipeline/") || starts_with(p, ".github/") || starts_with(p, "third_party/") ||
           starts_with(p, ".claude/") || p == "AGENTS.md" || p == "docs/AGENT_ROLE_CONTRACT.md" ||
           p == "CMakeLists.txt" || p.find(".env") != std::string::npos || p.find("secret") != std::string::npos ||
           p.find("credential") != std::string::npos;
}

bool has_secret_marker(const std::string& diff) {
    for (const char* marker : {"BEGIN PRIVATE KEY", "BEGIN RSA PRIVATE KEY", "ghp_", "github_pat_", "sk-ant-", "AKIA"})
        if (diff.find(marker) != std::string::npos) return true;
    return false;
}

}  // namespace

PipelineExecutor::PipelineExecutor(PipelineSettings settings, WorktreeAllocator& allocator, ProcessRunner& runner,
                                   GhAdapter& gh, RoleExecutor& next)
    : settings_(std::move(settings)), allocator_(allocator), runner_(runner), gh_(gh), next_(next) {}

RoleResult PipelineExecutor::run(Role role, const JobRecord& job) {
    if (role == Role::Implementation && job.state == State::PullRequest) return open_pull_request(job);
    if (role == Role::Qa) return run_qa(job);
    if (role == Role::Reviewer) return run_review(job);
    return next_.run(role, job);
}

ProcessResult PipelineExecutor::git(const std::vector<std::string>& args) {
    std::vector<std::string> argv = {settings_.git_executable, "-C", allocator_.path_for(settings_.issue_number).string(),
                                     "--no-pager"};
    argv.insert(argv.end(), args.begin(), args.end());
    return runner_.run(argv, kGitTimeoutMs);
}

bool PipelineExecutor::head_matches(const JobRecord& job) {
    if (!is_full_sha(job.head_sha)) return false;
    if (!std::filesystem::is_directory(allocator_.path_for(settings_.issue_number))) return false;
    const ProcessResult head = git({"rev-parse", "HEAD"});
    return head.exit_code == 0 && trim(head.stdout_text) == job.head_sha;
}

std::vector<std::string> PipelineExecutor::changed_files() {
    if (!is_full_sha(settings_.base_sha)) throw std::runtime_error("base_sha is not a full commit");
    const ProcessResult diff = git({"diff", "--name-only", settings_.base_sha + "..HEAD"});
    if (diff.exit_code != 0) throw std::runtime_error("git diff failed");
    std::vector<std::string> files;
    std::istringstream in(diff.stdout_text);
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (!line.empty()) files.push_back(line);
    }
    return files;
}

void PipelineExecutor::note(const std::string& line) const {
    if (settings_.evidence_file.empty()) return;
    std::ofstream out(settings_.evidence_file, std::ios::app);
    out << line << "\n";
}

RoleResult PipelineExecutor::open_pull_request(const JobRecord& job) {
    const RoleResult blocked{Event{EventKind::Blocked}, kMechanicalFamily};
    try {
        if (!head_matches(job)) {
            note("PR: BLOCKED worktree HEAD does not match the job head");
            return blocked;
        }
        const std::string branch = settings_.branch_prefix + std::to_string(settings_.issue_number);
        const std::vector<int> existing = gh_.find_open_pull_requests(branch);
        if (existing.size() > 1) {
            note("PR: BLOCKED several open PRs for " + branch);
            return blocked;
        }
        // Plain push (never forced): a retry adds a commit to the same branch and therefore the same PR.
        if (git({"push", "origin", "HEAD:refs/heads/" + branch}).exit_code != 0) {
            note("PR: BLOCKED push failed");
            return blocked;
        }
        int number = existing.empty() ? 0 : existing.front();
        if (number == 0) {
            if (settings_.pr_body_file.empty()) throw std::runtime_error("pr_body_file not configured");
            std::ofstream body(settings_.pr_body_file, std::ios::binary | std::ios::trunc);
            body << "Automated CIPHER implementation for issue #" << settings_.issue_number << ".\n\n"
                 << "Head: " << job.head_sha << "\nBase: " << settings_.base_sha
                 << "\n\nNot merged automatically. Requires normal review and green CI.\n";
            body.close();
            number = gh_.create_pull_request(branch, settings_.base_branch,
                                             "cipher: implementation for issue #" + std::to_string(settings_.issue_number),
                                             settings_.pr_body_file.string());
        }
        const PullRequestFacts pr = gh_.read_pull_request_detail(number);
        if (!pr.open || pr.head_sha != job.head_sha) {
            note("PR: BLOCKED PR #" + std::to_string(number) + " is closed or its head differs from the job head");
            return blocked;
        }
        note("PR: #" + std::to_string(number) + " head=" + pr.head_sha + " base=" + pr.base_sha);
        Event event;
        event.kind = EventKind::PullRequestOpened;
        event.head_sha = pr.head_sha;
        return {event, kMechanicalFamily};
    } catch (const std::exception& error) {
        note(std::string("PR: BLOCKED ") + error.what());
        return blocked;
    }
}

RoleResult PipelineExecutor::run_qa(const JobRecord& job) {
    auto result = [&](Verdict verdict, const std::string& why) {
        note("QA: " + std::string(verdict == Verdict::Approve ? "PASS" : verdict == Verdict::Blocked ? "BLOCKED" : "FAIL") +
             " " + why);
        Event event;
        event.kind = verdict == Verdict::Blocked ? EventKind::Blocked : EventKind::QaResult;
        event.head_sha = job.head_sha;
        event.verdict = verdict;
        return RoleResult{event, kMechanicalFamily};
    };
    try {
        if (!head_matches(job)) return result(Verdict::Blocked, "worktree HEAD does not match the job head");
        if (settings_.qa_command_ids.empty()) return result(Verdict::Blocked, "no approved test commands configured");
        const std::vector<std::string> files = changed_files();
        if (files.empty()) return result(Verdict::ChangesRequested, "empty diff");
        bool code = false, tests = false;
        for (const auto& f : files) {
            code = code || is_code_path(f);
            tests = tests || is_test_path(f);
        }
        if (code && !tests) return result(Verdict::ChangesRequested, "source changed without any test change");
        for (const auto& id : settings_.qa_command_ids) {
            const auto it = settings_.test_commands.find(id);
            if (it == settings_.test_commands.end()) return result(Verdict::Blocked, "unknown command id " + id);
            const ProcessResult run = runner_.run(it->second, kTestTimeoutMs);
            note("QA " + id + " @" + job.head_sha + " exit=" + std::to_string(run.exit_code) +
                 (run.timed_out ? " timed_out" : "") + (run.output_overflow ? " output_overflow" : ""));
            if (run.timed_out || run.output_overflow) return result(Verdict::Blocked, id + " timed out or overflowed");
            if (run.exit_code != 0) return result(Verdict::ChangesRequested, id + " exited " + std::to_string(run.exit_code));
        }
        if (!head_matches(job)) return result(Verdict::Blocked, "HEAD moved while QA ran");
        // QA only verifies: any change to tracked files (e.g. a test rewriting sources) invalidates the evidence.
        const ProcessResult status = git({"status", "--porcelain", "--untracked-files=no"});
        if (status.exit_code != 0 || !trim(status.stdout_text).empty())
            return result(Verdict::Blocked, "QA modified tracked files or git status failed");
        return result(Verdict::Approve, std::to_string(files.size()) + " files, all approved commands passed");
    } catch (const std::exception& error) {
        return result(Verdict::Blocked, error.what());
    }
}

RoleResult PipelineExecutor::run_review(const JobRecord& job) {
    auto verdict = [&](Verdict v, const std::string& why) {
        note("REVIEW: " + std::string(v == Verdict::Approve ? "APPROVE" : v == Verdict::ChangesRequested ? "CHANGES_REQUESTED"
                                      : v == Verdict::NeedsHuman ? "NEEDS_HUMAN" : "BLOCKED") + " " + why);
        Event event;
        event.kind = v == Verdict::Blocked ? EventKind::Blocked : EventKind::ReviewVerdict;
        event.head_sha = job.head_sha;
        event.verdict = v;
        return RoleResult{event, kMechanicalFamily};
    };
    try {
        // A mechanical check is not an independent security review, whatever family label it carries.
        if (job.facts.security_sensitive) return verdict(Verdict::NeedsHuman, "security-sensitive job needs a human");
        if (!head_matches(job)) return verdict(Verdict::Blocked, "worktree HEAD does not match the job head");
        const std::vector<std::string> files = changed_files();
        if (files.empty()) return verdict(Verdict::ChangesRequested, "empty diff");
        if (files.size() > settings_.max_changed_files) return verdict(Verdict::NeedsHuman, "diff too large to review mechanically");
        bool code = false, tests = false;
        for (const auto& f : files) {
            if (is_protected_path(f)) return verdict(Verdict::NeedsHuman, "touches protected path " + f);
            code = code || is_code_path(f);
            tests = tests || is_test_path(f);
        }
        const ProcessResult diff = git({"diff", settings_.base_sha + "..HEAD"});
        if (diff.exit_code != 0) return verdict(Verdict::Blocked, "git diff failed");
        if (has_secret_marker(diff.stdout_text)) return verdict(Verdict::ChangesRequested, "possible secret in diff");
        if (code && !tests) return verdict(Verdict::ChangesRequested, "source changed without any test change");
        return verdict(Verdict::Approve, "mechanical checks only (protected paths, secrets, tests, size)");
    } catch (const std::exception& error) {
        return verdict(Verdict::Blocked, error.what());
    }
}

std::optional<Event> observe_ci(const JobRecord& job, GhAdapter& gh, int pr_number) {
    Event event;
    try {
        const PullRequestFacts pr = gh.read_pull_request_detail(pr_number);
        if (!pr.open || pr.head_sha != job.head_sha) {
            event.kind = EventKind::Blocked;
            return event;
        }
        if (pr.checks == CheckState::Pending) return std::nullopt;
        event.kind = EventKind::CiResult;
        event.head_sha = pr.head_sha;
        event.ci_passed = pr.checks == CheckState::Passed;
        return event;
    } catch (const std::exception&) {
        event.kind = EventKind::Blocked;
        return event;
    }
}

}  // namespace cipher::orchestration
