#include "cipher/orchestration/worktree_allocator.hpp"

#include <fstream>
#include <stdexcept>
#include <string>

namespace cipher::orchestration {
namespace fs = std::filesystem;

WorktreeAllocator::WorktreeAllocator(fs::path root, WorktreeBackend& backend)
    : root_(std::move(root)), backend_(backend) {}

fs::path WorktreeAllocator::path_for(int issue_number) const {
    return root_ / ("job-" + std::to_string(issue_number));
}

fs::path WorktreeAllocator::owner_record_for(int issue_number) const {
    return root_ / ("job-" + std::to_string(issue_number) + ".owner");
}

WorktreeOutcome WorktreeAllocator::allocate(int issue_number) {
    const fs::path directory = path_for(issue_number);
    const fs::path record = owner_record_for(issue_number);
    const bool directory_exists = fs::exists(directory);
    const bool record_exists = fs::exists(record);

    if (directory_exists || record_exists) {
        // Reuse only when both the directory and the record exist and the record names this issue.
        if (!directory_exists || !record_exists) return WorktreeOutcome::Conflict;
        std::ifstream in(record, std::ios::binary);
        std::string line;
        std::getline(in, line);
        if (line == "issue=" + std::to_string(issue_number)) return WorktreeOutcome::Reused;
        return WorktreeOutcome::Conflict;
    }

    fs::create_directories(root_);
    backend_.add(directory, issue_number);
    // Written last: if the process stops between add and this write, the next run sees a directory with no
    // record and reports a conflict instead of guessing ownership.
    std::ofstream out(record, std::ios::binary | std::ios::trunc);
    out << "issue=" << issue_number << '\n';
    out.close();
    if (!out) throw std::runtime_error("worktree allocator: cannot write owner record " + record.string());
    return WorktreeOutcome::Created;
}

}  // namespace cipher::orchestration
