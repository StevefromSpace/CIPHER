#include "cipher/orchestration/qa_profile.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>

namespace cipher::orchestration {

namespace {

// Fixed script; paths arrive as argv, never interpolated. Fails on zero tests or any skip.
constexpr const char* kPythonRunner =
    "import os,sys,unittest;os.chdir(sys.argv[1]);sys.path.insert(0,sys.argv[1]);"
    "r=unittest.TextTestRunner().run(unittest.defaultTestLoader.discover(sys.argv[2]));"
    "sys.exit(0 if r.wasSuccessful() and r.testsRun>0 and not r.skipped else 1)";

void require(const std::string& tool, const char* name) {
    if (tool.empty()) throw std::invalid_argument(std::string("QA profile needs ") + name + ", which was not found");
}

void add(QaPlan& plan, const std::string& id, std::vector<std::string> argv) {
    plan.commands[id] = std::move(argv);
    plan.ids.push_back(id);
}

}  // namespace

QaPlan build_qa_plan(const std::string& profiles, const std::filesystem::path& worktree,
                     const std::filesystem::path& build_dir, const QaTools& tools) {
    QaPlan plan;
    std::vector<std::string> seen;
    std::istringstream in(profiles);
    const std::string wt = worktree.string();
    const std::string build = build_dir.string();
    for (std::string name; std::getline(in, name, ',');) {
        name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c); }), name.end());
        if (name.empty()) continue;
        if (std::find(seen.begin(), seen.end(), name) != seen.end()) throw std::invalid_argument("duplicate QA profile " + name);
        if (name == "ctest" && std::find(seen.begin(), seen.end(), "build") == seen.end())
            throw std::invalid_argument("QA profile ctest must come after build");
        seen.push_back(name);
        if (name == "check") {
            require(tools.git, "git");
            add(plan, "check", {tools.git, "-C", wt, "diff", "--check"});
        } else if (name == "build") {
            require(tools.cmake, "cmake");
            add(plan, "cmake-configure", {tools.cmake, "-S", wt, "-B", build});
            add(plan, "cmake-build", {tools.cmake, "--build", build, "--config", "Release", "--parallel"});
        } else if (name == "ctest") {
            require(tools.ctest, "ctest");
            add(plan, "ctest", {tools.ctest, "--test-dir", build, "-C", "Release", "--output-on-failure", "--timeout", "300"});
        } else if (name == "python") {
            require(tools.python, "python");
            add(plan, "python-integrations", {tools.python, "-B", "-c", kPythonRunner, wt, "integrations"});
            add(plan, "python-pipeline", {tools.python, "-B", "-c", kPythonRunner, wt, "tools/pipeline/tests"});
        } else {
            throw std::invalid_argument("unknown QA profile '" + name + "' (check, build, ctest, python)");
        }
    }
    if (plan.ids.empty()) throw std::invalid_argument("QA profile list is empty");
    return plan;
}

}  // namespace cipher::orchestration
