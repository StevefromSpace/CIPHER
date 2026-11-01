#include "cipher/orchestration/claude_executor.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace cipher::orchestration {

namespace {

// Argv reads the model and budget as values, so they are passed as-is; only the prompt needs a guard.
void require_safe_prompt(const std::string& prompt) {
    if (prompt.empty()) throw std::invalid_argument("claude prompt is empty");
    if (prompt.front() == '-') throw std::invalid_argument("claude prompt must not start with '-'");
}

}  // namespace

const char* to_string(ClaudeStatus status) {
    switch (status) {
        case ClaudeStatus::Ok: return "OK";
        case ClaudeStatus::Timeout: return "TIMEOUT";
        case ClaudeStatus::NonZeroExit: return "NON_ZERO_EXIT";
        case ClaudeStatus::MalformedOutput: return "MALFORMED_OUTPUT";
        case ClaudeStatus::ModelError: return "MODEL_ERROR";
    }
    return "UNKNOWN";
}

std::vector<std::string> build_claude_argv(const std::string& executable, const std::string& model,
                                           const std::string& max_budget_usd, const std::string& prompt) {
    if (model.empty()) throw std::invalid_argument("claude model is empty");
    require_safe_prompt(prompt);
    // --tools "" disables every tool, so this call can only return text.
    return {executable,
            "-p",
            "--tools",
            "",
            "--no-session-persistence",
            "--output-format",
            "json",
            "--max-budget-usd",
            max_budget_usd,
            "--model",
            model,
            prompt};
}

ClaudeResult parse_claude_output(const ProcessResult& process) {
    ClaudeResult result;
    if (process.timed_out) {
        result.status = ClaudeStatus::Timeout;
        return result;
    }
    // Parse even on non-zero exit: the budget overrun exits 1 but still prints a diagnostic JSON object.
    const nlohmann::json body = nlohmann::json::parse(process.stdout_text, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        result.status = process.exit_code != 0 ? ClaudeStatus::NonZeroExit : ClaudeStatus::MalformedOutput;
        return result;
    }
    if (body.contains("subtype") && body["subtype"].is_string()) result.subtype = body["subtype"].get<std::string>();
    if (body.contains("total_cost_usd") && body["total_cost_usd"].is_number()) {
        result.total_cost_usd = body["total_cost_usd"].get<double>();
    }
    const bool is_error = body.value("is_error", true);
    if (process.exit_code != 0 || is_error || result.subtype != "success") {
        result.status = process.exit_code != 0 ? ClaudeStatus::NonZeroExit : ClaudeStatus::ModelError;
        return result;
    }
    if (!body.contains("result") || !body["result"].is_string()) {
        result.status = ClaudeStatus::MalformedOutput;
        return result;
    }
    result.status = ClaudeStatus::Ok;
    result.text = body["result"].get<std::string>();
    return result;
}

ClaudeResult run_claude(ProcessRunner& runner, const std::string& executable, const std::string& model,
                        const std::string& max_budget_usd, const std::string& prompt, int timeout_ms) {
    const std::vector<std::string> argv = build_claude_argv(executable, model, max_budget_usd, prompt);
    return parse_claude_output(runner.run(argv, timeout_ms));
}

}  // namespace cipher::orchestration
