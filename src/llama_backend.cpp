#include "cipher/llama_backend.hpp"

#include <llama.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace cipher {

namespace {

constexpr std::size_t kPieceBufferBytes = 256;

// llama.cpp logs every load step at info level. CIPHER keeps the terminal quiet and shows errors only.
void log_errors_only(ggml_log_level level, const char* text, void* /*user_data*/) {
    if (level == GGML_LOG_LEVEL_ERROR) {
        std::fputs(text, stderr);
    }
}

void init_llama_once() {
    static std::once_flag once;
    std::call_once(once, [] {
        llama_log_set(log_errors_only, nullptr);
        llama_backend_init();
    });
}

AssistantResponse make_failure_response(AssistantStatus status, const std::string& message) {
    AssistantResponse resp;
    resp.status = status;
    resp.error = message;
    return resp;
}

// Wraps the prompt in the model's own chat template. Models without a supported template get the raw prompt.
std::string apply_chat_template(const llama_model* model, const std::string& prompt) {
    const char* tmpl = llama_model_chat_template(model, nullptr);
    if (tmpl == nullptr) {
        return prompt;
    }
    const llama_chat_message message{"user", prompt.c_str()};
    std::vector<char> buffer(prompt.size() * 2 + 512);
    int32_t written = llama_chat_apply_template(tmpl, &message, 1, true, buffer.data(),
                                                static_cast<int32_t>(buffer.size()));
    if (written > static_cast<int32_t>(buffer.size())) {
        buffer.resize(static_cast<std::size_t>(written));
        written = llama_chat_apply_template(tmpl, &message, 1, true, buffer.data(),
                                            static_cast<int32_t>(buffer.size()));
    }
    if (written < 0) {
        return prompt;
    }
    return std::string(buffer.data(), static_cast<std::size_t>(written));
}

// Returns false when the text cannot be tokenized (for example an int32 overflow).
bool tokenize(const llama_vocab* vocab, const std::string& text, std::vector<llama_token>& tokens) {
    const auto length = static_cast<int32_t>(text.size());
    // A negative result is the required token count.
    const int32_t needed = -llama_tokenize(vocab, text.c_str(), length, nullptr, 0, true, true);
    if (needed <= 0) {
        return false;
    }
    tokens.resize(static_cast<std::size_t>(needed));
    return llama_tokenize(vocab, text.c_str(), length, tokens.data(), needed, true, true) == needed;
}

std::string token_text(const llama_vocab* vocab, llama_token token) {
    std::string piece(kPieceBufferBytes, '\0');
    int32_t n = llama_token_to_piece(vocab, token, piece.data(), static_cast<int32_t>(piece.size()), 0, false);
    if (n < 0) {
        piece.resize(static_cast<std::size_t>(-n));
        n = llama_token_to_piece(vocab, token, piece.data(), static_cast<int32_t>(piece.size()), 0, false);
    }
    piece.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    return piece;
}

struct SamplerDeleter {
    void operator()(llama_sampler* sampler) const { llama_sampler_free(sampler); }
};
using SamplerPtr = std::unique_ptr<llama_sampler, SamplerDeleter>;

SamplerPtr make_sampler(float temperature) {
    SamplerPtr chain(llama_sampler_chain_init(llama_sampler_chain_default_params()));
    if (temperature <= 0.0f) {
        llama_sampler_chain_add(chain.get(), llama_sampler_init_greedy());
        return chain;
    }
    llama_sampler_chain_add(chain.get(), llama_sampler_init_min_p(0.05f, 1));
    llama_sampler_chain_add(chain.get(), llama_sampler_init_temp(temperature));
    llama_sampler_chain_add(chain.get(), llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
    return chain;
}

bool deadline_passed(void* data) {
    return std::chrono::steady_clock::now() >= *static_cast<std::chrono::steady_clock::time_point*>(data);
}

// Checked before llama.cpp sees the path, so the owner gets a precise reason instead of a generic load failure.
std::string check_model_file(const std::string& path) {
    if (path.empty()) {
        return "No model path is set. Set llama_model_path in cipher.conf to a local .gguf file.";
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return "Model file '" + path + "' does not exist.";
    }
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return "Model path '" + path + "' is not a regular file.";
    }
    return "";
}

} // anonymous namespace

void LlamaCppBackend::ModelDeleter::operator()(llama_model* model) const { llama_model_free(model); }
void LlamaCppBackend::ContextDeleter::operator()(llama_context* context) const { llama_free(context); }

LlamaCppBackend::LlamaCppBackend(LlamaCppConfig config) : config_(std::move(config)) {
    // cipher.conf already checks each value; this guards the cross-key rule and direct callers.
    if (config_.context_tokens < 1 || config_.max_output_tokens < 1 ||
        config_.max_output_tokens >= config_.context_tokens ||
        !std::isfinite(config_.temperature) || config_.temperature < 0.0f) {
        load_error_ = "Invalid llama.cpp settings: need 0 < llama_max_output_tokens < llama_context_tokens "
                      "and a finite llama_temperature >= 0.";
        return;
    }
    load_error_ = check_model_file(config_.model_path);
    if (!load_error_.empty()) {
        return;
    }

    init_llama_once();
    model_.reset(llama_model_load_from_file(config_.model_path.c_str(), llama_model_default_params()));
    if (!model_) {
        load_error_ = "llama.cpp could not load '" + config_.model_path + "'. Check that it is a valid GGUF model.";
        return;
    }

    llama_context_params params = llama_context_default_params();
    params.n_ctx = static_cast<uint32_t>(config_.context_tokens);
    params.n_batch = params.n_ctx;  // the whole prompt is decoded in one batch
    params.abort_callback = deadline_passed;
    params.abort_callback_data = &deadline_;
    params.no_perf = true;
    context_.reset(llama_init_from_model(model_.get(), params));
    if (!context_) {
        load_error_ = "llama.cpp loaded the model but could not create a context of " +
                      std::to_string(config_.context_tokens) + " tokens.";
        model_.reset();
    }
}

LlamaCppBackend::~LlamaCppBackend() = default;

AssistantCapabilities LlamaCppBackend::capabilities() const {
    AssistantCapabilities caps;
    caps.supports_text_conversation = true;
    // Inference runs in this process; the build disables llama.cpp's RPC and HTTP code, so nothing leaves the host.
    caps.processing_route = context_ ? ProcessingRoute::VerifiedLocal : ProcessingRoute::Unknown;
    return caps;
}

AssistantResponse LlamaCppBackend::execute_request(const AssistantRequest& request) {
    if (!context_) {
        return make_failure_response(AssistantStatus::Unavailable, "llama.cpp backend is unavailable: " + load_error_);
    }
    if (request.model_override) {
        return make_failure_response(AssistantStatus::Error,
                                     "The llama.cpp backend serves one configured model; model_override is not supported.");
    }

    const auto started = std::chrono::steady_clock::now();
    deadline_ = started + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(request.timeout_seconds));

    AssistantResponse resp = generate(request.prompt);
    resp.model = std::filesystem::path(config_.model_path).filename().string();
    resp.route = capabilities().processing_route;
    resp.duration_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return resp;
}

AssistantResponse LlamaCppBackend::generate(const std::string& prompt) {
    // A fresh context per request: earlier prompts (including shared memory) never leak into this one.
    llama_memory_clear(llama_get_memory(context_.get()), true);

    const llama_vocab* vocab = llama_model_get_vocab(model_.get());
    std::vector<llama_token> tokens;
    if (!tokenize(vocab, apply_chat_template(model_.get(), prompt), tokens)) {
        return make_failure_response(AssistantStatus::Error, "llama.cpp could not tokenize the prompt.");
    }
    const auto window = static_cast<std::size_t>(config_.context_tokens - config_.max_output_tokens);
    if (tokens.size() > window) {
        return make_failure_response(AssistantStatus::Error,
                                     "Prompt is " + std::to_string(tokens.size()) + " tokens; this model setup allows " +
                                         std::to_string(window) + ".");
    }

    SamplerPtr sampler = make_sampler(config_.temperature);
    std::string text;
    llama_batch batch = llama_batch_get_one(tokens.data(), static_cast<int32_t>(tokens.size()));
    llama_token next = 0;
    for (int produced = 0; produced < config_.max_output_tokens; ++produced) {
        const int32_t rc = llama_decode(context_.get(), batch);
        if (rc == 2 || std::chrono::steady_clock::now() >= deadline_) {
            return make_failure_response(AssistantStatus::Timeout, "llama.cpp generation exceeded the time limit.");
        }
        if (rc != 0) {
            return make_failure_response(AssistantStatus::Error,
                                         "llama.cpp decode failed (code " + std::to_string(rc) + ").");
        }
        next = llama_sampler_sample(sampler.get(), context_.get(), -1);
        if (llama_vocab_is_eog(vocab, next)) {
            break;
        }
        text += token_text(vocab, next);
        batch = llama_batch_get_one(&next, 1);
    }

    AssistantResponse resp;
    resp.status = AssistantStatus::Ok;
    resp.text = std::move(text);
    return resp;
}

} // namespace cipher
