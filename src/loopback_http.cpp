#include "cipher/loopback_http.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace cipher {

namespace {

constexpr const char* kLoopbackAddress = "127.0.0.1";
constexpr std::size_t kReadChunkBytes = 8192;

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

void close_socket(SocketHandle sock) { closesocket(sock); }

bool is_timeout_error() { return WSAGetLastError() == WSAETIMEDOUT; }

// Winsock must be initialized once per process before any socket call.
bool ensure_winsock() {
    static const bool initialized = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
}

// shortcut: the timeout applies to each send/recv, not the whole request, so a slow trickle can exceed it; upgrade to a total deadline before any non-owner caller.
void set_socket_timeouts(SocketHandle sock, double timeout_seconds) {
    const DWORD millis = static_cast<DWORD>(timeout_seconds * 1000.0);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&millis), sizeof(millis));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&millis), sizeof(millis));
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

void close_socket(SocketHandle sock) { close(sock); }

bool is_timeout_error() { return errno == EAGAIN || errno == EWOULDBLOCK; }

bool ensure_winsock() { return true; }

// shortcut: the timeout applies to each send/recv, not the whole request, so a slow trickle can exceed it; upgrade to a total deadline before any non-owner caller.
void set_socket_timeouts(SocketHandle sock, double timeout_seconds) {
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout_seconds);
    tv.tv_usec = static_cast<long>((timeout_seconds - static_cast<double>(tv.tv_sec)) * 1000000.0);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
#endif

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

HttpResult make_failure(HttpOutcome outcome, const std::string& detail) {
    HttpResult result;
    result.outcome = outcome;
    result.detail = detail;
    return result;
}

// Decodes a "Transfer-Encoding: chunked" body. Returns false if the framing is invalid.
bool decode_chunked(const std::string& raw, std::string& decoded) {
    std::size_t pos = 0;
    while (true) {
        const auto line_end = raw.find("\r\n", pos);
        if (line_end == std::string::npos) {
            return false;
        }
        const std::string size_line = raw.substr(pos, line_end - pos);
        char* end = nullptr;
        const unsigned long chunk_size = std::strtoul(size_line.c_str(), &end, 16);
        if (end == size_line.c_str()) {
            return false;
        }
        pos = line_end + 2;
        if (chunk_size == 0) {
            return true;
        }
        // Compare against the remaining bytes so a huge declared size cannot overflow the index math.
        const std::size_t remaining = raw.size() - pos;
        if (remaining < 2 || chunk_size > remaining - 2 || raw.compare(pos + chunk_size, 2, "\r\n") != 0) {
            return false;
        }
        decoded.append(raw, pos, chunk_size);
        pos += chunk_size + 2;
    }
}

} // anonymous namespace

HttpResult parse_http_response(const std::string& raw) {
    const auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return make_failure(HttpOutcome::MalformedResponse, "Response has no header terminator.");
    }

    const auto status_line_end = raw.find("\r\n");
    const std::string status_line = raw.substr(0, status_line_end);
    if (status_line.compare(0, 7, "HTTP/1.") != 0 || status_line.size() < 12) {
        return make_failure(HttpOutcome::MalformedResponse, "Response status line is not HTTP/1.x.");
    }

    HttpResult result;
    result.outcome = HttpOutcome::Ok;
    result.status_code = std::atoi(status_line.substr(9, 3).c_str());
    if (result.status_code < 100 || result.status_code > 599) {
        return make_failure(HttpOutcome::MalformedResponse, "Response status code is out of range.");
    }

    bool chunked = false;
    bool has_length = false;
    std::size_t content_length = 0;
    std::size_t line_start = status_line_end + 2;
    while (line_start < header_end) {
        const auto line_end = raw.find("\r\n", line_start);
        const std::string line = raw.substr(line_start, line_end - line_start);
        line_start = line_end + 2;
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string name = lowercase(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        if (name == "transfer-encoding" && lowercase(value).find("chunked") != std::string::npos) {
            chunked = true;
        } else if (name == "content-length") {
            has_length = true;
            content_length = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
        }
    }

    const std::string body_raw = raw.substr(header_end + 4);
    if (chunked) {
        if (!decode_chunked(body_raw, result.body)) {
            return make_failure(HttpOutcome::MalformedResponse, "Chunked response framing is invalid.");
        }
    } else {
        result.body = body_raw;
    }
    if (has_length && !chunked && body_raw.size() != content_length) {
        return make_failure(HttpOutcome::MalformedResponse, "Response body length does not match Content-Length.");
    }
    return result;
}

HttpResult loopback_http_request(
    int port,
    const std::string& method,
    const std::string& path,
    const std::string& body,
    double timeout_seconds
) {
    if (!ensure_winsock()) {
        return make_failure(HttpOutcome::TransportFailed, "Socket subsystem failed to initialize.");
    }

    const SocketHandle sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == kInvalidSocket) {
        return make_failure(HttpOutcome::TransportFailed, "Could not create socket.");
    }
    // shortcut: connect uses the OS default timeout; loopback refusal returns immediately, upgrade if remote hosts are ever allowed.
    set_socket_timeouts(sock, timeout_seconds);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    inet_pton(AF_INET, kLoopbackAddress, &address.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_socket(sock);
        return make_failure(HttpOutcome::ConnectFailed, "No local server accepted the connection.");
    }

    std::ostringstream request;
    request << method << ' ' << path << " HTTP/1.1\r\n"
            << "Host: " << kLoopbackAddress << ':' << port << "\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << body;
    const std::string payload = request.str();

    std::size_t sent = 0;
    while (sent < payload.size()) {
        const int written = send(sock, payload.data() + sent, static_cast<int>(payload.size() - sent), 0);
        if (written <= 0) {
            const bool timed_out = is_timeout_error();
            close_socket(sock);
            return make_failure(timed_out ? HttpOutcome::TimedOut : HttpOutcome::TransportFailed,
                                "Failed while sending request.");
        }
        sent += static_cast<std::size_t>(written);
    }

    std::string raw;
    std::vector<char> chunk(kReadChunkBytes);
    while (true) {
        const int received = recv(sock, chunk.data(), static_cast<int>(chunk.size()), 0);
        if (received == 0) {
            break;
        }
        if (received < 0) {
            const bool timed_out = is_timeout_error();
            close_socket(sock);
            return make_failure(timed_out ? HttpOutcome::TimedOut : HttpOutcome::TransportFailed,
                                "Failed while receiving response.");
        }
        raw.append(chunk.data(), static_cast<std::size_t>(received));
        if (raw.size() > kMaxHttpResponseBytes) {
            close_socket(sock);
            return make_failure(HttpOutcome::ResponseTooLarge, "Response exceeds the configured size limit.");
        }
    }
    close_socket(sock);

    return parse_http_response(raw);
}

} // namespace cipher
