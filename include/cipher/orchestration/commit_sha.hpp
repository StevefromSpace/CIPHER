#ifndef CIPHER_ORCHESTRATION_COMMIT_SHA_HPP
#define CIPHER_ORCHESTRATION_COMMIT_SHA_HPP

#include <string>

namespace cipher::orchestration {

// A full 40-character lowercase hex commit id. Short ids are rejected: they can be ambiguous and are never trusted
// as a head or base identity.
inline bool is_full_sha(const std::string& sha) {
    if (sha.size() != 40) return false;
    for (const char c : sha) {
        const bool lower_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!lower_hex) return false;
    }
    return true;
}

}  // namespace cipher::orchestration

#endif  // CIPHER_ORCHESTRATION_COMMIT_SHA_HPP
