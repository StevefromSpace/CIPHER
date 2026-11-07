#ifndef CIPHER_ORCHESTRATION_WORKTREE_ALLOCATOR_HPP
#define CIPHER_ORCHESTRATION_WORKTREE_ALLOCATOR_HPP

#include <filesystem>

namespace cipher::orchestration {

// Git operation seam. The real implementation runs `git worktree add` with an argument vector; tests use a fake.
class WorktreeBackend {
public:
    virtual ~WorktreeBackend() = default;
    // Creates a worktree directory at path for the job. Must leave an existing directory in place on success.
    virtual void add(const std::filesystem::path& path, int issue_number) = 0;
    // Removes a worktree previously created by add. Must refuse a dirty or unregistered tree; never force.
    virtual void remove(const std::filesystem::path& path, int issue_number) = 0;
};

enum class WorktreeOutcome {
    Created,    // a new worktree was added and its ownership recorded
    Reused,     // the recorded owner is this job; nothing changed
    Conflict    // a directory or a record exists that this job does not own; the caller must stop globally
};

// Allocates <root>/job-<issue> for one job. Ownership lives in <root>/job-<issue>.owner, outside the
// worktree, so the worktree's own git status is untouched. A directory without a matching record is never reused.
class WorktreeAllocator {
public:
    WorktreeAllocator(std::filesystem::path root, WorktreeBackend& backend);

    WorktreeOutcome allocate(int issue_number);
    // Removes the worktree and its record only when the record names this issue. Never deletes a bare directory.
    void release(int issue_number);
    std::filesystem::path path_for(int issue_number) const;
    std::filesystem::path owner_record_for(int issue_number) const;

private:
    std::filesystem::path root_;
    WorktreeBackend& backend_;
};

}  // namespace cipher::orchestration

#endif  // CIPHER_ORCHESTRATION_WORKTREE_ALLOCATOR_HPP
