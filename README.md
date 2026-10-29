# CIPHER

**CIPHER** stands for **Contextual Intelligence Platform for Home, Enterprise & Research**.

CIPHER is a long-term, local-first assistant platform intended to grow through small, testable milestones. The core architecture is designed to remain under our control and not require any single AI company, cloud service, model, or framework to operate.

> Build it so that no single company, model, or generation of technology can determine its future.

## Current milestone: v1.0.0

CIPHER v1.0.0 is a local-first assistant loop: a configured local model can ask for governed tools, CIPHER checks and confirms each call, and the result goes back to the model. Configuration and diagnostics from v0.2 remain. See `docs/V1_ASSISTANT_LOOP.md` for the loop and its limits.

Earlier milestones added configuration and diagnostics (v0.2). Config and logging:

- An optional `cipher.conf` configuration file (key = value format).
- A structured logger with four levels: `debug`, `info`, `warn`, `error`.
- Log output to stderr (always) and an optional log file (configurable).
- Clear error messages when the configuration file contains mistakes.
- Sensible defaults when no configuration file is present.

Persistent memory is available as opt-in REPL commands (set `memory_file` in `cipher.conf`). What is still not implemented: voice, computer control, autonomous planning, general network access. Local inference runs only through a configured loopback Ollama or an in-process llama.cpp model.

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
│   ├── logger.hpp         ← Logger (v0.2)
│   └── version.hpp.in     ← Version template (CMake generated)
├── src/                   ← Implementation
│   ├── main.cpp
│   ├── cipher_core.cpp
│   ├── config.cpp         ← (v0.2)
│   └── logger.cpp         ← (v0.2)
├── tests/                 ← Dependency-free tests
│   ├── test_cipher_core.cpp
│   ├── test_config.cpp    ← (v0.2)
│   └── test_logger.cpp    ← (v0.2)
└── docs/                  ← Architecture, decisions, state, roadmap, and vision
    ├── ARCHITECTURE.md    ← Component boundaries and system design
    ├── DECISIONS.md       ← Durable engineering and architectural decisions
    ├── PROJECT_STATE.md   ← Current authoritative baseline and status
    ├── ROADMAP.md         ← Milestones and progress tracking
    └── VISION.md          ← Founding principles and long-term vision
```

## Principles

1. **Local-first:** keep core operation possible without a cloud connection.
2. **Provider independence:** integrations can be optional adapters, never the authority over the architecture.
3. **Replaceable components:** keep clear boundaries around the runtime, storage, tools, and interface.
4. **Portable data:** use documented formats and provide export paths before data becomes difficult to move.
5. **Security by design:** tools receive explicit permissions; high-risk actions require approval.
6. **Test before change:** upgrades must pass tests and preserve a rollback path.
7. **Understand what we build:** use AI-assisted development, but review and learn from every core component.

## Instructions for Future AI Coding Agents

To maintain continuity across different models, sessions, and tools, every AI coding assistant working on CIPHER must follow these rules:

1. **Read context before editing:** Always read [`docs/PROJECT_STATE.md`](docs/PROJECT_STATE.md), [`docs/DECISIONS.md`](docs/DECISIONS.md), [`docs/VISION.md`](docs/VISION.md), and [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) before modifying code.
2. **Verify live Git state:** Always run `git status`, `git branch --show-current`, and check remote synchronization instead of assuming a previous conversation report is current.
3. **Update state on milestones:** Update [`docs/PROJECT_STATE.md`](docs/PROJECT_STATE.md) whenever a significant, verified milestone or baseline change is achieved.
4. **Record durable architectural choices:** When new long-term technical or design choices are made, document them in [`docs/DECISIONS.md`](docs/DECISIONS.md).
5. **Distinguish local from remote results:** Clearly separate local build/test results from remote GitHub Actions CI outcomes.
6. **Integrity in reporting:** Never fabricate build results, test outcomes, commit hashes, or successful pushes.
7. **Preserve history:** Never overwrite uncommitted user work or rewrite Git history to hide mistakes.
8. **Keep documentation lean:** Do not invent new documentation frameworks or add unnecessary files to the repository.

## Development status

This project is experimental and under active development. Do not use it to control important systems or handle secrets. The core has no system-control or general network access. The only network use is a loopback Ollama endpoint when `ollama_port` is set.
