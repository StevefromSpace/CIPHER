#include "cipher/orchestration/governed_merge.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>

namespace cipher::orchestration {
namespace fs = std::filesystem;

MergeDecision evaluate_merge_gates(const MergeGateInputs& in) {
    // Preconditions first: a moved head means every earlier gate result is about a different commit.
    if (!in.pr_open) return {false, MergeBlock::PrNotOpen, false};
    if (!in.head_matches_gated_sha) return {false, MergeBlock::HeadChanged, false};
    if (!in.mergeable) return {false, MergeBlock::NotMergeable, false};
    if (!in.ci_all_required_passed) return {false, MergeBlock::CiNotPassed, false};
    if (!in.qa_approved_or_not_applicable) return {false, MergeBlock::QaNotApproved, false};
    if (!in.reviewer_approved) return {false, MergeBlock::ReviewerNotApproved, false};
    if (!in.security_not_applicable_or_approved) return {false, MergeBlock::SecurityNotApproved, true};
    if (!in.no_open_blocker_or_high) return {false, MergeBlock::BlockerOrHighOpen, false};
    if (!in.scope_accepted) return {false, MergeBlock::ScopeNotAccepted, true};
    if (!in.no_protected_content) return {false, MergeBlock::ProtectedContent, true};
    return {true, MergeBlock::None, false};
}

MergeLock::MergeLock(fs::path directory) : directory_(std::move(directory)) {}

fs::path MergeLock::lock_path() const {
    return directory_ / "merge.lock";
}

bool MergeLock::try_acquire(int issue_number) {
    fs::create_directories(directory_);
    // "wx" fails when the file already exists, which makes creation the lock itself (no read-then-write race).
    const std::string path = lock_path().string();
#ifdef _MSC_VER
    std::FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "wx") != 0 || file == nullptr) return false;
#else
    std::FILE* file = std::fopen(path.c_str(), "wx");
    if (file == nullptr) return false;
#endif
    const std::string record = "issue=" + std::to_string(issue_number) + "\n";
    std::fwrite(record.data(), 1, record.size(), file);
    std::fclose(file);
    return true;
}

void MergeLock::release(int issue_number) {
    // Only the holder releases. A release by anyone else leaves the lock in place for a human to inspect.
    if (holder() != issue_number) return;
    std::error_code ignored;
    fs::remove(lock_path(), ignored);
}

std::optional<int> MergeLock::holder() const {
    std::ifstream in(lock_path());
    if (!in) return std::nullopt;
    std::string line;
    std::getline(in, line);
    if (line.rfind("issue=", 0) != 0) return std::nullopt;
    try {
        return std::stoi(line.substr(6));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace cipher::orchestration
