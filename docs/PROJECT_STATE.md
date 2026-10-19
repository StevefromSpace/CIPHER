# CIPHER: Current Project State

This document is the authoritative summary of CIPHER's verified state. It provides human contributors and AI coding agents with the exact technical baseline, verified capabilities, test results, limitations, and the next recommended tasks.

---

## 1. Verified Code Baseline & Repository State

* **Repository:** [https://github.com/StevefromSpace/CIPHER](https://github.com/StevefromSpace/CIPHER)
* **Active Branch:** `research/assistant-source-intelligence` (Dedicated research and selective extraction branch branched from `1c6d3e4`)
* **Verified Code Baseline:** Commit `677e8455bd51f760288c122aed97c7cea88ec22b` (`677e845`)
  * **Subject:** `fix: centralize application version reporting`
  * **Scope:** Full C++17 core, configuration parser, leveled diagnostic logger, CMake centralized versioning (`version.hpp`), and comprehensive test suites.
* **v0.3 Persistent Memory & Pipeline Extension:** Incorporates standalone `cipher_memory` persistent storage subsystem and the Dual-Claude developer automation pipeline (`tools/pipeline`).
* **v0.4 Assistant Interoperability, Source Intelligence & Hardening Gates 1, 2 & 2.1:** Incorporates C++17 replaceable assistant interface (`IAssistantBackend`), risk-tiered permission policy (`RiskTier`, `PermissionPolicy`), context budget byte bounding (`ContextBudget`), message event envelopes (`EventMessage`), hardened decoupled adapters (`integrations/` for OpenClaw, Dodaios, Leon, OpenVoiceOS), and completed Operation Hardening Gate 2.1:
  - Reusable bounded I/O primitive (`read_bounded`) halting stream consumption at `max_bytes + 1` and aborting/closing streams immediately.
  - Genuine OpenClaw tool isolation via strict upstream configuration schema (`tools: {profile: "minimal", deny: ["*"]}`), environment variable binding (`OPENCLAW_CONFIG_PATH`, `OPENCLAW_STATE_DIR`, `OPENCLAW_GATEWAY_URL`, `OPENCLAW_PROFILE`), empty tool inventory verification probe, and immediate subprocess overflow kill without blocking on child exit.
  - Alignment of Dodaios contract as an agent-runtime gateway proxy with unverified live compatibility, bounded response/error body streams, and enforced `enable_live_execution=False` + `verified_tool_isolation=False` guards preventing Boolean opt-in bypass.
  - Fail-closed tool isolation boundary for Leon, requiring both `enable_live_execution=True` and positive downstream `verified_tool_isolation=True` before query dispatch.
  - Defensible boundary for OpenVoiceOS enforcing mock-only client factory verification (`is_mock=True` / `_is_cipher_mock=True`), blocking live bus dispatch bypass, and requiring explicit `session_id` response correlation.
  - Developer pipeline reproducibility via `requirements-dev.txt` (`mcp>=1.0.0`) in CI.
* **Working Tree:** Controlled research branch state with verified C++ core enhancements, full CTest suite (5 targets, 75 cases, 264 assertions), 127 adapter tests passing 100%, and 49 developer pipeline tests passing 100%.

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
* **Note on v0.3 Extension:** v0.3 persistent memory and pipeline additions are locally verified (202 CTest assertions, 49 Python pipeline tests passing); CI verification on GitHub will occur upon PR submission.

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

4. **Persistent Memory Subsystem (`cipher_memory` — v0.3 Foundation):**
   * Interface: `include/cipher/memory.hpp`, Implementation: `src/memory.cpp`
   * Standalone, local-first key-value persistent storage engine with schema versioning (`cipher-memory-v1`).
   * Explicit owner-provided memory entries (`id`, `category`, `content`, `created_at`, `updated_at`).
   * Win32 atomic file replacement (`MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`) preventing corruption or data loss on Windows.
   * Full lossless preservation of UTF-8 multilingual characters, leading/trailing content whitespace, multiline text, tabs, quotes, and backslashes.
   * Strict validation and line-numbered error diagnostics on corrupted or invalid files.
   * Automatic backward-compatible migration from Legacy Schema 0 (flat format) to Schema 1.
   * Completely decoupled from core, config, and logger.

5. **Executable Application (`cipher`):**
   * Source: `src/main.cpp`
   * Orchestrates configuration loading, logger initialization, and the interactive session.
   * Emits accurate startup and shutdown diagnostics (`Configuration active (log_level=...)`, `CIPHER session ended (user request)`).

6. **Development Pipeline (`tools/pipeline` — Developer Tooling):**
   * Local development automation connecting Gemini (planner), Claude Agent A (implementer), and Claude Agent B (independent read-only reviewer).
   * Strict security boundaries: read-only tool restrictions, fail-closed schema validation, and complete candidate fingerprinting.
   * Mandatory owner commit confirmation gate (`tools.pipeline.confirm_commit`) strictly separated from remote push.

7. **Assistant Capability Providers & Source Intelligence (`cipher_assistant` & `integrations/`):**
   * C++17 Core Interface & Security Engine: `include/cipher/assistant_backend.hpp`, `src/assistant_backend.cpp`, tested by `tests/test_assistant_backend.cpp` (62 assertions).
   * Standalone architecture for replaceable external assistant capability providers without coupling to `cipher_core` or `cipher_memory`.
   * Enforces fail-closed validation: blank prompt rejection, timeout bounds [1s, 300s], non-finite IEEE 754 float rejection (`NaN`, `+Inf`, `-Inf`), complete tool execution lockdown, zero prompt budget enforcement, safe string trimming (avoiding reverse iterator decrement UB), empty action fail-closed, and local provenance route verification.
   * **Clean-Room Native Components (Extracted via Operation Source Intelligence):**
     - **Risk-Tiered Permission Policy:** `RiskTier` (`Safe`, `Monitor`, `Ask`, `Block`), `ActionDescriptor`, and `PermissionPolicy` implementing strict Dodaios-style priority order (`Blacklist` > `Whitelist` > `Default Tier`). Empty actions fail closed to `Block`.
     - **Context Budget Bounding:** `ContextBudget` enforcing UTF-8 prompt byte bounding (`max_prompt_bytes`) to prevent context window exhaustion, strictly rejecting prompts when budget is zero.
     - **Decoupled Event Envelopes:** `EventMessage` implementing standardized wire format (`OVOS-MSG-1`) with type, payload data, session routing, and source tracking.
   * Four provider adapters in `integrations/` (127 unit tests passing 100%):
     - Shared Contract (`integrations/contract.py`): Loopback URL validation (`validate_loopback_url`) strictly limited to `{"127.0.0.1", "localhost", "::1"}` rejecting user credentials, and redirect denial (`NoRedirectHandler`, `safe_urlopen`) blocking 301, 302, 303, 307, 308 (22 unit tests).
     - OpenClaw (`integrations/openclaw/`): Execution adapter distinguishing `openclaw agent` (gateway-mediated) and `openclaw agent exec` (embedded) with strict upstream configuration schema (`tools` only, rejecting unverified `gateway.url`), environment variable binding (`OPENCLAW_CONFIG_PATH`, `OPENCLAW_STATE_DIR`, `OPENCLAW_GATEWAY_URL`, `OPENCLAW_PROFILE`), empty tool inventory verification probe (`_verify_empty_tool_inventory()`), immediate subprocess overflow detection killing the process immediately without blocking on child exit, broken pipe handling, reader exception handling, and fail-closed `enable_live_execution=False` (36 unit tests).
     - Dodaios (`integrations/dodaios/`): OpenAPI Control REST API adapter over loopback targeting verified `/api/runtime-gateway/v1/chat/completions` and `/api/health`, route defaulting to `UNKNOWN`, redirect rejection, bounded success and error response streams (`read_bounded`), and enforced `enable_live_execution=False` + `verified_tool_isolation=False` guards preventing Boolean opt-in bypass (23 unit tests).
     - Leon (`integrations/leon/`): Fastify HTTP query route (`/api/v1/query` and `/api/v1/info`) with version pinning (Leon 1.x stable contract, rejecting 2.x preview), route defaulting to `UNKNOWN`, sanitized profile API key headers (`X-API-Key` redaction), redirect rejection, bounded streaming (`read_bounded`), and enforced fail-closed tool isolation requiring both `enable_live_execution=True` and positive downstream `verified_tool_isolation=True` (24 unit tests).
     - OpenVoiceOS (`integrations/ovos/`): Messagebus JSON event integration with session correlation (`context.session_id`), voice event bus role, route defaulting to `UNKNOWN`, client factory verification requiring authentic test mocks (`is_mock=True` / `_is_cipher_mock=True`), blocking live bus dispatch bypass, and best-effort secret pattern detection (22 unit tests).
   * Zero runtime dependency on any assistant in `cipher_core` or `cipher_memory`.


---

## 5. Architecture & Project Layout

```text
CIPHER/
├── .agents/                   # Local agent configuration & subagent skills
│   └── skills/cipher-subagents/SKILL.md
├── .github/workflows/
│   └── ci.yml                 # Pinned runners: ubuntu-24.04, windows-2022
├── CMakeLists.txt             # CMake 3.16+, C++17 standard, strict compiler warnings
├── AGENTS.md                  # Project directives and canonical guidance for AI agents
├── GEMINI.md                  # Workspace directives for Gemini in Antigravity
├── cipher.conf.example        # Tracked example configuration with documentation
├── docs/
│   ├── ARCHITECTURE.md        # Component relationships, boundaries, and design rules
│   ├── DECISIONS.md           # Durable engineering and architectural decisions
│   ├── PROJECT_STATE.md       # Current authoritative baseline and status (this file)
│   ├── ROADMAP.md             # Milestone tracking and forward planning
│   └── VISION.md              # Founding charter, independence rules, and safety principles
├── include/cipher/
│   ├── assistant_backend.hpp  # Replaceable assistant backend interface and validation
│   ├── cipher_core.hpp        # Core command processor interface
│   ├── config.hpp             # Configuration parser data structures and API
│   ├── logger.hpp             # Leveled logger interface
│   ├── memory.hpp             # Persistent memory data structures and API (v0.3)
│   └── version.hpp.in         # Template for CMake-generated version header
├── integrations/
│   ├── __init__.py            # Root package exporting all 4 bridges and contracts
│   ├── contract.py            # Shared Python contract and request validation
│   ├── dodaios/               # Dodaios Control REST API adapter (14 tests)
│   ├── leon/                  # Leon Fastify HTTP query adapter (14 tests)
│   ├── openclaw/              # OpenClaw CLI adapter with minimal profile (19 tests)
│   └── ovos/                  # OpenVoiceOS messagebus event adapter (14 tests)
├── src/
│   ├── assistant_backend.cpp  # AssistantManager and central request validation
│   ├── cipher_core.cpp        # Command execution implementation
│   ├── config.cpp             # Configuration parsing and validation
│   ├── logger.cpp             # Formatted diagnostic logging
│   ├── memory.cpp             # Persistent memory storage and atomic write (v0.3)
│   └── main.cpp               # Application entry point and REPL loop
├── tests/
│   ├── test_assistant_backend.cpp # C++ assistant contract and policy tests (30 assertions)
│   ├── test_cipher_core.cpp   # Core command test suite
│   ├── test_config.cpp        # Configuration parser test suite
│   ├── test_logger.cpp        # Leveled logger test suite
│   └── test_memory.cpp        # Persistent memory test suite (v0.3)
├── tools/
│   └── pipeline/              # Dual-Claude developer automation pipeline
│       ├── adapters/          # Claude implementer, reviewer, and mock adapters
│       ├── tests/             # Pipeline unit tests (49 passing tests)
│       ├── config.py          # Pipeline configuration and safety thresholds
│       ├── confirm_commit.py  # Owner-controlled local commit confirmation CLI
│       ├── git_manager.py     # Git worktree isolation and candidate fingerprinting
│       ├── job_store.py       # Atomic job record persistence
│       ├── orchestrator.py    # Pipeline workflow orchestration
│       ├── server.py          # FastMCP server interface
│       └── verifier.py        # Independent candidate build and test execution
└── README.md                  # Project overview, quickstart, and agent instructions
```

### Dependency Decoupling
```text
main.cpp
  ├── cipher_core
  ├── cipher_config
  └── cipher_logger ──> cipher_config (for LogLevel)

test_cipher_core.cpp    ──> cipher_core only
test_config.cpp         ──> cipher_config only
test_logger.cpp         ──> cipher_logger (and cipher_config)
test_memory.cpp         ──> cipher_memory only
```
Neither `cipher_config`, `cipher_logger`, nor `cipher_memory` depends on `cipher_core`. `cipher_core` does not depend on `cipher_config`, `cipher_logger`, or `cipher_memory`.

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

CTest registers 5 distinct test binaries, covering 75 test functions/cases and 264 individual assertions (261 on POSIX due to platform-specific Win32 locking checks) with a 100% pass rate:

| Test Target | Source File | Functions / Cases | Assertions / Checks | Purpose |
| :--- | :--- | :--- | :--- | :--- |
| `cipher_core_tests` | `tests/test_cipher_core.cpp` | 20 command cases + 1 version check | 21 | Command responses, whitespace trimming, time format, exit flags, version consistency |
| `cipher_config_tests` | `tests/test_config.cpp` | 19 functions | 56 | Parsing, comments, line numbers, error collection, unknown keys, CRLF |
| `cipher_logger_tests` | `tests/test_logger.cpp` | 11 functions | 28 | Levels, labels, timestamping, file append, degradation on bad path |
| `cipher_memory_tests` | `tests/test_memory.cpp` | 14 functions | 97 (94 on POSIX) | Missing/empty storage, CRUD round-trips, unicode/multilingual, validation/duplicate handling, corrupt storage line reporting, unsupported schema versions, v0->v1 migration, atomic write failure recovery, repeatable save/load, Windows existing file replacement & locked destination preservation, lossless whitespace, category validation, entry count mismatch, stoi overflow safety |
| `cipher_assistant_tests` | `tests/test_assistant_backend.cpp` | 14 functions | 62 | Replaceable assistant interface, central request validation, blank prompt rejection, timeout bounds, non-finite float rejection, tool execution lockdown, local route verification, zero budget rejection, RiskTier, PermissionPolicy priority, ContextBudget prompt byte bounding, EventMessage wire envelope, mock backend |
| **Total (CTest)** | | **75 cases/functions** | **264 assertions (97 memory, 62 assistant)** | **All 5 test targets pass (100%)** |
| `pipeline_unit_tests` | `tools/pipeline/tests/` | 49 test cases | 49 passing (100% pass rate; reproducible dev dependency via `requirements-dev.txt`) | Verifier, Git manager, Job store, MCP server, Orchestrator, Reviewer adapter |
| `assistant_integrations_tests` | `integrations/` | 127 test cases | 127 passing (100%) | Contract (22), OpenClaw (36), Dodaios (23), Leon (24), OpenVoiceOS (22) |


---

## 8. Explicitly Unimplemented Capabilities

To prevent confusion or hallucination by future AI agents, the following capabilities have **NOT** been implemented:
* **No AI Inference Engine:** No language models, no local runtime (e.g., llama.cpp/ONNX), and no external API integration.
* **No Vector Search or Embeddings:** Persistent memory stores explicit, human-readable entries in local files; no vector database, semantic embeddings, or autonomous background memory extraction.
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

1. **Owner Review (v0.3 Foundation & Pipeline):** The project owner should review `include/cipher/memory.hpp`, `src/memory.cpp`, `tests/test_memory.cpp`, and the Dual-Claude pipeline tooling (`tools/pipeline/`) to maintain understanding of the v0.3 foundation.
2. **Next Development Milestone (v0.3 REPL Integration):**
   * Integrate `cipher_memory` into the CIPHER core interactive REPL (e.g. `remember`, `recall`, `forget` commands).
   * Integrate persistent memory storage path into `cipher_config` (`memory_file` key in `cipher.conf`).
   * Preserve strict decoupling: `cipher_core` coordinates via clean interfaces without memory coupling to logger or config.
