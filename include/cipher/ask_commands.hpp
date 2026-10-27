#ifndef CIPHER_ASK_COMMANDS_HPP
#define CIPHER_ASK_COMMANDS_HPP

#include "cipher/assistant_backend.hpp"

#include <string>

namespace cipher {

class ModelToolLoop;

/**
 * @brief Result of offering one REPL line to AskCommands. Same contract as the other command
 *        classes: when handled is false, the line belongs to the next handler.
 */
struct AskCommandResult {
    bool handled{false};
    std::string text;
};

/**
 * @brief Owner-typed `ask <prompt>` REPL command that sends one prompt to a local backend.
 *
 * Every request is local_only, so AssistantManager::submit fails closed unless the backend
 * reports VerifiedLocal. Without a tool loop, tools are never requested and no memory is attached:
 * the prompt is only the text the owner typed. With a tool loop, the question runs through
 * ModelToolLoop, which lets the model propose tools that the owner must confirm.
 */
class AskCommands {
public:
    /// A null backend disables `ask`, and the command says how to enable it.
    /// A non-null tool_loop routes the question through it. The loop must wrap the same backend.
    explicit AskCommands(IAssistantBackend* backend, double timeout_seconds = 120.0, ModelToolLoop* tool_loop = nullptr);

    AskCommandResult execute(const std::string& input);

private:
    IAssistantBackend* backend_;
    double timeout_seconds_;
    ModelToolLoop* tool_loop_;
};

} // namespace cipher

#endif // CIPHER_ASK_COMMANDS_HPP
