#include "cipher/orchestration/job_commands.hpp"

#include <algorithm>
#include <ctime>
#include <exception>
#include <fstream>
#include <sstream>
#include <vector>

namespace cipher {

namespace {

using namespace orchestration;

// Default executor: no role has a real implementation yet, so every role hands the job to the owner.
class EscalatingExecutor final : public RoleExecutor {
public:
    RoleResult run(Role, const JobRecord&) override {
        return {Event{EventKind::Escalate, {}, Verdict::None, Verdict::None, false, {},
                      EscalationReason::RoleNotSupported},
                std::string()};
    }
};

EscalatingExecutor& default_executor() {
    static EscalatingExecutor instance;
    return instance;
}

std::vector<std::string> split(const std::string& text) {
    std::istringstream stream(text);
    std::vector<std::string> words;
    for (std::string word; stream >> word;) words.push_back(word);
    return words;
}

// Positive issue number written in plain digits only; returns 0 when invalid.
int parse_issue(const std::string& text) {
    if (text.empty() || text.size() > 9) return 0;
    if (!std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) return 0;
    return std::stoi(text);
}

// Unique per job: create() refuses a second job for the same issue, so issue + creation second is stable.
std::string new_run_id(int issue) {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y%m%dT%H%M%SZ", &parts);
    return "run-" + std::to_string(issue) + "-" + buffer;
}

std::string describe(int issue, const JobRecord& job) {
    std::ostringstream out;
    out << "job " << issue << ": " << to_string(job.state);
    if (job.escalation != EscalationReason::None) out << " (" << to_string(job.escalation) << ")";
    out << " implementation_executions=" << job.implementation_executions
        << " review_rounds=" << job.review_rounds;
    return out.str();
}

// The record is saved before the event is logged, so a failed log write leaves a correct, persisted job. Say so
// instead of reporting a job error, and say plainly that the missing line is not back-filled.
std::string log_warning(const Orchestrator& orchestrator) {
    if (orchestrator.log_error().empty()) return std::string();
    return "\nWARNING: the job state was saved, but its event could not be logged (" + orchestrator.log_error() +
           "). The event log is diagnostic history; this line will not be added later.";
}

constexpr const char* kUsage =
    "Usage: job new <issue> [research] [design] [security] | job status <issue> | job list | "
    "job task <issue> <text> | job step <issue> | job recover <issue> | job ci <issue> <pr> | "
    "job show <issue> [--json] | job log [issue]";

}  // namespace

JobCommands::JobCommands(std::filesystem::path directory, RoleExecutor* executor, ExecutorFactory factory,
                         CiObserver ci)
    : directory_(directory),
      store_(directory),
      log_(directory / "events.log"),
      executor_(executor != nullptr ? executor : &default_executor()),
      factory_(std::move(factory)),
      ci_(std::move(ci)) {}

JobCommandResult JobCommands::execute(const std::string& input) {
    const std::vector<std::string> words = split(input);
    if (words.empty() || words[0] != "job") return {};
    if (words.size() < 2) return {true, kUsage};

    const std::string& verb = words[1];
    const std::string rest = [&] {
        std::string joined;
        for (std::size_t i = 2; i < words.size(); ++i) joined += (i > 2 ? " " : "") + words[i];
        return joined;
    }();
    try {
        if (verb == "new") return {true, create(rest)};
        if (verb == "status") return {true, status(rest)};
        if (verb == "list") return {true, list()};
        if (verb == "step") return {true, step(rest)};
        if (verb == "recover") return {true, recover(rest)};
        if (verb == "log") return {true, log(rest)};
        if (verb == "show") return {true, show(rest)};
        if (verb == "ci") return {true, ci(rest)};
        if (verb == "task") return {true, task(rest)};
    } catch (const std::exception& error) {
        return {true, std::string("job error: ") + error.what()};
    }
    return {true, kUsage};
}

std::string JobCommands::create(const std::string& arguments) {
    const std::vector<std::string> words = split(arguments);
    const int issue = words.empty() ? 0 : parse_issue(words[0]);
    if (issue == 0) return "job new needs a positive issue number.";
    if (store_.load(issue)) return "job " + std::to_string(issue) + " already exists; use job status.";

    IssueFacts facts;
    for (std::size_t i = 1; i < words.size(); ++i) {
        if (words[i] == "research") facts.research = true;
        else if (words[i] == "design") facts.needs_design = true;
        else if (words[i] == "security") facts.security_sensitive = true;
        else return "Unknown flag '" + words[i] + "'. Flags: research design security.";
    }

    Orchestrator orchestrator(issue, facts, *executor_, store_, Policy{});
    orchestrator.attach_event_log(log_);
    orchestrator.assign_run_id(new_run_id(issue));
    orchestrator.start();
    return describe(issue, orchestrator.job()) + log_warning(orchestrator);
}

std::string JobCommands::status(const std::string& arguments) const {
    const int issue = parse_issue(arguments);
    if (issue == 0) return "job status needs a positive issue number.";
    const auto job = store_.load(issue);
    if (!job) return "No job for issue " + std::to_string(issue) + ".";
    return describe(issue, *job);
}

std::string JobCommands::list() const {
    std::vector<int> issues;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory_, ec)) {
        const std::string name = entry.path().filename().string();
        const std::string prefix = "job-";
        const std::string suffix = ".state";
        if (name.size() <= prefix.size() + suffix.size() || name.rfind(prefix, 0) != 0 ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        const int issue =
            parse_issue(name.substr(prefix.size(), name.size() - prefix.size() - suffix.size()));
        if (issue != 0) issues.push_back(issue);
    }
    if (issues.empty()) return "No jobs.";
    std::sort(issues.begin(), issues.end());
    std::string out;
    for (const int issue : issues) {
        const auto job = store_.load(issue);
        out += (out.empty() ? "" : "\n") + (job ? describe(issue, *job) : "job " + std::to_string(issue) + ": unreadable");
    }
    return out;
}

std::string JobCommands::step(const std::string& arguments) {
    const int issue = parse_issue(arguments);
    if (issue == 0) return "job step needs a positive issue number.";
    const auto saved = store_.load(issue);
    if (!saved) return "No job for issue " + std::to_string(issue) + ".";
    // Checked before make_executor so a flagged record never reaches an executor.
    if (saved->execution_in_flight) {
        return "execution in flight for issue " + std::to_string(issue) + "; run job recover " +
               std::to_string(issue) + " first. No step taken.";
    }

    std::unique_ptr<RoleExecutor> owned = make_executor(issue);
    Orchestrator orchestrator(issue, saved->facts, owned ? *owned : *executor_, store_, Policy{});
    orchestrator.attach_event_log(log_);
    const bool moved = orchestrator.advance();
    return std::string(moved ? "advanced. " : "no step taken. ") + describe(issue, orchestrator.job()) +
           log_warning(orchestrator);
}

std::string JobCommands::recover(const std::string& arguments) {
    const int issue = parse_issue(arguments);
    if (issue == 0) return "job recover needs a positive issue number.";
    const auto saved = store_.load(issue);
    if (!saved) return "No job for issue " + std::to_string(issue) + ".";

    std::unique_ptr<RoleExecutor> owned = make_executor(issue);
    Orchestrator orchestrator(issue, saved->facts, owned ? *owned : *executor_, store_, Policy{});
    orchestrator.attach_event_log(log_);
    const bool recovered = orchestrator.recover();
    if (!recovered && orchestrator.job().execution_in_flight) {
        return "inconsistent record: execution flagged in flight outside Implementation; left unchanged for the owner. " +
               describe(issue, orchestrator.job()) + log_warning(orchestrator);
    }
    return std::string(recovered ? "recovered. " : "nothing in flight. ") + describe(issue, orchestrator.job()) +
           log_warning(orchestrator);
}

std::string JobCommands::load_task(int issue) const {
    std::ifstream in(directory_ / ("task-" + std::to_string(issue) + ".txt"), std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return in ? text.str() : std::string();
}

std::unique_ptr<RoleExecutor> JobCommands::make_executor(int issue) const {
    if (!factory_) return nullptr;
    const std::string task = load_task(issue);
    return task.empty() ? nullptr : factory_(issue, task);
}

std::string JobCommands::task(const std::string& arguments) {
    if (!factory_) {
        return "job task is unavailable: orchestration_repo, orchestration_base_sha and orchestration_worktree_root "
               "are not all set.";
    }
    const std::size_t split_at = arguments.find(' ');
    const int issue = parse_issue(arguments.substr(0, split_at));
    if (issue == 0 || split_at == std::string::npos) return "Usage: job task <issue> <text>";
    if (!store_.load(issue)) return "No job for issue " + std::to_string(issue) + ".";
    const std::string text = arguments.substr(split_at + 1);
    if (text.size() > 2000) return "Task text is limited to 2000 characters.";
    std::ofstream out(directory_ / ("task-" + std::to_string(issue) + ".txt"), std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) return "Could not save the task.";
    return "task saved for job " + std::to_string(issue) + "; run job step " + std::to_string(issue) + ".";
}

// Stateless and idempotent: the job record is the only state, so an interrupted `job ci` is simply run again.
// The state machine rejects a CI result whose head differs from the job head (stale CI); that rejection is logged.
std::string JobCommands::ci(const std::string& arguments) {
    if (!ci_) return "job ci is unavailable: gh was not found on PATH.";
    const std::vector<std::string> words = split(arguments);
    const int issue = words.size() == 2 ? parse_issue(words[0]) : 0;
    const int pr = words.size() == 2 ? parse_issue(words[1]) : 0;
    if (issue == 0 || pr == 0) return "Usage: job ci <issue> <pr-number>";
    const auto saved = store_.load(issue);
    if (!saved) return "No job for issue " + std::to_string(issue) + ".";
    if (saved->state != State::Ci) return "job is not waiting for CI. " + describe(issue, *saved);

    const std::optional<Event> event = ci_(*saved, pr);
    if (!event) return "CI still pending for PR " + std::to_string(pr) + ". " + describe(issue, *saved);

    Orchestrator orchestrator(issue, saved->facts, *executor_, store_, Policy{});
    orchestrator.attach_event_log(log_);
    const bool accepted = orchestrator.apply(*event);
    return std::string(accepted ? "CI recorded. " : "CI result rejected (stale or invalid). ") +
           describe(issue, orchestrator.job()) + log_warning(orchestrator);
}

std::string JobCommands::log(const std::string& arguments) const {
    const std::string filter = arguments.empty() ? std::string() : std::to_string(parse_issue(arguments));
    if (!arguments.empty() && filter == "0") return "job log takes an optional positive issue number.";
    const std::string prefix = "issue=" + filter + '\t';
    std::string out;
    for (const std::string& line : log_.read_all()) {
        if (!filter.empty() && line.rfind(prefix, 0) != 0) continue;
        out += (out.empty() ? "" : "\n") + line;
    }
    return out.empty() ? "No job events." : out;
}

// Read-only summary of the persisted record plus that job's own events. --json emits a flat object for scripts.
std::string JobCommands::show(const std::string& arguments) const {
    const std::vector<std::string> words = split(arguments);
    const bool json = words.size() == 2 && words[1] == "--json";
    const int issue = (words.size() == 1 || json) ? parse_issue(words[0]) : 0;
    if (issue == 0) return "Usage: job show <issue> [--json]";
    const auto job = store_.load(issue);
    if (!job) return "No job for issue " + std::to_string(issue) + ".";

    const auto yn = [](bool v) { return v ? "true" : "false"; };
    std::ostringstream out;
    if (json) {
        // Values are enum names and hex SHAs only, so no escaping is needed.
        out << "{\"issue\":" << issue << ",\"run_id\":\"" << job->run_id << "\",\"state\":\"" << to_string(job->state) << "\",\"escalation\":\""
            << to_string(job->escalation) << "\",\"implementation_executions\":" << job->implementation_executions
            << ",\"review_rounds\":" << job->review_rounds << ",\"blocked_count\":" << job->blocked_count
            << ",\"pull_request_open\":" << yn(job->pull_request_open) << ",\"qa_approved\":"
            << yn(job->qa_approved) << ",\"ci_passed\":" << yn(job->ci_passed) << ",\"review_approved\":"
            << yn(job->review_approved) << ",\"head_sha\":\"" << job->head_sha << "\",\"reviewed_head_sha\":\""
            << job->reviewed_head_sha << "\",\"execution_in_flight\":" << yn(job->execution_in_flight) << "}";
        return out.str();
    }
    out << describe(issue, *job) << '\n'
        << "  run_id=" << (job->run_id.empty() ? "-" : job->run_id) << '\n'
        << "  blocked_count=" << job->blocked_count << " pull_request_open=" << yn(job->pull_request_open)
        << " qa_approved=" << yn(job->qa_approved) << " ci_passed=" << yn(job->ci_passed)
        << " review_approved=" << yn(job->review_approved) << '\n'
        << "  head_sha=" << (job->head_sha.empty() ? "-" : job->head_sha)
        << " reviewed_head_sha=" << (job->reviewed_head_sha.empty() ? "-" : job->reviewed_head_sha) << '\n'
        << "  execution_in_flight=" << yn(job->execution_in_flight)
        << (job->execution_in_flight ? " (run job recover)" : "") << '\n'
        << "events:\n" << log(std::to_string(issue));
    return out.str();
}

}  // namespace cipher
