#ifndef CIPHER_ORCHESTRATION_JOB_STORE_FILE_HPP
#define CIPHER_ORCHESTRATION_JOB_STORE_FILE_HPP

#include "cipher/orchestration/orchestrator.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace cipher::orchestration {

// Schema of the on-disk job record. A file with any other value is refused, never upgraded silently.
inline constexpr int kJobStoreSchemaVersion = 1;

// Stage 8 durable store. One file per issue: <directory>/job-<issue>.state, key=value lines.
// A save writes <file>.tmp and renames it over the target, so a reader sees the old or the new record.
// A target that cannot be parsed throws; it is never treated as missing, so no state is discarded.
class FileJobStore final : public JobStore {
public:
    explicit FileJobStore(std::filesystem::path directory);

    void save(int issue_number, const JobRecord& job) override;
    std::optional<JobRecord> load(int issue_number) const override;

    std::filesystem::path path_for(int issue_number) const;

private:
    std::filesystem::path directory_;
};

// Throws std::invalid_argument when a string value contains a line break, which the format cannot hold.
std::string serialize_job(const JobRecord& job);

// Throws std::runtime_error on a wrong or missing schema, a missing, duplicate or unknown key, or a bad value.
JobRecord parse_job(const std::string& text);

}  // namespace cipher::orchestration

#endif  // CIPHER_ORCHESTRATION_JOB_STORE_FILE_HPP
