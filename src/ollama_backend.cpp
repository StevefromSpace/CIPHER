#include "cipher/ollama_backend.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

namespace cipher {

namespace {

constexpr const char* kLoopbackHost = "127.0.0.1";
constexpr int kMaxPort = 65535;
constexpr double kAvailabilityProbeSeconds = 2.0;
constexpr const char* kVersionPath = "/api/version";
constexpr const char* kGeneratePath = "/api/generate";

// -----------------------------------------------------------------------------
// Minimal JSON helpers (no dependency; only the shapes Ollama uses)
// -----------------------------------------------------------------------------

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += raw;
                }
        }
    }
    return out;
}

void append_utf8(std::string& out, unsigned int code_point) {
    if (code_point < 0x80) {
        out += static_cast<char>(code_point);
    } else if (code_point < 0x800) {
        out += static_cast<char>(0xC0 | (code_point >> 6));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else if (code_point < 0x10000) {
        out += static_cast<char>(0xE0 | (code_point >> 12));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code_point >> 18));
        out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    }
}

// Reads 4 hex digits at pos; returns false if they are not all hex.
bool read_hex4(const std::string& text, std::size_t pos, unsigned int& value) {
    if (pos + 4 > text.size()) {
        return false;
    }
    value = 0;
    for (std::size_t i = pos; i < pos + 4; ++i) {
        const char c = text[i];
        value <<= 4;
        if (c >= '0' && c <= '9') value |= static_cast<unsigned int>(c - '0');
        else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned int>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned int>(c - 'A' + 10);
        else return false;
    }
    return true;
}

// Decodes a JSON string whose opening quote is at pos. On success pos is one past the closing quote.
bool read_json_string(const std::string& text, std::size_t& pos, std::string& out) {
    if (pos >= text.size() || text[pos] != '"') {
        return false;
    }
    ++pos;
    out.clear();
    while (pos < text.size()) {
        const char c = text[pos++];
        if (c == '"') {
            return true;
        }
        if (c != '\\') {
            out += c;
            continue;
        }
        if (pos >= text.size()) {
            return false;
        }
        const char esc = text[pos++];
        switch (esc) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned int code_point = 0;
                if (!read_hex4(text, pos, code_point)) {
                    return false;
                }
                pos += 4;
                // Combine a UTF-16 surrogate pair into one code point.
                if (code_point >= 0xD800 && code_point <= 0xDBFF && pos + 6 <= text.size() &&
                    text[pos] == '\\' && text[pos + 1] == 'u') {
                    unsigned int low = 0;
                    if (read_hex4(text, pos + 2, low) && low >= 0xDC00 && low <= 0xDFFF) {
                        code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
                        pos += 6;
                    }
                }
                append_utf8(out, code_point);
                break;
            }
            default:
                return false;
        }
    }
    return false;
}

void skip_whitespace(const std::string& text, std::size_t& pos) {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n')) {
        ++pos;
    }
}

// Skips one JSON value of any kind. Returns false if the value is unterminated.
bool skip_json_value(const std::string& text, std::size_t& pos) {
    if (pos >= text.size()) {
        return false;
    }
    if (text[pos] == '"') {
        std::string ignored;
        return read_json_string(text, pos, ignored);
    }
    if (text[pos] == '{' || text[pos] == '[') {
        int depth = 0;
        while (pos < text.size()) {
            const char c = text[pos];
            if (c == '"') {
                std::string ignored;
                if (!read_json_string(text, pos, ignored)) {
                    return false;
                }
                continue;
            }
            ++pos;
            if (c == '{' || c == '[') {
                ++depth;
            } else if (c == '}' || c == ']') {
                if (--depth == 0) {
                    return true;
                }
            }
        }
        return false;
    }
    while (pos < text.size() && text[pos] != ',' && text[pos] != '}' && text[pos] != ']') {
        ++pos;
    }
    return true;
}

enum class FieldLookup { Found, Missing, Malformed };

// Looks up a string field among the top-level keys of a JSON object body.
FieldLookup find_top_level_string(const std::string& body, const std::string& key, std::string& value) {
    std::size_t pos = 0;
    skip_whitespace(body, pos);
    if (pos >= body.size() || body[pos] != '{') {
        return FieldLookup::Malformed;
    }
    ++pos;
    while (true) {
        skip_whitespace(body, pos);
        if (pos < body.size() && body[pos] == '}') {
            return FieldLookup::Missing;
        }
        std::string current_key;
        if (!read_json_string(body, pos, current_key)) {
            return FieldLookup::Malformed;
        }
        skip_whitespace(body, pos);
        if (pos >= body.size() || body[pos] != ':') {
            return FieldLookup::Malformed;
        }
        ++pos;
        skip_whitespace(body, pos);
        if (current_key == key && pos < body.size() && body[pos] == '"') {
            return read_json_string(body, pos, value) ? FieldLookup::Found : FieldLookup::Malformed;
        }
        if (!skip_json_value(body, pos)) {
            return FieldLookup::Malformed;
        }
        skip_whitespace(body, pos);
        if (pos < body.size() && body[pos] == ',') {
            ++pos;
        }
    }
}

std::string build_generate_body(const std::string& model, const std::string& prompt) {
    return "{\"model\":\"" + json_escape(model) + "\",\"prompt\":\"" + json_escape(prompt) +
           "\",\"stream\":false}";
}

AssistantResponse make_failure_response(AssistantStatus status, const std::string& message) {
    AssistantResponse resp;
    resp.status = status;
    resp.error = message;
    return resp;
}

AssistantStatus status_for_transport(HttpOutcome outcome) {
    switch (outcome) {
        case HttpOutcome::ConnectFailed: return AssistantStatus::ConnectionError;
        case HttpOutcome::TimedOut: return AssistantStatus::Timeout;
        case HttpOutcome::TransportFailed: return AssistantStatus::ConnectionError;
        case HttpOutcome::MalformedResponse:
        case HttpOutcome::ResponseTooLarge:
        case HttpOutcome::Ok:
            return AssistantStatus::Error;
    }
    return AssistantStatus::Error;
}

} // anonymous namespace

OllamaBackend::OllamaBackend(OllamaConfig config, HttpTransport transport)
    : config_(std::move(config)),
      transport_(std::move(transport)),
      endpoint_verified_(is_loopback_host(config_.host) && config_.port >= 1 && config_.port <= kMaxPort) {}

bool OllamaBackend::is_loopback_host(const std::string& host) {
    // Only the literal address is accepted. "localhost" is refused because its resolution can be redirected by the hosts file.
    return host == kLoopbackHost;
}

bool OllamaBackend::is_available() {
    if (!endpoint_verified_) {
        return false;
    }
    const HttpResult probe = transport_(config_.port, "GET", kVersionPath, "", kAvailabilityProbeSeconds);
    return probe.outcome == HttpOutcome::Ok && probe.status_code == 200;
}

AssistantCapabilities OllamaBackend::capabilities() const {
    AssistantCapabilities caps;
    caps.supports_text_conversation = true;
    // Verified means the destination address is the loopback literal. It does not prove the listener is Ollama.
    caps.processing_route = endpoint_verified_ ? ProcessingRoute::VerifiedLocal : ProcessingRoute::Unknown;
    return caps;
}

AssistantResponse OllamaBackend::execute_request(const AssistantRequest& request) {
    if (!endpoint_verified_) {
        return make_failure_response(AssistantStatus::Unavailable,
                                     "Ollama endpoint is not verified loopback; refusing to send the request.");
    }

    const auto started = std::chrono::steady_clock::now();
    const std::string model = request.model_override.value_or(config_.model);
    const HttpResult http = transport_(config_.port, "POST", kGeneratePath,
                                       build_generate_body(model, request.prompt), request.timeout_seconds);

    AssistantResponse resp;
    resp.model = model;
    resp.route = capabilities().processing_route;
    resp.duration_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

    if (http.outcome != HttpOutcome::Ok) {
        resp.status = status_for_transport(http.outcome);
        resp.error = "Ollama request failed: " + http.detail;
        return resp;
    }

    std::string error_text;
    if (find_top_level_string(http.body, "error", error_text) == FieldLookup::Found) {
        resp.status = AssistantStatus::Error;
        resp.error = "Ollama returned HTTP " + std::to_string(http.status_code) + ": " + error_text;
        return resp;
    }
    if (http.status_code != 200) {
        resp.status = AssistantStatus::Error;
        resp.error = "Ollama returned HTTP " + std::to_string(http.status_code) + ".";
        return resp;
    }

    if (find_top_level_string(http.body, "response", resp.text) != FieldLookup::Found) {
        resp.status = AssistantStatus::Error;
        resp.text.clear();
        resp.error = "Ollama response is missing the 'response' field.";
        return resp;
    }

    resp.status = AssistantStatus::Ok;
    return resp;
}

} // namespace cipher
