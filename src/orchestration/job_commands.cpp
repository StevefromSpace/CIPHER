#include "cipher/orchestration/job_commands.hpp"

#include <algorithm>
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

std::string describe(int issue, const JobRecord& job) {
    std::ostringstream out;
    out << "job " << issue << ": " << to_string(job.state);
    if (job.escalation != EscalationReason::None) out << " (" << to_string(job.escalation) << ")";
    out << " implementation_executions=" << job.implementation_executions
        << " review_rounds=" << job.review_rounds;
    return out.str();
}

constexpr const char* kUsage =
    "Usage: job new <issue> [research] [design] [security] | job status <issue> | job list | "
    "job task <issue> <text> | job step <issue> | job recover <issue> | job log";

}  // namespace

JobCommands::JobCommands(std::filesystem::path directory, RoleExecutor* executor, ExecutorFactory factory)
    : directory_(directory),
      store_(directory),
      log_(directory / "events.log"),
      executor_(executor != nullptr ? executor : &default_executor()),
      factory_(std::move(factory)) {}

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
        if (verb == "log") return {true, log()};
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
    orchestrator.start();
    return describe(issue, orchestrator.job());
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

    std::unique_ptr<RoleExecutor> owned = make_executor(issue);
    Orchestrator orchestrator(issue, saved->facts, owned ? *owned : *executor_, store_, Policy{});
    orchestrator.attach_event_log(log_);
    const bool moved = orchestrator.advance();
    return std::string(moved ? "advanced. " : "no step taken. ") + describe(issue, orchestrator.job());
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
    return std::string(recovered ? "recovered. " : "nothing in flight. ") + describe(issue, orchestrator.job());
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

std::string JobCommands::log() const {
    const std::vector<std::string> lines = log_.read_all();
    if (lines.empty()) return "No job events.";
    std::string out;
    for (const std::string& line : lines) out += (out.empty() ? "" : "\n") + line;
    return out;
}

}  // namespace cipher
