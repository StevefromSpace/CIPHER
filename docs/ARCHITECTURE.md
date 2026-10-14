# Initial Architecture

## v0.1 logical layout

```text
User
  |
  v
Terminal interface (src/main.cpp)
  |
  v
CIPHER Core (include/cipher, src/cipher_core.cpp)
  |
  +-- command parsing and basic responses

Tests (tests/) exercise the core independently of the terminal.
```

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

## Current limitations

The current v0.1 implementation is a single-process command-line program. It has no database, networking, AI inference, long-term memory, authentication, sandbox, or system-control tools. That is deliberate scope control, not unfinished magic hidden somewhere else.
