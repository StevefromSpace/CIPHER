#ifndef CIPHER_LOOPBACK_HTTP_HPP
#define CIPHER_LOOPBACK_HTTP_HPP

#include <cstddef>
#include <string>

namespace cipher {

/**
 * @brief Transport-level outcome of one loopback HTTP exchange.
 */
enum class HttpOutcome {
    Ok,                 // A complete HTTP response was received and parsed.
    ConnectFailed,      // Nothing accepted the connection on 127.0.0.1.
    TimedOut,           // The socket timed out before the response completed.
    TransportFailed,    // Other socket failure.
    MalformedResponse,  // Bytes were received but are not a valid HTTP/1.x response.
    ResponseTooLarge    // The response exceeded kMaxHttpResponseBytes.
};

struct HttpResult {
    HttpOutcome outcome{HttpOutcome::TransportFailed};
    int status_code{0};
    std::string body;
    std::string detail;
};

/** Upper bound on any response body read from a local model server. */
inline constexpr std::size_t kMaxHttpResponseBytes = 4 * 1024 * 1024;

/**
 * @brief Sends one HTTP/1.1 request to 127.0.0.1:port and returns the parsed response.
 *
 * The host is a fixed IPv4 loopback literal, so the peer is loopback by construction.
 * Each request uses a fresh connection with "Connection: close".
 */
HttpResult loopback_http_request(
    int port,
    const std::string& method,
    const std::string& path,
    const std::string& body,
    double timeout_seconds
);

/**
 * @brief Parses a complete raw HTTP/1.x response (status line, headers, body).
 *
 * Supports Content-Length-less bodies read to close and chunked transfer coding.
 */
HttpResult parse_http_response(const std::string& raw);

} // namespace cipher

#endif // CIPHER_LOOPBACK_HTTP_HPP
