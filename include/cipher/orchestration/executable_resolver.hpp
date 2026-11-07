#pragma once

#include <string>

namespace cipher::orchestration {

// Resolves a bare executable name to an absolute path by walking a PATH list. Empty and relative entries are skipped:
// a relative entry would let an agent-written file in a working directory stand in for a real tool. Windows tries
// NAME.exe when NAME has no extension; POSIX requires the execute bit. Throws std::runtime_error when nothing matches,
// so a missing gh, git or claude fails at construction.
std::string resolve_executable(const std::string& name, const std::string& path_list);

// Same as resolve_executable, reading the PATH environment variable.
std::string resolve_executable_on_path(const std::string& name);

}  // namespace cipher::orchestration
