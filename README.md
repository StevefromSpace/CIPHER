# CIPHER

**CIPHER** stands for **Contextual Intelligence Platform for Home, Enterprise & Research**.

CIPHER is a long-term, local-first assistant platform intended to grow through small, testable milestones. The core architecture is designed to remain under our control and not require any single AI company, cloud service, model, or framework to operate.

> Build it so that no single company, model, or generation of technology can determine its future.

## Current milestone: v0.2.0

This release adds configuration and diagnostics to the v0.1 foundation. CIPHER can now be configured with a simple text file and logs meaningful startup and shutdown events. It is still not an AI assistant — that comes in a later milestone.

New in v0.2:

- An optional `cipher.conf` configuration file (key = value format).
- A structured logger with four levels: `debug`, `info`, `warn`, `error`.
- Log output to stderr (always) and an optional log file (configurable).
- Clear error messages when the configuration file contains mistakes.
- Sensible defaults when no configuration file is present.

What is still not implemented: AI runtime, persistent memory, voice, computer control, autonomous planning, network access.

## Configuration

CIPHER reads `cipher.conf` from the directory where it is launched. The file is optional — if it is absent, defaults are used and startup continues normally.

Copy the example file to get started:

```
# Windows
copy cipher.conf.example cipher.conf

# Linux / macOS
cp cipher.conf.example cipher.conf
```

Then edit `cipher.conf`. Available settings:

| Key         | Values                          | Default | Description                              |
|-------------|---------------------------------|---------|------------------------------------------|
| `log_level` | `debug`, `info`, `warn`, `error`| `info`  | Minimum severity level to log            |
| `log_file`  | any file path, or empty         | (empty) | Optional log file; empty = stderr only   |

**Example cipher.conf:**
```
# Show all messages including internal state changes
log_level = debug

# Write log messages to a file as well as stderr
log_file = cipher.log
```

**Rules:**
- Lines beginning with `#` are comments.
- Blank lines are ignored.
- Unknown keys are rejected with an error message — fix the typo and restart.
- Configuration errors stop startup; CIPHER does not silently fall back.
- `cipher.conf` is excluded from Git (see `.gitignore`). `cipher.conf.example` is tracked.
- **Never store passwords, API keys, or tokens in `cipher.conf`.**

## Logging

Log lines are written to stderr by default. Format:

```
2026-10-15 13:00:00 [INFO ] CIPHER v0.2.0 starting
2026-10-15 13:00:00 [DEBUG] Configuration loaded (log_level=debug)
2026-10-15 13:01:23 [INFO ] CIPHER session ended (user request)
2026-10-15 13:01:23 [INFO ] CIPHER shutdown complete
```

To also write to a file, set `log_file` in `cipher.conf`. CIPHER appends to the file on every run; rotate or delete it manually when it grows large.

## Build

Required: a C++17 compiler and CMake 3.16 or newer. No third-party libraries or package managers are needed.

### Windows (PowerShell)

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Run the program (with Ninja generator the executable is directly in `build`):

```powershell
.\build\cipher.exe
```

With a Visual Studio generator it may be under `build\Release\`:

```powershell
.\build\Release\cipher.exe
```

### Linux / macOS

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/cipher
```

## Project structure

```text
CIPHER/
├── CMakeLists.txt
├── cipher.conf.example    ← Copy to cipher.conf and edit
├── include/cipher/        ← Public interfaces
│   ├── cipher_core.hpp
│   ├── config.hpp         ← Configuration subsystem (v0.2)
│   └── logger.hpp         ← Logger (v0.2)
├── src/                   ← Implementation
│   ├── main.cpp
│   ├── cipher_core.cpp
│   ├── config.cpp         ← (v0.2)
│   └── logger.cpp         ← (v0.2)
├── tests/                 ← Dependency-free tests
│   ├── test_cipher_core.cpp
│   ├── test_config.cpp    ← (v0.2)
│   └── test_logger.cpp    ← (v0.2)
└── docs/                  ← Vision, architecture, and roadmap
```

## Principles

1. **Local-first:** keep core operation possible without a cloud connection.
2. **Provider independence:** integrations can be optional adapters, never the authority over the architecture.
3. **Replaceable components:** keep clear boundaries around the runtime, storage, tools, and interface.
4. **Portable data:** use documented formats and provide export paths before data becomes difficult to move.
5. **Security by design:** tools receive explicit permissions; high-risk actions require approval.
6. **Test before change:** upgrades must pass tests and preserve a rollback path.
7. **Understand what we build:** use AI-assisted development, but review and learn from every core component.

## Development status

This project is experimental and under active development. Do not use it to control important systems or handle secrets. The current build has no system-control, network, or AI-inference capabilities.
