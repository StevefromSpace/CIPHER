// Manual live validation of the local executor (NOT in CTest; free, local Ollama only).
//   cipher_local_executor_validation [model]      (default qwen2.5:3b at 127.0.0.1:11434)
// Runs ImplementationRig(agent=local) once on a docs-only task in a throwaway git repo. A small model failing
// (ImplementationFailed) is an expected, recorded outcome, not a bug. Prints PASS / FAIL / SKIP.
#include "cipher/ollama_backend.hpp"
#include "cipher/orchestration/executable_resolver.hpp"
#include "cipher/orchestration/implementation_rig.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;
using namespace cipher::orchestration;

namespace {
std::string git(ProcessRunner& runner, const std::string& exe, const fs::path& repo, std::vector<std::string> args) {
    std::vector<std::string> argv{exe, "-C", repo.string()};
    argv.insert(argv.end(), args.begin(), args.end());
    std::string out = runner.run(argv, 30000).stdout_text;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    cipher::OllamaConfig oc;
    if (argc > 1) oc.model = argv[1];
    cipher::OllamaBackend backend(oc);
    if (!backend.is_available()) { std::cout << "SKIP: Ollama not reachable\n"; return 0; }
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path base = fs::temp_directory_path() / ("cipher local validation " + std::to_string(stamp));
        const fs::path repo = base / "repo";
        fs::create_directories(repo / "docs");
        std::ofstream(repo / "docs" / "NOTES.md") << "# Notes\n\nSome notes.\n";
        SystemProcessRunner runner;
        const std::string exe = resolve_executable_on_path("git");
        git(runner, exe, repo, {"init", "-q"});
        git(runner, exe, repo, {"config", "user.email", "v@example.invalid"});
        git(runner, exe, repo, {"config", "user.name", "validator"});
        git(runner, exe, repo, {"add", "-A"});
        git(runner, exe, repo, {"commit", "-q", "-m", "init"});
        RigConfig config;
        config.repository = repo;
        config.base_sha = git(runner, exe, repo, {"rev-parse", "HEAD"});
        config.worktree_root = base / "worktrees";
        config.agent = "local";
        config.local_backend = &backend;
        config.local_family = "ollama";
        ImplementationRig rig(config, 1, "Append the single line 'Validated locally.' to the end of docs/NOTES.md");
        JobRecord record;
        record.state = State::Implementation;
        const bool ok = rig.run(Role::Implementation, record).event.kind == EventKind::ImplementationComplete;
        std::ifstream log(config.worktree_root / "evidence-1.log");
        std::stringstream text;
        text << log.rdbuf();
        std::cout << "--- evidence log ---\n" << text.str() << "\n--------------------\n";
        std::cout << (ok ? "PASS: local model completed the docs edit\n"
                         : "FAIL: ImplementationFailed (possible for a small model)\n");
        return ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::cout << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
