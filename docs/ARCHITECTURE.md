# Architecture

## v0.2 logical layout

```text
User
  |
  v
Terminal interface (src/main.cpp)
  |
  +-- loads Config via ConfigParser
  |
  +-- initialises Logger using Config
  |
  v
CIPHER Core (include/cipher/cipher_core.hpp, src/cipher_core.cpp)
  |
  +-- command parsing and responses
  |   (no dependency on Config or Logger)

Config subsystem (include/cipher/config.hpp, src/config.cpp)
  +-- reads cipher.conf (optional key=value file)
  +-- returns Config struct or list of errors
  +-- no dependency on Core or Logger

Logger (include/cipher/logger.hpp, src/logger.cpp)
  +-- writes timestamped lines to stderr and optional file
  +-- uses LogLevel from config.hpp
  +-- no dependency on Core

Tests (tests/) exercise each component independently.
```

## Dependency diagram

```text
main.cpp
  |-- cipher_core   (commands)
  |-- cipher_config (configuration)
  |-- cipher_logger (logging, depends on cipher_config for LogLevel)

test_cipher_core.cpp  --> cipher_core   only
test_config.cpp       --> cipher_config only
test_logger.cpp       --> cipher_logger (and transitively cipher_config)
```

Neither `cipher_config` nor `cipher_logger` depends on `cipher_core`.
`cipher_core` does not depend on Config or Logger.
This keeps each component testable and replaceable in isolation.

## Boundaries to preserve as CIPHER grows

```text
Interfaces (terminal / web / voice)
              |
              v
        CIPHER Core API
              |
       +------+------+
       |             |
    Planner      Permission Gate
       |             |
       +------ Tool Registry ------+
              |                    |
        Local tools          Optional adapters
              |
      Storage / Memory
              |
   Replaceable AI Runtime
              |
      Local model files
```

This is a target direction, not an implemented diagram. Components should communicate through small documented interfaces rather than tightly depending on each other's internals.

## Architectural rules

- Keep UI code out of the core.
- Keep model inference behind a replaceable interface.
- Do not make external AI services mandatory for startup or basic operation.
- Keep file, shell, browser, and network tools disabled until permissions and logging exist.
- Add tests with each meaningful capability.
- Store user data in documented, exportable formats.
- Separate the mechanism that approves updates from the code being updated.
- Config and Logger are cross-cutting services, but they must not create circular dependencies.

## v0.2 component details

### ConfigParser

Stateless helper.  Reads a `key = value` text file line by line.
Supported keys: `log_level`, `log_file`.
Unknown keys are rejected (not silently ignored) to surface typos.
Invalid values produce a clear error with a line number.
Returns `ConfigResult` — either a valid `Config` or a list of `ConfigError` items.
A missing file returns default values and no errors (the file is optional).
File location: `cipher.conf` is resolved strictly relative to the process's
current working directory (the directory where CIPHER was launched). It does not
search parent directories, system configuration paths, or the executable location.

### Logger

Owns an optional `std::ofstream`.  Opens it once in append mode at construction.
Writes every message at or above `minimum_level` to stderr and, if open, to the file.
Uses the same platform-safe `localtime_s` / `localtime_r` pattern as `cipher_core.cpp`.
If the log file cannot be opened, the Logger reports the problem to stderr and
continues with stderr-only output — it does not crash the application.
Not thread-safe (CIPHER is currently single-threaded).

### Versioning

The single source of truth for the application version is declared in `CMakeLists.txt`
via `project(CIPHER VERSION ...)`.  CMake generates `version.hpp` into the build tree
from `include/cipher/version.hpp.in` via `configure_file()`.  `CipherCore::version()`
and diagnostic outputs consume these constants (`CIPHER_VERSION`, `CIPHER_VERSION_MAJOR`,
etc.), guaranteeing that the startup banner, command outputs, and logs remain synchronized.

## Current limitations

The v0.2 implementation is a single-process command-line program with configuration
and diagnostic logging.  It has no database, networking, AI inference, long-term
memory, authentication, sandbox, or system-control tools.  That is deliberate scope
control, not hidden complexity.

Not implemented yet (planned for future milestones):
- Persistent memory (v0.3)
- Replaceable AI runtime interface (v0.4)
- Permissioned tool registry (v0.5)
- Voice and alternative interfaces (later)
