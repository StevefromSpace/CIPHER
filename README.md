# CIPHER

**CIPHER** stands for **Contextual Intelligence Platform for Home, Enterprise & Research**.

CIPHER is a long-term, local-first assistant platform intended to grow through small, testable milestones. The core architecture is designed to remain under our control and not require any single AI company, cloud service, model, or framework to operate.

> Build it so that no single company, model, or generation of technology can determine its future.

## Current milestone: v0.1.0

This is the first runnable foundation, not a finished AI assistant. It includes:

- A C++17 core with a small command interface.
- `help`, `about`, `status`, `time`, `echo`, `exit`, and `quit` commands.
- A UI-independent core that can later be connected to a terminal, web interface, or voice interface.
- Basic automated tests using only the C++ standard library.
- No mandatory AI provider, network access, database, or third-party runtime.

The AI runtime, persistent memory, voice, computer control, and autonomous planning are intentionally **not implemented yet**.

## Build

Required: a C++17 compiler and CMake 3.16 or newer.

### Windows (PowerShell)

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Run the program (Visual Studio generators commonly place the executable under `Release`):

```powershell
.\build\Release\cipher.exe
```

If your CMake generator places it directly in `build`, run the `cipher.exe` found there instead.

### Linux

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
├── include/cipher/       # Public core interfaces
├── src/                  # Implementation and terminal entry point
├── tests/                # Dependency-free tests
└── docs/                 # Vision, architecture, and roadmap
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

This project is experimental and under active development. Do not use it to control important systems or handle secrets. The current build has no system-control capabilities.
