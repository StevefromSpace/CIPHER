#ifndef CIPHER_FILESYSTEM_TOOLS_HPP
#define CIPHER_FILESYSTEM_TOOLS_HPP

#include "cipher/tool_registry.hpp"

#include <string>
#include <vector>

namespace cipher {

/**
 * @brief Registers `filesystem.read`, a read-only tool limited to one allowlisted root.
 *
 * The argument is a relative path under @p root. The policy target is the canonical
 * path, so a `..` component, an absolute path, a symlink or a junction anywhere on the
 * path is denied before the policy or the handler runs. Files larger than a fixed
 * size bound are refused.
 *
 * @p private_files lists CIPHER's own memory, log and configuration files. A file that is
 * the same file as one of them (by file identity, so case and hard links do not help) is
 * denied even when it sits under @p root, so this tool cannot bypass memory_deny.
 */
void register_filesystem_tools(ToolRegistry& registry, const std::string& root,
                               std::vector<std::string> private_files = {});

} // namespace cipher

#endif // CIPHER_FILESYSTEM_TOOLS_HPP
