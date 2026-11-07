#include "cipher/orchestration/worktree_allocator.hpp"

#include <fstream>
#include <stdexcept>
#include <string>

namespace cipher::orchestration {
namespace fs = std::filesystem;

WorktreeAllocator::WorktreeAllocator(fs::path root, WorktreeBackend& backend)
    : root_(std::move(root)), backend_(backend) {}

fs::path WorktreeAllocator::path_for(int issue_number) const {
    // Positive issue numbers only: a zero or negative value would not name a managed job directory.
    if (issue_number <= 0) throw std::invalid_argument("worktree allocator: issue number must be positive");
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

void WorktreeAllocator::release(int issue_number) {
    const fs::path directory = path_for(issue_number);
    const fs::path record = owner_record_for(issue_number);
    // Both must exist and the record must name this issue; anything else is not a managed worktree.
    if (!fs::exists(directory) || !fs::exists(record)) {
        throw std::runtime_error("worktree allocator: refusing to release unmanaged path " + directory.string());
    }
    {
        // Scoped so the handle is closed before the record is deleted (Windows refuses to delete an open file).
        std::ifstream in(record, std::ios::binary);
        std::string line;
        std::getline(in, line);
        if (line != "issue=" + std::to_string(issue_number)) {
            throw std::runtime_error("worktree allocator: owner record does not match issue " +
                                     std::to_string(issue_number));
        }
    }
    // Git removes the worktree first; the record goes only after that succeeds, so a failed removal stays owned.
    backend_.remove(directory, issue_number);
    fs::remove(record);
}

}  // namespace cipher::orchestration
