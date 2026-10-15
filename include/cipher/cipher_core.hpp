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

} // namespace cipher

#endif // CIPHER_CIPHER_CORE_HPP
