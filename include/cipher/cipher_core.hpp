#ifndef CIPHER_CIPHER_CORE_HPP
#define CIPHER_CIPHER_CORE_HPP

#include "cipher/version.hpp"

#include <string>

namespace cipher {

struct CommandResponse {
    std::string text;
    bool exit_requested = false;
};

// The core contains application behavior and does not depend on a UI,
// model provider, network service, or database.
class CipherCore {
public:
    static constexpr const char* version() noexcept {
        return CIPHER_VERSION;
    }

    CommandResponse execute(const std::string& input) const;
};

// Drops a UTF-8 byte-order mark (editors and PowerShell pipes prepend one to the first line of input).
inline std::string strip_utf8_bom(const std::string& line) {
    return line.compare(0, 3, "\xEF\xBB\xBF") == 0 ? line.substr(3) : line;
}

} // namespace cipher

#endif // CIPHER_CIPHER_CORE_HPP
