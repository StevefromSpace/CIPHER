# CIPHER: Current Project State

This document is the authoritative summary of CIPHER's verified state. It provides human contributors and AI coding agents with the exact technical baseline, verified capabilities, test results, limitations, and the next recommended tasks.

---

## 1. Verified Code Baseline & Repository State

* **Repository:** [https://github.com/StevefromSpace/CIPHER](https://github.com/StevefromSpace/CIPHER)
* **Active Branch:** `main`
* **Verified Code Baseline:** Commit `677e8455bd51f760288c122aed97c7cea88ec22b` (`677e845`)
  * **Subject:** `fix: centralize application version reporting`
  * **Scope:** Full C++17 core, configuration parser, leveled diagnostic logger, CMake centralized versioning (`version.hpp`), and comprehensive test suites.
* **Documentation & Housekeeping Layer:** Subsequent documentation-only commits maintain project notes, roadmaps, and architecture guides without altering compiled source or application behavior.
* **Working Tree:** Clean (zero uncommitted changes; obsolete temporary files removed).

---

## 2. CI Verification for Code Baseline

* **Continuous Integration Provider:** GitHub Actions
* **Workflow Configuration:** [.github/workflows/ci.yml](../.github/workflows/ci.yml)
* **Triggers:** Push to `main`, Pull Requests targeting `main`.
* **Code Baseline Tested:** Commit `677e845` (`fix: centralize application version reporting`)
* **Workflow Run:** [Run #6 (ID: 38710534839)](https://github.com/StevefromSpace/CIPHER/actions/runs/38710534839)
* **Status:** **Success (Passing)** in 42 seconds
* **Matrix Jobs:**
  1. **Ubuntu 24.04 (`ubuntu-24.04`):** Passed (GCC on Ubuntu 24.04 LTS; CTest passed).
  2. **Windows 2022 (`windows-2022`):** Passed (MSVC on Windows Server 2022; CTest passed).
* **Note on Documentation Commits:** CI verifies the compiled code baseline; subsequent documentation-only commits on `main` do not alter the verified binaries or test suite outputs.

---

## 3. Project Purpose & Long-Term Direction

**CIPHER** stands for **Contextual Intelligence Platform for Home, Enterprise & Research**.

CIPHER is an independent, local-first assistant platform built incrementally in small, portable, and verifiable milestones. Its long-term vision encompasses:
* Natural conversation through text and voice.
* Persistent, user-controlled, exportable memory.
* Context-aware computing across personal, laboratory, and enterprise environments.
* Tool execution protected by explicit permission boundaries and structured action logs.
* Replaceable AI runtimes, progressively advancing toward models and training pipelines under our own control.
* Complete independence from mandatory cloud APIs or single-provider lock-in.

---

## 4. Current Implemented Capabilities (v0.2 Baseline)

The codebase currently contains three modular C++17 libraries and a command-line executable:

1. **CIPHER Core (`cipher_core`):**
   * Interface: `include/cipher/cipher_core.hpp`, Implementation: `src/cipher_core.cpp`
   * Interactive REPL command processing.
   * Built-in commands: `help`, `about`, `status`, `time`, `echo`, `quit`, `exit`.
   * Whitespace handling (leading/trailing whitespace trimming), case-sensitive command matching.
   * Platform-portable timestamp formatting using `localtime_s` (MSVC/Windows) and `localtime_r` (POSIX/Linux).
   * Centralized application version reporting via CMake-generated `cipher/version.hpp` (`0.2.0`).
   * Completely decoupled from configuration and logging.

2. **Configuration Subsystem (`cipher_config`):**
   * Interface: `include/cipher/config.hpp`, Implementation: `src/config.cpp`
   * Stateless parser for key-value configuration (`cipher.conf`).
   * Supported keys: `log_level` (`debug`, `info`, `warn`, `error`) and `log_file` (string path).
   * Strict validation: rejects unknown keys and malformed values with exact line numbers.
   * Safe defaults when `cipher.conf` is absent (`log_level = info`, `log_file = ""` / stderr only).
   * Decoupled from core and logger.

3. **Structured Logger (`cipher_logger`):**
   * Interface: `include/cipher/logger.hpp`, Implementation: `src/logger.cpp`
   * Leveled logging (`DEBUG`, `INFO`, `WARN`, `ERROR`).
   * Timestamped output formatting (`YYYY-MM-DD HH:MM:SS [LEVEL] message`).
   * Writes to `stderr` and optionally appends to a user-configured file.
   * Graceful degradation: if the log file path cannot be opened, a warning is printed to `stderr` and logging continues on `stderr` without crashing.

4. **Executable Application (`cipher`):**
   * Source: `src/main.cpp`
   * Orchestrates configuration loading, logger initialization, and the interactive session.
   * Emits accurate startup and shutdown diagnostics (`Configuration active (log_level=...)`, `CIPHER session ended (user request)`).

---

## 5. Architecture & Project Layout

```text
CIPHER/
├── .github/workflows/
│   └── ci.yml                 # Pinned runners: ubuntu-24.04, windows-2022
├── CMakeLists.txt             # CMake 3.16+, C++17 standard, strict compiler warnings
├── cipher.conf.example        # Tracked example configuration with documentation
├── docs/
│   ├── ARCHITECTURE.md        # Component relationships, boundaries, and design rules
│   ├── DECISIONS.md           # Durable engineering and architectural decisions
│   ├── PROJECT_STATE.md       # Current authoritative baseline and status (this file)
│   ├── ROADMAP.md             # Milestone tracking and forward planning
│   └── VISION.md              # Founding charter, independence rules, and safety principles
├── include/cipher/
│   ├── cipher_core.hpp        # Core command processor interface
│   ├── config.hpp             # Configuration parser data structures and API
│   ├── logger.hpp             # Leveled logger interface
│   └── version.hpp.in         # Template for CMake-generated version header
├── src/
│   ├── cipher_core.cpp        # Command execution implementation
│   ├── config.cpp             # Configuration parsing and validation
│   ├── logger.cpp             # Formatted diagnostic logging
│   └── main.cpp               # Application entry point and REPL loop
├── tests/
│   ├── test_cipher_core.cpp   # Core command test suite
│   ├── test_config.cpp        # Configuration parser test suite
│   └── test_logger.cpp        # Leveled logger test suite
└── README.md                  # Project overview, quickstart, and agent instructions
```

### Dependency Decoupling
```text
main.cpp
  ├── cipher_core
  ├── cipher_config
  └── cipher_logger ──> cipher_config (for LogLevel)

test_cipher_core.cpp  ──> cipher_core only
test_config.cpp       ──> cipher_config only
test_logger.cpp       ──> cipher_logger (and cipher_config)
```
Neither `cipher_config` nor `cipher_logger` depends on `cipher_core`. `cipher_core` does not depend on `cipher_config` or `cipher_logger`.

---

## 6. Build & Test Instructions

### Prerequisites
* C++17 conforming compiler (MSVC 2019+, GCC 9+, or Clang 10+).
* CMake 3.16 or newer.
* No external third-party package managers or dependencies required.

### Build Commands
```bash
# Configure
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Compile
cmake --build build --config Release

# Execute Tests via CTest
ctest --test-dir build -C Release --output-on-failure
```

---

## 7. Current Test Suites & Counts

CTest registers 3 distinct test binaries, covering 51 test functions/cases and 105 individual assertions with a 100% pass rate:

| Test Target | Source File | Functions / Cases | Assertions / Checks | Purpose |
| :--- | :--- | :--- | :--- | :--- |
| `cipher_core_tests` | `tests/test_cipher_core.cpp` | 20 command cases + 1 version check | 21 | Command responses, whitespace trimming, time format, exit flags, version consistency |
| `cipher_config_tests` | `tests/test_config.cpp` | 19 functions | 56 | Parsing, comments, line numbers, error collection, unknown keys, CRLF |
| `cipher_logger_tests` | `tests/test_logger.cpp` | 11 functions | 28 | Levels, labels, timestamping, file append, degradation on bad path |
| **Total** | | **51 cases/functions** | **105 assertions** | **All 3 test targets pass** |

---

## 8. Explicitly Unimplemented Capabilities

To prevent confusion or hallucination by future AI agents, the following capabilities have **NOT** been implemented:
* **No AI Inference Engine:** No language models, no local runtime (e.g., llama.cpp/ONNX), and no external API integration.
* **No Persistent Memory:** No SQLite, vector store, embedding database, or cross-session persistence.
* **No Voice / Audio:** No speech-to-text, text-to-speech, or audio capture.
* **No Operating System Automation:** No arbitrary shell execution, browser control, or GUI manipulation.
* **No Networking:** No web server, REST API, socket listener, or outbound HTTP client.
* **No Autonomous Self-Modification:** No dynamic code rewriting or unattended system reconfiguration.

---

## 9. Known Limitations & Unresolved Decisions

1. **Open-Source Licensing:** An official open-source license (e.g., MIT, Apache-2.0, MPL-2.0, AGPL) has **not yet been chosen**.
2. **Single-Threaded Execution:** The logger and core assume single-threaded execution; no mutex locking is implemented yet.
3. **Log Rotation:** The logger appends continuously to `log_file` without size limits or rotation.
4. **Config File Path:** `cipher.conf` is loaded exclusively from the current working directory.

---

## 10. Next Recommended Tasks

1. **Owner Review (v0.2):** The project owner should review `src/config.cpp`, `src/logger.cpp`, and the test suites to maintain understanding of the v0.2 changes (as listed in [ROADMAP.md](ROADMAP.md)).
2. **Next Development Milestone (v0.3 — Persistent Memory Foundation):**
   * Define the scope of memory (what CIPHER should remember vs. discard).
   * Select a transparent, exportable local storage format with schema versioning.
   * Implement save, load, and migration functions with comprehensive unit tests before integrating into core.
