// Manual live validation of ClaudeToolAgent (NOT in CTest; one PAID claude call).
//   cipher_claude_tool_agent_validation <claude> <model> <max-budget-usd>
// With an empty transcript the agent must propose fs.read of README.md.
#include "cipher/orchestration/implementation_executor.hpp"

#include <iostream>

namespace {
// Forwards to the real runner and echoes the raw claude JSON so total_cost_usd is visible.
class EchoRunner final : public cipher::orchestration::ProcessRunner {
public:
    cipher::orchestration::ProcessResult run(const std::vector<std::string>& argv, int timeout_ms) override {
        auto result = inner_.run(argv, timeout_ms);
        std::cout << "RAW exit=" << result.exit_code << " stdout=" << result.stdout_text << "\n";
        return result;
    }
private:
    cipher::orchestration::SystemProcessRunner inner_;
};
}  // namespace

int main(int argc, char** argv) {
    using namespace cipher::orchestration;
    if (argc != 4) { std::cerr << "usage: claude model max-budget-usd\n"; return 2; }
    try {
        EchoRunner sys;
        ClaudeToolAgent agent(sys, argv[1], argv[2], argv[3], 120000);
        const AgentStep first = agent.next("Read README.md, then report done.", {});
        std::cout << "step1 done=" << first.done << " tool=" << first.call.tool << " path=" << first.call.path << "\n";
        const bool ok = !first.done && first.call.tool == "fs.read" && first.call.path == "README.md";
        std::cout << (ok ? "PASS: valid fs.read proposal parsed\n" : "FAIL: unexpected first step\n");
        return ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << "\n";
        return 1;
    }
}
