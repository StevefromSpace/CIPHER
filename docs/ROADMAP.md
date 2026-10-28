# Development Roadmap

The dates are planning targets, not promises. Work at a pace that preserves learning and quality.

## Milestone Status (reconciled 2026-10-25)

Execution order is an explicit project constraint: **v0.2 → v0.3 → v0.4 → v0.5**. A later milestone does not begin until the earlier one is formally complete, including its owner-review gate.

| Milestone | Status | Remaining work |
|-----------|--------|----------------|
| v0.1 foundation | COMPLETE (owner review open) | Owner review of source files (WAITING_FOR_OWNER) |
| v0.1.1 hardening | COMPLETE | None |
| v0.2 configuration and diagnostics | COMPLETE | Owner-cleared. PR #13 merged to `main` (merge `dde6bc1`, from `7a27130`). Unreadable `cipher.conf` is a startup error; missing file uses defaults. |
| v0.2.1 version consistency | COMPLETE | None |
| v0.3 persistent memory foundation | IMPLEMENTED, OWNER REVIEW OPEN | Permission gate implemented on PR #14 (`69e2ee6`, Draft, base `feat/repl-memory-commands`). `remember`/`forget` default to `Monitor`; denied operations leave the memory file byte-identical. Not merged; owner review (WAITING_FOR_OWNER). |
| v0.4 replaceable AI runtime interface | CURRENT (no-model verification) | No-model verification suite on PR #15 (`9047f13`, Draft, base PR #14 branch): core, memory and provider layer with no provider attached. The in-process `llama.cpp` runtime was approved by the owner (2026-10-28) and is delivered under v0.6 (DECISIONS §21). |
| v0.5 permissioned tools | DESIGN SCOPE COMPLETE, OWNER REVIEW OPEN | Design `docs/V05_TOOL_ARCHITECTURE.md` (ADR 18 Accepted). All five documented phases merged (PR #19 registry and built-in tools, PR #20 owner `tool` commands, PR #21 `filesystem.read`, PR #22 owner confirmation for Ask tier). No later phase is documented. PR #12 not merged; owner decides its disposition. |

### Milestone dependencies

- v0.3 permission gate uses the existing `RiskTier`, `ActionDescriptor` and `PermissionPolicy` (`include/cipher/assistant_backend.hpp`, on the PR #2 stack). It does not depend on the v0.5 tool registry.
- PR #12 is not a prerequisite for v0.2 or v0.3. Reassess it when v0.5 begins.

### PR and issue mapping (as of this reconciliation)

- PR #1 (open, not draft, base `main`, head `research/assistant-source-intelligence`): memory load fix and hardening. Not merged.
- PR #2 (draft, base PR #1 branch, head `feat/repl-memory-commands`): v0.3 REPL memory commands; contains the v0.3 and v0.4 feature history.
- PR #3 (draft, base `feat/repl-memory-commands`): Claude and Hermes CLI provider adapters, Windows handle inheritance fix (issue #5). Not tied to a numbered milestone.
- PR #9 (draft): autonomous operating policy. Owner review pending.
- PR #11 (draft): memory inspection regression test seam (issue #4, closed).
- PR #12 (draft, parked, base PR #3 branch): tool registry slice for v0.5. Not part of current milestone execution.
- Issue #4 and #5: closed. Issue #6 (POSIX process-tree termination misses `setsid` descendants): open, design decision WAITING_FOR_OWNER. Issue #7 (verify `claude --safe-mode` suppresses user-level instructions): open, inconclusive; no further billed calls without approval. Issue #8 (Hermes adapter): open, deferred; Hermes is not installed.

### Known limitations and deferred work

- The in-process llama.cpp backend is CPU-only, returns whole answers, uses the same text `TOOL_CALL` protocol as Ollama (no native tool calling) (no streaming); it is opt-in at build time. Provider adapters are not runtime-verified for live use.
- Persistent memory is explicit and human-readable only; no semantic search or automatic extraction.
- Process-tree termination on POSIX does not cover descendants that call `setsid` (Issue #6).
- Open-source license not yet chosen.

### External research track (non-blocking)

Reports in `CIPHER-upstream-research/reports/` cover OpenClaw, Dodaios, Leon, OpenVoiceOS, licensing and security. Ollama and `llama.cpp` have no report or clone yet; that gap does not block v0.2–v0.4. The `experiment/openclaw-bridge` branch has not been inspected. Findings feed v0.4 (runtime abstraction) and v0.5 (permissions, progressive tool loading) when those milestones begin.

## October 2026: v0.1 foundation

- [x] Establish repository structure and founding principles.
- [x] Create a C++17 core and basic command interface.
- [x] Add a repeatable build and starter tests.
- [x] Build locally on the owner's computer (Windows, MSVC via VS 2022 Build Tools, Ninja generator).
      *(Confirmed: build succeeded locally before the initial push.)*
- [x] Create the GitHub repository and push the initial baseline (commit 5501749, branch main).
      *(Confirmed: repository is at https://github.com/StevefromSpace/CIPHER)*
- [ ] Review every source file and understand the execution path.
      *(The owner must do this step; it cannot be marked done by the agent.)*

## October 2026: v0.1.1 foundation hardening

- [x] Add GitHub Actions CI (Ubuntu + Windows, build + CTest).
      See `.github/workflows/ci.yml`.
      Runners pinned to ubuntu-24.04 and windows-2022.
- [x] Fix MSVC localtime deprecation warning (localtime_s / localtime_r platform helper).
- [x] Expand test coverage from 7 to 18 cases (empty input, whitespace, time,
      echo edge cases, case sensitivity, leading-space stripping).

## October 2026: v0.2 — Configuration and diagnostics

- [x] Add a configuration subsystem: optional `cipher.conf` (key = value format).
      Supports `log_level` and `log_file`. Unknown keys and invalid values are
      rejected with line-number error messages.
- [x] Add a structured logger: four levels (DEBUG, INFO, WARN, ERROR), stderr by default,
      optional file output, graceful degradation when the file cannot be opened.
- [x] Integrate config and logger into the startup sequence in `main.cpp`.
      Configuration errors stop startup with a clear message.
      A missing config file uses sensible defaults — no error.
- [x] Provide `cipher.conf.example` with documentation and a security reminder.
      Exclude `cipher.conf` from Git; keep the example tracked.
- [x] Add config tests (test_config.cpp) and logger tests (test_logger.cpp).
- [ ] Review the v0.2 source files and understand the configuration and logging code.
      *(The owner must do this step.)*

## October 2026: v0.2.1 — Version consistency and test alignment

- [x] Centralize application version in `CMakeLists.txt` via CMake project `VERSION`.
- [x] Generate `include/cipher/version.hpp` using `configure_file()` from `version.hpp.in`.
- [x] Ensure `CipherCore::version()`, `help`, `about`, and `status` dynamically report `0.2.0`.
- [x] Reconcile and expand test coverage across the repository:
      * `cipher_core_tests`: 20 command cases + 1 direct version constant check (21 assertion checks).
      * `cipher_config_tests`: 19 test functions (56 assertion checks).
      * `cipher_logger_tests`: 11 test functions (28 assertion checks).
      * Total: 51 test cases/functions and 105 individual assertions across 3 CTest targets (all passing).
- [x] Document working-directory configuration loading assumption in `docs/ARCHITECTURE.md`.

## October 2026: v0.3 — Persistent memory foundation (Foundation Complete)

- [x] Define what should and should not be remembered (explicit owner entries only; secrets, tokens, transcripts, and unapproved observations prohibited).
- [x] Select a transparent, exportable local storage format with strict schema versioning (`cipher-memory-v1` sectioned format) and atomic recoverable writes.
- [x] Implement standalone `cipher_memory` library (`include/cipher/memory.hpp`, `src/memory.cpp`) decoupled from core, config, and logger.
- [x] Add comprehensive memory test suite (`tests/test_memory.cpp`, `cipher_memory_tests` CTest target) verifying 97 assertions (94 on POSIX) across 14 categories:
      * Missing storage file
      * Empty storage
      * Save-and-load round trips
      * Multiple entries and deterministic update behavior
      * Delete behavior and persistence across reloads
      * Unicode and special-character preservation (UTF-8, multiline, backslashes, quotes)
      * Invalid input and duplicate-entry rejection
      * Corrupted storage detection with line-number reporting
      * Unsupported schema version rejection
      * Supported schema migration from Legacy Schema 0 to Schema 1
      * Failed writes preserving last valid data (atomic write)
      * Repeatable save/load operations
      * Windows existing destination atomic replacement & locked destination preservation
      * Hardening regressions: lossless whitespace, category validation, entry count mismatch, stoi overflow safety
- [x] Expand CTest suite to 4 test targets, 65 functions/cases, and 202 assertions (100% passing).
- [x] Implement Codex-free Dual-Claude Developer Pipeline (`tools/pipeline/`) with read-only reviewer isolation and owner confirmation gates (49 unit tests, 100% passing).

- [ ] Review the v0.3 source files and understand the memory subsystem and pipeline tools.
      *(The owner must do this step.)*
- [x] Integrate persistent memory into command REPL (`remember`, `recall`, `forget`, `memories`; opt-in via `memory_file`; `cipher_memory_commands` library, 29 assertions).
- [ ] Integrate persistent memory into the permission gate (not started). Decision: `remember` and `forget` default to `Monitor`; blacklist and override rules stay authoritative; denied operations must leave the memory file unchanged.

## Next milestones


### v0.4: Replaceable AI runtime interface
 
- [x] Implement standard C++17 assistant backend interface (`cipher_assistant`: `IAssistantBackend`, `AssistantManager`) with central validation enforcing blank prompt rejection, timeout bounds, tool lockdown, and local route verification (30 CTest assertions).
- [x] Implement provider-independent assistant adapters (`integrations/`) for OpenClaw, Dodaios, Leon, and OpenVoiceOS (61 unit tests) with loopback binding, cloud rejection, and failure containment.
- [x] Operation Source Intelligence: Conduct source-level architectural investigation of OpenClaw, Dodaios, Leon, and OpenVoiceOS (6 audit reports + ARCHITECTURE_DECISION synthesis).
- [x] Clean-Room Native C++17 Extraction: Port proven mechanisms directly into CIPHER Core:
  * Risk-tiered permission policy (`RiskTier`: `Safe`, `Monitor`, `Ask`, `Block`; `PermissionPolicy` with `Blacklist` > `Whitelist` > `Default Tier`).
  * Context budget prompt byte bounding (`ContextBudget` with `max_prompt_bytes`).
  * Decoupled message wire envelopes (`EventMessage` with `OVOS-MSG-1` schema).
- [x] Defense-in-Depth Security Hardening:
  * Non-finite IEEE 754 float rejection (`std::isnan`, `std::isinf`, `math.isfinite()`).
  * Strict parsed hostname loopback binding (`urlsplit().hostname` in `127.0.0.1`, `localhost`, `::1`).
  * HTTP redirect rejection (`NoRedirectHandler`) to prevent loopback bypass.
- [x] Operation Hardening Gate (Six-Agent Security & Isolation Sprint):
  * C++ Safe String Trimming & Zero Prompt Budget Bounding: Replaced unsafe reverse iterator decrement in `trim_string` with safe `find_first_not_of`/`find_last_not_of`; enforced zero-budget fail-closed bounding; empty action names fail-closed to `RiskTier::Block` (62 CTest assertions, 100% passing).
  * Shared Loopback & Redirect Contract: Validated scheme, stripped credentials, port range check, fail-closed `NoRedirectHandler` (codes 301, 302, 303, 307, 308) via `safe_urlopen()`, and sensitive token scrubbing (12 contract unit tests).
  * OpenClaw Lockdown: Audited CLI, replaced unverified `--deny-tools` with isolated config (`tools: {profile: "minimal", deny: ["*"]}`) and environment binding; bounded streaming subprocess execution.
  * Dodaios Upstream Verification: Classified runtime gateway endpoint as an agent-runtime gateway proxy with unverified live compatibility; bounded success and error body streams.
  * Leon Tool Isolation Boundary: Pinned stable 1.x REST API; disabled live execution fail-closed due to unproven tool-free query path; bounded success and error streams.
  * OpenVoiceOS Safety Boundary: Enforced mock-only assistant submission boundary; explicit session correlation on response frames; best-effort confidentiality notice.
- [x] Operation Hardening Gate 2 (Bounded I/O and Effective Isolation):
  * Bounded I/O Primitive: Created reusable `read_bounded()` stopping at `max_bytes + 1` with immediate stream abort and close.
  * Authentic Tool Lockdown: Verified OpenClaw Commander options, implemented isolated config generation, bound loopback Gateway, and streamed process I/O with kill on limit breach.
  * Downstream Boundaries: Classified Dodaios runtime gateway as agent-runtime model call endpoint; enforced fail-closed tool isolation policy for Leon; enforced mock-only safety boundary for OVOS skills dispatch with explicit correlation.
  * Test Suite Expansion: 5 CTest targets (75 cases, 264 assertions, 100% passing), 115 adapter unit tests (100% passing), 45 pipeline unit tests passing (100% pass rate).
- [x] Operation Hardening Gate 2.1 (Effective Isolation and Release Readiness):
  * OpenClaw Strict Schema & Immediate Subprocess Kill: Replaced unverified `gateway.url` with strict upstream `tools` schema; distinguished `openclaw agent` (gateway-mediated) vs `openclaw agent exec` (embedded); implemented immediate reader-thread overflow notification killing child process immediately without blocking on child exit; handled broken pipe, hanging child, and reader exception edge cases; verified empty tool inventory probe (`_verify_empty_tool_inventory()`); fail-closed `enable_live_execution=False`.
  * Dodaios & Leon Safety Enforcement: Implemented and enforced `enable_live_execution=False` and `verified_tool_isolation=False` in executable code, preventing unverified Boolean opt-in bypass without positive isolation proof.
  * OpenVoiceOS Mock-Only Bus Dispatch Guard: Injected `bus_client_factory` verified for authentic test mock (`is_mock=True` / `_is_cipher_mock=True`), blocking live skill dispatch bypass.
  * CI & Developer Pipeline Reproducibility: Added `requirements-dev.txt` (`mcp>=1.0.0`) in CI workflow, resolving clean CI runner execution across all 49 pipeline tests.
  * Test Suite Expansion: 5 CTest targets (75 cases, 264 assertions, 100% passing), 127 adapter unit tests (100% passing), 49 pipeline unit tests (100% passing).
- [x] Final Isolation Verification (Mock-Only Boundaries & Strict Schema Gate):
  * OpenVoiceOS Mock-Only Type Safety: Replaced marker checks (`is_mock=True` / `_is_cipher_mock=True`) with exact type equality `type(client) is MockBusClient`, blocking spoofed live-like clients from bypassing safety boundary with zero messages sent (25 tests).
  * OpenClaw Tool Inventory Schema Rigor & Version Probing: Enforced upstream version checks (`supported_cli_versions`), strict response schema validation on `openclaw tools list --json` (rejecting missing `tools` property and non-list structures), and unverified live enforcement when compatible CLI is unavailable (48 tests).
  * Route Provenance Operator Assertion: Documented `verified_local` across all adapters as an operator configuration assertion of loopback hostname compliance, reporting `verified_local_status` in health responses.
  * Baseline Rollback Tag Publication: Published `rollback-hardening-gate-2.1-baseline` to remote `origin` at `ccc33273cdf3e36851b2b5b40847b90676362269`.
  * Complete Test Coverage: 5 CTest targets (75 cases, 264 assertions, 100%), 144 adapter unit tests (100%), 49 pipeline unit tests (100%).
- [x] Configure dedicated CI test job for assistant integrations and developer pipeline.
- [x] Direct in-process local runtime adapter: `llama.cpp` backend, delivered in v0.6 (DECISIONS §21).
- [ ] Keep the core useful when no model or assistant is available (verification not yet done; see Milestone Status).

### v0.5: Permissioned tools (DESIGN SCOPE COMPLETE, OWNER REVIEW OPEN)

- Introduce a tool registry, explicit permissions, action logs, and dry-run mode.
- Begin with safe read-only tools before allowing changes.
- Merged: Phases 1–3 (PR #19 registry, `time.now`, `system.info`, `memory.list`; PR #20 owner `tool list`, `tool <id>`, `tool log`). Phase 4 read-only `filesystem.read` under `filesystem_read_root` (PR #21).
- Merged: Phase 5 owner confirmation for the Ask tier (PR #22). Only an exact `yes` runs an Ask-tier tool. No production Ask tool exists yet, so the path is exercised by tests only.
- Later phases: none documented in `docs/V05_TOOL_ARCHITECTURE.md` §8. PR #12 is not merged and is untouched; the owner decides its disposition.
- Note: this milestone started before the v0.3 and v0.4 owner-review gates closed. That was an explicit owner direction (2026-10-27).

### v0.6: Reuse before build, owner-shared memory (MERGED)

- Merged: PR #27 vendors nlohmann/json 3.11.3 (MIT) for the Ollama backend. About 180 lines of custom JSON code were removed. It also audits model-proposed tool calls with `origin=model` in the owner's registry. See DECISIONS §19.
- Merged: PR #28 adds owner-typed `ask --memory <id> <question>`. It is the only path from memory to a model: one entry, permission action `share`, local route only, no tool loop, and the id is logged but not the content. See DECISIONS §20.
- Operation v1 PR 2: the model may propose `filesystem.read <path>` through the existing text `TOOL_CALL` protocol on Ollama and llama.cpp. CIPHER validates the path, shows the resolved file, requires the exact `yes` per call, caps output at 4096 bytes and keeps it on the VerifiedLocal route. No writes, no multi-step execution.
- Next candidates:
  - Merged: `memory_deny` patterns in `cipher.conf` (blacklist for remember/recall/forget/share).
  - Merged: in-process llama.cpp backend (below).
- In-process llama.cpp runtime: `LlamaCppBackend` behind `IAssistantBackend`, opt-in build `-DCIPHER_WITH_LLAMA=ON`, pinned llama.cpp v0.7.0 (SHA-256 checked), `llama_model_path` in `cipher.conf`. Text only, CPU, one model loaded at startup, empty context per request, no fallback between backends. Verified by real inference on a pinned tiny GGUF in the `llama-runtime` CI job (Ubuntu and Windows). See DECISIONS §21.
- Configurable generation settings: `llama_context_tokens`, `llama_max_output_tokens` and `llama_temperature` in `cipher.conf` (defaults 4096 / 512 / 0.8, unchanged when omitted). Validated at parse time; real-inference tests check each value changes llama.cpp's behavior. See DECISIONS §21.
- Known limits: no GPU build, no streaming, no model tool calls through llama.cpp.

### Later: voice, interfaces, planning, learning, and self-improvement

Each capability should be introduced behind tests, narrow permissions, observable behavior, and recovery paths. Model training and self-upgrading are advanced milestones, not prerequisites for a useful first version.
