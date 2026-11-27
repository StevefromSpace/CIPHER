#ifndef CIPHER_ORCHESTRATION_QA_PROFILE_HPP
#define CIPHER_ORCHESTRATION_QA_PROFILE_HPP

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace cipher::orchestration {

// Absolute paths of the tools QA may launch. An empty path means the tool is unavailable.
struct QaTools {
    std::string git, cmake, ctest, python;
};

// Approved QA commands: id -> fixed argv (no shell), and the order they run in.
struct QaPlan {
    std::map<std::string, std::vector<std::string>> commands;
    std::vector<std::string> ids;
};

// Profile names, comma separated, from the owner's config: check, build, ctest, python.
//   check  git diff --check
//   build  cmake configure + build (out-of-tree in @p build_dir, so QA never writes into the worktree)
//   ctest  ctest in @p build_dir (needs build first)
//   python unittest discovery of integrations/ and tools/pipeline/tests (-B: no bytecode written)
// Same argv on Windows and Linux (--config / -C are ignored by single-config generators).
// Throws std::invalid_argument for an unknown name, a duplicate, ctest without build, or an unavailable tool:
// a required check is never silently dropped, because a skipped check must not read as a pass.
QaPlan build_qa_plan(const std::string& profiles, const std::filesystem::path& worktree,
                     const std::filesystem::path& build_dir, const QaTools& tools);

}  // namespace cipher::orchestration

#endif  // CIPHER_ORCHESTRATION_QA_PROFILE_HPP
