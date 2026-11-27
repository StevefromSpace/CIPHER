// Manual live validation of GhAdapter PR creation (NOT in CTest; talks to real GitHub).
//   cipher_gh_pr_validation <gh> <head-branch> <base-branch> <title> <body-file> <expected-head-sha>
// Refuses to create a PR if one is already open for the head branch. Never merges.

#include "cipher/orchestration/gh_process_runner.hpp"
#include "cipher/orchestration/github_adapter.hpp"

#include <iostream>

int main(int argc, char** argv) {
    using namespace cipher::orchestration;
    if (argc != 7) { std::cerr << "usage: gh head base title body-file expected-head-sha\n"; return 2; }
    try {
        SystemProcessRunner sys;
        GhProcessRunner runner(sys, argv[1], 60000);
        GhAdapter gh(runner);
        const auto existing = gh.find_open_pull_requests(argv[2]);
        if (!existing.empty()) { std::cerr << "STOP: open PR already exists for " << argv[2] << "\n"; return 3; }
        const int number = gh.create_pull_request(argv[2], argv[3], argv[4], argv[5]);
        std::cout << "created PR number " << number << "\n";
        const auto pr = gh.read_pull_request_detail(number);
        std::cout << "open=" << pr.open << " merged=" << pr.merged << " head=" << pr.head_sha << " base=" << pr.base_sha << "\n";
        const bool ok = pr.open && !pr.merged && pr.head_sha == argv[6];
        std::cout << (ok ? "PASS: head SHA matches pushed commit\n" : "FAIL: head SHA mismatch\n");
        return ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << "\n";
        return 1;
    }
}
