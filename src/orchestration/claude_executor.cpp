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

// Model ids are plain tokens: a leading '-' would be read as a flag, whitespace or control bytes are never valid.
void require_safe_model(const std::string& model) {
    if (model.empty()) throw std::invalid_argument("claude model is empty");
    if (model.front() == '-') throw std::invalid_argument("claude model must not start with '-'");
    for (const unsigned char c : model) {
        if (c <= ' ' || c == 0x7f) throw std::invalid_argument("claude model contains whitespace or control characters");
    }
}

// Plain positive decimal such as "0.50" or "2": no sign, exponent, "nan" or "inf".
void require_safe_budget(const std::string& budget) {
    bool seen_dot = false;
    bool seen_nonzero_digit = false;
    for (const char c : budget) {
        if (c == '.' && !seen_dot) {
            seen_dot = true;
        } else if (c >= '0' && c <= '9') {
            if (c != '0') seen_nonzero_digit = true;
        } else {
            throw std::invalid_argument("claude budget must be a plain positive decimal");
        }
    }
    if (budget.empty() || budget.front() == '.' || budget.back() == '.' || !seen_nonzero_digit) {
        throw std::invalid_argument("claude budget must be a plain positive decimal");
    }
}

}  // namespace

const char* to_string(ClaudeStatus status) {
    switch (status) {
        case ClaudeStatus::Ok: return "OK";
        case ClaudeStatus::Timeout: return "TIMEOUT";
        case ClaudeStatus::NonZeroExit: return "NON_ZERO_EXIT";
        case ClaudeStatus::MalformedOutput: return "MALFORMED_OUTPUT";
        case ClaudeStatus::ModelError: return "MODEL_ERROR";
        case ClaudeStatus::RunnerError: return "RUNNER_ERROR";
    }
    return "UNKNOWN";
}

std::vector<std::string> build_claude_argv(const std::string& executable, const std::string& model,
                                           const std::string& max_budget_usd, const std::string& prompt) {
    require_safe_model(model);
    require_safe_budget(max_budget_usd);
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
    if (process.output_overflow) {
        result.status = ClaudeStatus::MalformedOutput;   // truncated JSON is never a usable answer
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
    // Exactly one runner call, no retry. A runner failure becomes a status so callers never see a stray exception.
    ProcessResult process;
    try {
        process = runner.run(argv, timeout_ms);
    } catch (const std::exception&) {
        ClaudeResult failed;
        failed.status = ClaudeStatus::RunnerError;
        return failed;
    }
    return parse_claude_output(process);
}

}  // namespace cipher::orchestration
