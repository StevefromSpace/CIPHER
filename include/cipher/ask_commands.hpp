#ifndef CIPHER_ASK_COMMANDS_HPP
#define CIPHER_ASK_COMMANDS_HPP

#include "cipher/assistant_backend.hpp"

#include <string>

namespace cipher {

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
 * reports VerifiedLocal. Tools are never requested and no memory is attached: the prompt is
 * only the text the owner typed. The reply is printed, never executed.
 */
class AskCommands {
public:
    /// A null backend disables `ask`, and the command says how to enable it.
    explicit AskCommands(IAssistantBackend* backend, double timeout_seconds = 120.0);

    AskCommandResult execute(const std::string& input);

private:
    IAssistantBackend* backend_;
    double timeout_seconds_;
};

} // namespace cipher

#endif // CIPHER_ASK_COMMANDS_HPP
