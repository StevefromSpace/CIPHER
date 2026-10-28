#ifndef CIPHER_MODEL_TOOL_LOOP_HPP
#define CIPHER_MODEL_TOOL_LOOP_HPP

#include "cipher/assistant_backend.hpp"
#include "cipher/tool_registry.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace cipher {

/**
 * @brief Owner confirmation for one model-proposed call. Returns true only for an exact "yes".
 *        `target` is what the tool resolved the model's argument to (the canonical file for
 *        filesystem.read), or empty for a tool without an argument.
 */
using ModelToolConfirmation =
    std::function<bool(const std::string& tool_id, const std::string& description, const std::string& target)>;

/**
 * @brief Tool IDs the model may request. memory.list is absent on purpose: its output is private and
 *        no memory-to-provider policy exists yet. filesystem.read is offered because the loop only runs
 *        over a VerifiedLocal route and its output never leaves it.
 */
std::vector<std::string> model_offered_tool_ids();

/**
 * @brief Runs the owner's `ask` question through the local backend, letting the model request
 *        at most one tool per round. Every request goes through AssistantManager::submit, and every
 *        tool call goes through owner confirmation and then ToolRegistry::invoke.
 *
 * The model's reply must be exactly one line, "TOOL_CALL: <id>" or, for filesystem.read,
 * "TOOL_CALL: filesystem.read <relative path>", to request a tool. Anything else that mentions
 * TOOL_CALL is malformed and runs nothing. The argument is untrusted: the tool resolves and checks it
 * before the owner is asked, and the registry checks it again when the call runs.
 */
class ModelToolLoop {
public:
    /// shortcut: the loop is capped at kMaxModelRounds submits (prompt budget and latency); raise only with a measured need.
    static constexpr int kMaxModelRounds = 6;
    /// Refused or failed tool calls tolerated before the loop stops and shows the owner the last reason.
    static constexpr int kMaxToolFailures = 2;
    /// shortcut: tool output above this size is withheld instead of truncated, so the model never sees a partial result.
    static constexpr std::size_t kMaxToolOutputBytes = 4096;
    /// A longer model-supplied argument is malformed; it keeps the confirmation prompt readable.
    static constexpr std::size_t kMaxArgumentBytes = 1024;

    /// Offered IDs that are not registered are dropped. The registry is used for policy and invocation.
    ModelToolLoop(IAssistantBackend& backend,
                  ToolRegistry& registry,
                  ModelToolConfirmation confirm,
                  double timeout_seconds,
                  const std::vector<std::string>& offered_tool_ids);

    /// Returns the text to show the owner. Never throws for backend or tool failures.
    std::string run(const std::string& question);

private:
    std::string build_prompt(const std::string& question, const std::vector<std::string>& results) const;

    IAssistantBackend& backend_;
    ToolRegistry& registry_;
    ModelToolConfirmation confirm_;
    double timeout_seconds_;
    std::vector<std::string> offered_;
};

} // namespace cipher

#endif // CIPHER_MODEL_TOOL_LOOP_HPP
