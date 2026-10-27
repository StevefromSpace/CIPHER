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
 */
using ModelToolConfirmation = std::function<bool(const std::string& tool_id, const std::string& description)>;

/**
 * @brief Tool IDs the model may request. memory.list and filesystem.read are absent on purpose:
 *        their output is private and no memory-to-provider or file-output policy exists yet.
 */
std::vector<std::string> model_offered_tool_ids();

/**
 * @brief Runs the owner's `ask` question through the local backend, letting the model request
 *        at most one tool per round. Every request goes through AssistantManager::submit, and every
 *        tool call goes through owner confirmation and then ToolRegistry::invoke.
 *
 * The model's reply must be exactly "TOOL_CALL: <id>" to request a tool. Anything else that mentions
 * TOOL_CALL is malformed and runs nothing.
 */
class ModelToolLoop {
public:
    /// shortcut: the loop is capped at kMaxModelRounds submits (prompt budget and latency); raise only with a measured need.
    static constexpr int kMaxModelRounds = 3;
    /// shortcut: tool output above this size is withheld instead of truncated, so the model never sees a partial result.
    static constexpr std::size_t kMaxToolOutputBytes = 4096;

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
