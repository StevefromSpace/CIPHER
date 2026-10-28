#ifndef CIPHER_LLAMA_BACKEND_HPP
#define CIPHER_LLAMA_BACKEND_HPP

#include "cipher/assistant_backend.hpp"

#include <chrono>
#include <memory>
#include <string>

// llama.cpp types stay out of CIPHER headers; only llama_backend.cpp includes llama.h.
struct llama_model;
struct llama_context;

namespace cipher {

/**
 * @brief Settings for the in-process llama.cpp backend.
 *
 * The owner supplies model_path. CIPHER never downloads or picks a model.
 */
struct LlamaCppConfig {
    std::string model_path;
    int context_tokens{4096};    // prompt + answer must fit in this window
    int max_output_tokens{512};  // generation stops here even without an end token
    float temperature{0.8f};     // <= 0 selects greedy (deterministic) decoding
};

/**
 * @brief Assistant backend that runs a local GGUF model in this process via llama.cpp.
 *
 * The model loads once, in the constructor. A load failure leaves the backend unavailable with
 * load_error() explaining why; requests then fail with AssistantStatus::Unavailable.
 * Each request starts from an empty context, so nothing from one `ask` reaches the next.
 * Not thread-safe: one request at a time, like the REPL that drives it.
 */
class LlamaCppBackend : public IAssistantBackend {
public:
    explicit LlamaCppBackend(LlamaCppConfig config);
    ~LlamaCppBackend() override;

    LlamaCppBackend(const LlamaCppBackend&) = delete;
    LlamaCppBackend& operator=(const LlamaCppBackend&) = delete;

    std::string backend_id() const override { return "llama.cpp"; }
    bool is_available() override { return context_ != nullptr; }
    AssistantCapabilities capabilities() const override;
    AssistantResponse execute_request(const AssistantRequest& request) override;

    /** Why the model did not load; empty when it did. */
    const std::string& load_error() const { return load_error_; }

private:
    struct ModelDeleter { void operator()(llama_model* model) const; };
    struct ContextDeleter { void operator()(llama_context* context) const; };

    AssistantResponse generate(const std::string& prompt);

    LlamaCppConfig config_;
    std::string load_error_;
    std::unique_ptr<llama_model, ModelDeleter> model_;
    // Declared after model_ so it is destroyed first: a context must not outlive its model.
    std::unique_ptr<llama_context, ContextDeleter> context_;
    // Per-request deadline; llama.cpp's abort callback reads it during long decodes.
    std::chrono::steady_clock::time_point deadline_{};
};

} // namespace cipher

#endif // CIPHER_LLAMA_BACKEND_HPP
