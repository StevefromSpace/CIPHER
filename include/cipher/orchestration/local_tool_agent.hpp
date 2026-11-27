#ifndef CIPHER_ORCHESTRATION_LOCAL_TOOL_AGENT_HPP
#define CIPHER_ORCHESTRATION_LOCAL_TOOL_AGENT_HPP

#include "cipher/assistant_backend.hpp"
#include "cipher/orchestration/implementation_executor.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace cipher::orchestration {

// Local-model agent (Phase C, slice C1): one prompt per step through AssistantManager::submit (so
// validate_assistant_request runs, local_only = true), reply parsed by parse_agent_reply. Only proposes; never
// executes. Throws std::runtime_error on a non-Ok status, a malformed reply, an over-budget prompt or a repeated
// call; the executor maps that to ImplementationFailed. No retry here. Wired into ImplementationRig by orchestration_agent=local.
class LocalToolAgent final : public ToolAgent {
public:
    LocalToolAgent(IAssistantBackend& backend, double timeout_seconds, std::size_t max_prompt_chars);
    AgentStep next(const std::string& task, const std::vector<ToolTurn>& transcript) override;

private:
    IAssistantBackend& backend_;
    double timeout_seconds_;
    std::size_t max_prompt_chars_;
};

}  // namespace cipher::orchestration

#endif  // CIPHER_ORCHESTRATION_LOCAL_TOOL_AGENT_HPP
