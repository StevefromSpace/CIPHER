#ifndef CIPHER_FILESYSTEM_TOOLS_HPP
#define CIPHER_FILESYSTEM_TOOLS_HPP

#include "cipher/tool_registry.hpp"

#include <string>

namespace cipher {

/**
 * @brief Registers `filesystem.read`, a read-only tool limited to one allowlisted root.
 *
 * The argument is a relative path under @p root. The policy target is the canonical
 * path, so a `..` component, an absolute path, a symlink or a junction anywhere on the
 * path is denied before the policy or the handler runs. Files larger than a fixed
 * size bound are refused.
 */
void register_filesystem_tools(ToolRegistry& registry, const std::string& root);

} // namespace cipher

#endif // CIPHER_FILESYSTEM_TOOLS_HPP
