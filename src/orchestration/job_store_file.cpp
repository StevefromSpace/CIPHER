#include "cipher/orchestration/job_store_file.hpp"

#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace cipher::orchestration {
namespace {

namespace fs = std::filesystem;

// Every key the format writes. parse_job() requires exactly this set, each key once.
const std::set<std::string> kKeys = {
    "schema", "state", "resume_state", "escalation", "implementation_executions", "review_rounds",
    "blocked_count", "reevaluation_used", "reevaluation_pending", "pull_request_open", "qa_approved",
    "ci_passed", "review_approved", "head_sha", "reviewed_head_sha", "implementation_family",
    "execution_in_flight", "facts_research", "facts_needs_design", "facts_security_sensitive"};

void require_plain(const std::string& value) {
    if (value.find('\n') != std::string::npos || value.find('\r') != std::string::npos) {
        throw std::invalid_argument("job store value contains a line break");
    }
}

int parse_int(const std::string& value, const std::string& key) {
    // Digits only: every stored integer is a non-negative count.
    if (value.empty() || value.size() > 9 || value.find_first_not_of("0123456789") != std::string::npos) {
        throw std::runtime_error("job store: bad integer for " + key);
    }
    return std::stoi(value);
}

bool parse_bool(const std::string& value, const std::string& key) {
    if (value == "0") return false;
    if (value == "1") return true;
    throw std::runtime_error("job store: bad flag for " + key);
}

// Enums are stored as their integer value. Range checks keep a corrupt file from forming an invalid enum.
template <typename Enum>
Enum parse_enum(const std::string& value, const std::string& key, Enum last) {
    const int number = parse_int(value, key);
    if (number > static_cast<int>(last)) throw std::runtime_error("job store: enum out of range for " + key);
    return static_cast<Enum>(number);
}

std::string flag(bool value) { return value ? "1" : "0"; }

std::string read_whole_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("job store: cannot open " + path.string());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

}  // namespace

FileJobStore::FileJobStore(fs::path directory) : directory_(std::move(directory)) {}

fs::path FileJobStore::path_for(int issue_number) const {
    return directory_ / ("job-" + std::to_string(issue_number) + ".state");
}

void FileJobStore::save(int issue_number, const JobRecord& job) {
    fs::create_directories(directory_);
    const fs::path target = path_for(issue_number);
    fs::path temp = target;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("job store: cannot write " + temp.string());
        out << serialize_job(job);
        out.close();
        if (!out) throw std::runtime_error("job store: write failed for " + temp.string());
    }
    // std::filesystem::rename replaces an existing target on Windows (MoveFileExW, MOVEFILE_REPLACE_EXISTING).
    fs::rename(temp, target);
}

std::optional<JobRecord> FileJobStore::load(int issue_number) const {
    const fs::path target = path_for(issue_number);
    if (!fs::exists(target)) return std::nullopt;
    return parse_job(read_whole_file(target));
}

std::string serialize_job(const JobRecord& job) {
    const std::string head_sha = job.head_sha;
    const std::string reviewed = job.reviewed_head_sha;
    const std::string family = job.implementation_family;
    require_plain(head_sha);
    require_plain(reviewed);
    require_plain(family);

    std::ostringstream out;
    out << "schema=" << kJobStoreSchemaVersion << '\n'
        << "state=" << static_cast<int>(job.state) << '\n'
        << "resume_state=" << static_cast<int>(job.resume_state) << '\n'
        << "escalation=" << static_cast<int>(job.escalation) << '\n'
        << "implementation_executions=" << job.implementation_executions << '\n'
        << "review_rounds=" << job.review_rounds << '\n'
        << "blocked_count=" << job.blocked_count << '\n'
        << "reevaluation_used=" << flag(job.reevaluation_used) << '\n'
        << "reevaluation_pending=" << flag(job.reevaluation_pending) << '\n'
        << "pull_request_open=" << flag(job.pull_request_open) << '\n'
        << "qa_approved=" << flag(job.qa_approved) << '\n'
        << "ci_passed=" << flag(job.ci_passed) << '\n'
        << "review_approved=" << flag(job.review_approved) << '\n'
        << "head_sha=" << head_sha << '\n'
        << "reviewed_head_sha=" << reviewed << '\n'
        << "implementation_family=" << family << '\n'
        << "execution_in_flight=" << flag(job.execution_in_flight) << '\n'
        << "facts_research=" << flag(job.facts.research) << '\n'
        << "facts_needs_design=" << flag(job.facts.needs_design) << '\n'
        << "facts_security_sensitive=" << flag(job.facts.security_sensitive) << '\n';
    return out.str();
}

JobRecord parse_job(const std::string& text) {
    std::map<std::string, std::string> values;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos) throw std::runtime_error("job store: line without '=' in record");
        const std::string key = line.substr(0, equals);
        if (!kKeys.count(key)) throw std::runtime_error("job store: unknown key " + key);
        if (!values.emplace(key, line.substr(equals + 1)).second) {
            throw std::runtime_error("job store: duplicate key " + key);
        }
    }
    for (const auto& key : kKeys) {
        if (!values.count(key)) throw std::runtime_error("job store: missing key " + key);
    }

    const int schema = parse_int(values.at("schema"), "schema");
    if (schema != kJobStoreSchemaVersion) {
        throw std::runtime_error("job store: unsupported schema version " + std::to_string(schema));
    }

    JobRecord job;
    job.state = parse_enum(values.at("state"), "state", State::NeedsHuman);
    job.resume_state = parse_enum(values.at("resume_state"), "resume_state", State::NeedsHuman);
    job.escalation = parse_enum(values.at("escalation"), "escalation", EscalationReason::ExplicitRequest);
    job.implementation_executions = parse_int(values.at("implementation_executions"), "implementation_executions");
    job.review_rounds = parse_int(values.at("review_rounds"), "review_rounds");
    job.blocked_count = parse_int(values.at("blocked_count"), "blocked_count");
    job.reevaluation_used = parse_bool(values.at("reevaluation_used"), "reevaluation_used");
    job.reevaluation_pending = parse_bool(values.at("reevaluation_pending"), "reevaluation_pending");
    job.pull_request_open = parse_bool(values.at("pull_request_open"), "pull_request_open");
    job.qa_approved = parse_bool(values.at("qa_approved"), "qa_approved");
    job.ci_passed = parse_bool(values.at("ci_passed"), "ci_passed");
    job.review_approved = parse_bool(values.at("review_approved"), "review_approved");
    job.head_sha = values.at("head_sha");
    job.reviewed_head_sha = values.at("reviewed_head_sha");
    job.implementation_family = values.at("implementation_family");
    job.execution_in_flight = parse_bool(values.at("execution_in_flight"), "execution_in_flight");
    job.facts.research = parse_bool(values.at("facts_research"), "facts_research");
    job.facts.needs_design = parse_bool(values.at("facts_needs_design"), "facts_needs_design");
    job.facts.security_sensitive = parse_bool(values.at("facts_security_sensitive"), "facts_security_sensitive");
    return job;
}

}  // namespace cipher::orchestration
