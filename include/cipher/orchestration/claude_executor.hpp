#pragma once

#include "cipher/orchestration/process_runner.hpp"

#include <string>
#include <vector>

namespace cipher::orchestration {

// Observed protocol (claude 2.1.316, one live call on claude-sonnet-5-5 with --max-budget-usd 0.50):
// stdout is one JSON object: {"type":"result","subtype":"success","is_error":false,"result":"OK",
// "stop_reason":"end_turn","total_cost_usd":0.0235484,...}. Budget overrun: exit 1, subtype "error_max_budget_usd".
enum class ClaudeStatus {
    Ok,
    Timeout,
    NonZeroExit,
    MalformedOutput,
    ModelError,   // JSON parsed but is_error or subtype is not "success"
    RunnerError,  // the runner itself threw (could not launch); the call was not retried
};

const char* to_string(ClaudeStatus status);

struct ClaudeResult {
    ClaudeStatus status = ClaudeStatus::MalformedOutput;
    std::string text;        // assistant text; only set when status is Ok
    std::string subtype;     // error subtype from the CLI, for diagnostics; contains no prompt text
    double total_cost_usd = 0.0;
};

// Fixed argv. The prompt is a single argv element, so no shell or flag injection through prompt text.
// Throws std::invalid_argument for a prompt or model that starts with '-' (read as a flag), an empty or
// whitespace-containing model, or a budget that is not a plain positive decimal.
std::vector<std::string> build_claude_argv(const std::string& executable, const std::string& model,
                                           const std::string& max_budget_usd, const std::string& prompt);

// Pure: maps captured process output to a result. Never throws on bad input; malformed output is a status.
ClaudeResult parse_claude_output(const ProcessResult& process);

// Runs one bounded, tool-less Claude call through the runner and classifies the outcome.
ClaudeResult run_claude(ProcessRunner& runner, const std::string& executable, const std::string& model,
                        const std::string& max_budget_usd, const std::string& prompt, int timeout_ms);

}  // namespace cipher::orchestration
