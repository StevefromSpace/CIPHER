# CIPHER: Durable Architecture & Engineering Decisions

This document records the foundational architectural principles, technical constraints, and engineering decisions that govern CIPHER across sessions, contributors, and AI coding agents.

For full architectural diagrams and long-term philosophical context, see [ARCHITECTURE.md](ARCHITECTURE.md) and [VISION.md](VISION.md).

---

## Decision Index

1. [Provider Independence](#1-provider-independence)
2. [Language Selection (C++, C, Rust)](#2-language-selection)
3. [Control Over AI Runtime and Training](#3-control-over-ai-runtime-and-training)
4. [External AI Coding Tools as Development Aids Only](#4-external-ai-coding-tools-as-development-aids-only)
5. [Hardware and Energy Stewardship](#5-hardware-and-energy-stewardship)
6. [Incremental, Tested Evolution Over Large Rewrites](#6-incremental-tested-evolution-over-large-rewrites)
7. [Automated Verification and Release Governance](#7-automated-verification-and-release-governance)
8. [Strict Isolation and Rollback for Potential Self-Modification](#8-strict-isolation-and-rollback-for-potential-self-modification)
9. [Owner Comprehension and Human Agency](#9-owner-comprehension-and-human-agency)
10. [Persistent Memory Foundation and Win32 Atomic Replacement](#10-persistent-memory-foundation-and-win32-atomic-replacement)
11. [Codex-Free Dual-Claude Development Pipeline with Read-Only Reviewer Isolation](#11-codex-free-dual-claude-development-pipeline-with-read-only-reviewer-isolation)
12. [Replaceable Assistant Capability Providers](#12-replaceable-assistant-capability-providers-assistant-interoperability-architecture-openclaw-dodaios-openvoiceos-leon)
13. [Clean-Room Multi-Agent Source Intelligence Extraction](#13-clean-room-multi-agent-source-intelligence-extraction-risk-tiered-permission-policy-and-defense-in-depth-loopback-hardening)
14. [Operation Hardening Gate: Comprehensive Multi-Agent Security Isolation and Zero-Trust Loopback Boundaries](#14-operation-hardening-gate-comprehensive-multi-agent-security-isolation-and-zero-trust-loopback-boundaries)

---

### 1. Provider Independence

* **Status:** Accepted (Foundational Rule)
* **Principle:** CIPHER must not depend on any single commercial AI provider or cloud service for its essential operation.
* **Rationale:** A core goal of CIPHER is long-term survivability and local user sovereignty. Tying the core to a single proprietary API leaves the platform vulnerable to pricing changes, terms of service shifts, network outages, and vendor deprecation.
* **Current Implementation:** The current baseline has zero cloud or provider dependencies. Startup and command handling require no internet connection.
* **Future Intent:** When cloud or hosted model integrations are introduced, they must be implemented as optional, swappable adapters behind an abstract interface, never as structural dependencies.

---

### 2. Language Selection

* **Status:** Accepted
* **Principle:** C++ (specifically modern C++17) is the primary implementation language. C is reserved for suitable low-level components (e.g., OS bindings, embedded interfaces, memory-constrained routines), and Rust is optional when memory safety or concurrency guarantees strongly justify it.
* **Rationale:** Modern C++ provides fine-grained control over memory, predictable execution performance, broad portability across operating systems and hardware architectures, and a stable standard library without requiring a heavy runtime or garbage collector.
* **Current Implementation:** The core, configuration parser, and logger are implemented in C++17 with strict compiler warnings (`/W4 /permissive-` on MSVC, `-Wall -Wextra -Wpedantic` on GCC/Clang).
* **Future Intent:** Keep the core in C++. If specific future subsystems (such as hardware drivers or specialized sandboxes) benefit from pure C ABI stability or Rust's safety guarantees, evaluate them modularly without rewriting existing working code.

---

### 3. Control Over AI Runtime and Training

* **Status:** Accepted
* **Principle:** Our eventual AI runtime, model weights, and training pipeline should progressively be brought under our direct control.
* **Rationale:** True independence requires not only running models locally, but also understanding and owning the data, fine-tuning scripts, and inference weights that shape the assistant's behavior.
* **Current Implementation:** No AI runtime or models are yet included (Milestone v0.2).
* **Future Intent:** Milestone v0.4 will introduce a small, provider-neutral inference interface. Initial prototypes will prioritize open-weights models running on local runtimes (e.g., llama.cpp or self-contained C++ runtimes) before considering custom training pipelines.

---

### 4. External AI Coding Tools as Development Aids Only

* **Status:** Accepted
* **Principle:** External AI coding tools (such as coding agents, LLMs, and IDE assistants) are development aids, not mandatory runtime components of CIPHER.
* **Rationale:** Using AI assistance during development accelerates prototyping and learning, but CIPHER must compile and run on standard toolchains without needing an AI agent in the loop to function.
* **Current Implementation:** CIPHER builds with standard CMake and any compliant C++17 compiler.
* **Future Intent:** Maintain a completely self-contained build, test, and release process. Developer tooling and AI prompts must remain outside the production runtime.

---

### 5. Hardware and Energy Stewardship

* **Status:** Accepted
* **Principle:** Keep hardware and electricity requirements modest; benchmark on existing hardware before recommending or purchasing specialized equipment.
* **Rationale:** CIPHER is designed for accessible personal computing, home labs, and long-running home servers. Building efficient software is preferred over requiring multi-thousand-dollar GPUs or excessive power consumption.
* **Current Implementation:** The entire test suite executes in less than one second on standard desktop hardware; memory footprint is negligible.
* **Future Intent:** Profile and benchmark quantized local models on consumer CPUs/GPUs before making architectural assumptions about dedicated hardware requirements.

---

### 6. Incremental, Tested Evolution Over Large Rewrites

* **Status:** Accepted
* **Principle:** Prefer small, tested, modular changes over large, generated code rewrites.
* **Rationale:** Large, wholesale AI-generated refactors often introduce hidden regressions, remove edge-case handling, and erode developer understanding. Incremental commits with verified tests make bugs easy to bisect and resolve.
* **Current Implementation:** v0.1 established the core and test harness; v0.1.1 hardened CI and tests; v0.2 added config and logging; v0.2.1 made the bad-path test portable. Each step maintained a passing test suite.
* **Future Intent:** All future milestones (memory, runtime, tools) must be built in small, reviewable increments with corresponding test coverage.

---

### 7. Automated Verification and Release Governance

* **Status:** Accepted
* **Principle:** Automated tests and human-reviewed changes must govern releases and branch integration.
* **Rationale:** Code without automated verification degrades quickly. CI ensures portability across platforms that a single developer may not be running locally.
* **Current Implementation:** GitHub Actions tests every push to `main` and pull request on both Ubuntu 24.04 (GCC) and Windows 2022 (MSVC). Local builds verify tests with CTest prior to pushing.
* **Future Intent:** Expand automated testing to include memory leak sanitizers, address sanitizer (ASan), and integration test suites as new subsystems emerge.

---

### 8. Strict Isolation and Rollback for Potential Self-Modification

* **Status:** Accepted
* **Principle:** Any eventual self-modification capability must strictly operate in an isolated workspace with automated verification, backups, and guaranteed rollback.
* **Rationale:** An assistant that modifies its own code or configuration can render itself inoperable or violate safety boundaries if errors occur.
* **Current Implementation:** Self-modification is completely prohibited and unimplemented.
* **Future Intent:** If self-adaptation is introduced, it must run sandboxed outside the active executable, pass all regression suites, require explicit user approval for deployment, and retain an automated rollback mechanism.

---

### 9. Owner Comprehension and Human Agency

* **Status:** Accepted
* **Principle:** The project owner must understand the architecture and major implementation choices of CIPHER.
* **Rationale:** CIPHER is a personal platform for learning and long-term empowerment. An AI assistant should never introduce complex abstractions that the owner cannot comprehend, maintain, or debug.
* **Current Implementation:** Each milestone includes explicit owner review tasks in [ROADMAP.md](ROADMAP.md) and straightforward, readable C++ without unnecessary template metaprogramming or obscure libraries.
* **Future Intent:** Maintain clear documentation, readable naming conventions, and simple patterns as capabilities expand.

---

### 10. Persistent Memory Foundation and Win32 Atomic Replacement

* **Status:** Accepted (Milestone v0.3 Foundation)
* **Principle:** Runtime memory persistence must be local-first, transparent, human-inspectable, and recoverable without external service dependencies. Initially, it stores only explicit owner-provided memory entries.
* **Rationale:** A transparent, sectioned plain-text key-value format (`cipher-memory-v1`) avoids introducing heavy external dependencies like SQLite, vector databases, or complex third-party JSON frameworks into CIPHER's pure C++17 build. Corruptions or malformed entries are immediately visible and debuggable with line-number diagnostics.
* **Current Implementation:** `cipher_memory` (`include/cipher/memory.hpp`, `src/memory.cpp`) implements an atomic replacement write pattern. On Windows, standard `std::filesystem::rename` fails or lacks atomic replacement semantics when the destination already exists; CIPHER invokes Win32 `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH` to ensure guaranteed atomic overwrites and data flushing to persistent storage. If a write fails or the destination is locked, the original valid file remains intact. Supports full UTF-8 multilingual text, lossless preservation of whitespace, strict schema version validation, and automatic migration from legacy schema version 0 to version 1.
* **Future Intent:** When model inference (v0.4) and permissioned tools (v0.5) arrive, persistent memory will serve as the backing store for user context, preferences, and verified project guidelines without coupling core command processing directly to storage internals.

---

### 11. Codex-Free Dual-Claude Development Pipeline with Read-Only Reviewer Isolation

* **Status:** Accepted
* **Principle:** Developer pipeline automation must be local-first, provider-isolated, and strictly bounded by human-in-the-loop gates. External reviewer agents must be physically restricted to read-only operations without permission to execute arbitrary shell commands or mutate candidate files.
* **Rationale:** Codex was completely removed to preserve usage budgets and eliminate mandatory cloud authentication dependencies. To ensure true peer review integrity, the independent reviewer (Claude Agent B) must not be able to modify the candidate code it reviews. Prompts alone are not security controls; tool-level confinement and process isolation must be enforced.
* **Current Implementation:** `tools/pipeline/` orchestrates Gemini Flash (planner), Claude Agent A (implementer), and Claude Agent B (read-only reviewer). Agent B is executed via Claude Code CLI configured strictly with `--tools Read,Glob,Grep`, `--disallowed-tools Bash,PowerShell,Write,Edit`, and `--permission-mode dontAsk`. Prompts are piped via stdin to prevent Win32 command-line buffer overflows. Local commit confirmation (`tools.pipeline.confirm_commit`) is an explicit, interactive owner gate requiring terminal TTY interaction, strictly decoupled from remote git push.
* **Future Intent:** Maintain strict separation between CIPHER runtime C++ code and developer tooling Python utilities. Pipeline tooling remains a development aid and is never a runtime dependency of CIPHER.

---

### 12. Replaceable Assistant Capability Providers (Assistant Interoperability Architecture: OpenClaw, Dodaios, OpenVoiceOS, Leon)

* **Status:** Accepted (v0.4 Assistant Interoperability Baseline)
* **Principle:** External assistants and LLM runtimes are optional capability providers, never mandatory runtime components of CIPHER. The C++17 core remains authoritative, self-contained, and fully operable when no assistant provider is installed or enabled.
* **Rationale:** Integrating external open-source assistants ([OpenClaw](https://github.com/openclaw/openclaw), [Dodaios](https://github.com/Dodaios/Dodaios), [Leon](https://github.com/leon-ai/leon), [OpenVoiceOS](https://github.com/OpenVoiceOS)) requires establishing a non-invasive boundary that prevents dependency creep into `cipher_core` or `cipher_memory`. A standard C++17 pure virtual interface (`IAssistantBackend`) with central request validation governs all requests. Python adapters implement the respective documented contracts over loopback network boundaries or CLI execution, with strict tool execution lockdown, bounded timeouts, and local provenance verification.
* **Current Implementation:**
  - `cipher_assistant` (C++17 library): In `include/cipher/assistant_backend.hpp` and `src/assistant_backend.cpp`. Implements `IAssistantBackend`, `AssistantManager`, and `validate_assistant_request()` enforcing fail-closed blank prompt rejection, timeout bounds [1s, 300s], tool lockdown, and local route verification. Fully covered by 30 unit tests in `tests/test_assistant_backend.cpp`.
  - `integrations/` (Python adapters):
    - `integrations/openclaw`: One-shot CLI bridge (`openclaw agent --json --profile minimal`) enforcing minimal tool profile, loopback binding, and cloud pattern rejection (19 unit tests).
    - `integrations/dodaios`: OpenAPI Control REST API adapter over loopback, with strict rejection of computer use, mission dispatch, and shell execution (14 unit tests).
    - `integrations/leon`: Fastify HTTP query route (`/api/v1/query`) with version pinning (Leon 1.x stable contract) and sanitized profile API key headers (14 unit tests).
    - `integrations/ovos`: Messagebus JSON event integration with session correlation (`context.session_id`), speech event resolution, and broadcast confidentiality warnings (14 unit tests).
* **Future Intent:** CIPHER Core interacts with external assistants solely through the abstract `IAssistantBackend` interface, allowing hot-swapping between direct local runtimes (e.g. `llama.cpp`), local daemons (e.g. `ollama`), or agent gateways with zero code modification to core commands or memory persistence.

---

### 13. Clean-Room Multi-Agent Source Intelligence Extraction, Risk-Tiered Permission Policy, and Defense-in-Depth Loopback Hardening

* **Status:** Accepted (Operation Source Intelligence, Milestone v0.4)
* **Principle:** Upstream open-source assistant codebases (OpenClaw, Dodaios, Leon, OpenVoiceOS) are architectural reference models, not vendored runtimes or dependencies. Architectural mechanisms—specifically risk-tiered permissions, strict prompt budget bounding, and decoupled event envelopes—must be cleanly reimplemented in modern C++17 without third-party runtime dependencies or copy-paste license contamination.
* **Rationale:** In-depth source code audit of OpenClaw (MIT), Dodaios (Apache 2.0 / historical MIT <= 1.6.0), Leon (MIT), and OpenVoiceOS (Apache 2.0) revealed critical architectural patterns and security nuances:
  1. Dodaios implements a four-tier risk classification (`Safe`, `Monitor`, `Ask`, `Block`) where blacklist overrides whitelist unconditionally.
  2. OpenClaw's context engine manages prompt budgets strictly in UTF-8 bytes to prevent model context exhaustion, but its default `minimal` profile still loads discovery tools (`presence`, `session_status`, `gateway`).
  3. OpenVoiceOS uses a standardized messagebus wire envelope (`OVOS-MSG-1`) that decouples message routing, data payloads, and session tracking, but broadcasts across all connected listeners.
  4. Security audit identified attack vectors: URL hostname spoofing via substring matching (e.g. `http://127.0.0.1.attacker.com`), non-finite IEEE 754 float values (`NaN`, `+Inf`, `-Inf`) evading boundary checks, and HTTP redirect follows (`301`/`302`) that could silently escape loopback confines.
* **Current Implementation:**
  - `cipher_assistant` (C++17 library):
    - `RiskTier` (`Safe`, `Monitor`, `Ask`, `Block`) and `ActionDescriptor` in `include/cipher/assistant_backend.hpp`.
    - `PermissionPolicy`: Implements Dodaios-style priority evaluation (`Blacklist` > `Whitelist` > `Default Tier`).
    - `ContextBudget`: Enforces byte-level UTF-8 bounding (`max_prompt_bytes`) during `validate_assistant_request()`.
    - `EventMessage`: Standardized decoupled message envelope (`OVOS-MSG-1` wire structure).
    - Hardened `validate_assistant_request()` with `std::isnan` and `std::isinf` rejection.
    - Verified by 45 C++ assertions in `tests/test_assistant_backend.cpp` (5/5 CTest targets pass; total C++ suite: 75 test cases, 247 assertions).
  - `integrations/` (Python adapters):
    - `integrations/contract.py`: Hardened with `math.isfinite()` validation and `NoRedirectHandler`.
    - Loopback validation in OpenClaw, Dodaios, Leon, and OVOS configs updated to use `urllib.parse.urlsplit().hostname` strictly restricted to `("127.0.0.1", "localhost", "::1")`.
    - Redirect blocking: HTTP bridges reject redirects to external hosts.
    - Verified by 69 unit tests across all adapter test suites.
* **Future Intent:** When native tool execution (Milestone v0.5) is implemented in CIPHER, the native C++17 `PermissionPolicy` and `RiskTier` evaluation engine will serve as the mandatory, authoritative security gate preceding any tool invocation.

---

### 14. Operation Hardening Gate: Comprehensive Multi-Agent Security Isolation and Zero-Trust Loopback Boundaries

* **Status:** Accepted (Operation Hardening Gate, Milestone v0.4)
* **Principle:** External assistant adapters must operate under zero-trust assumptions. Network bindings must strictly enforce local loopback addresses, reject HTTP redirects, lockdown all external tool invocation capabilities, default route provenance to `UNKNOWN` until cryptographically or in-process verified, pin exact upstream API endpoints, and guard broadcast channels against credential leakage.
* **Rationale:** Integrating third-party assistants introduces multiple attack surfaces if loose assumptions are made:
  1. Loose URL checks can be spoofed (e.g. `127.0.0.1.attacker.com` or embedded user credentials `user:pass@127.0.0.1`).
  2. Automatic HTTP redirection allows external services to redirect requests outside the loopback perimeter (SSRF) and leak headers.
  3. Upstream "minimal" profiles in OpenClaw still ship discovery tools; explicit denial flags (`--deny-tools *`) must be supplied.
  4. Model names (such as `ollama/llama3`) do not prove local execution; external agents can transparently route through cloud providers.
  5. Shared broadcast channels (such as the OpenVoiceOS messagebus) expose sensitive API tokens to all listeners unless explicitly sanitized and filtered.
  6. In C++, undefined behavior in string trimming (iterator decrement underflow) or zero prompt budget edge cases could lead to memory corruption or bypasses.
* **Current Implementation:**
  - `cipher_assistant` (C++17 library):
    - Fixed `trim_string` implementation using `find_first_not_of` and `find_last_not_of` to eliminate reverse iterator decrement undefined behavior on empty/whitespace inputs.
    - Centralized validation strictly rejects non-empty prompts when `max_prompt_bytes == 0` (zero prompt budget enforcement).
    - Hardened `PermissionPolicy` to fail-closed (`RiskTier::Block`) on empty action names.
    - Verified by 62 C++ assertions in `tests/test_assistant_backend.cpp` (all 5 CTest targets pass with 264 total assertions).
  - `integrations/contract.py` (Shared Python Contract):
    - Implemented `validate_loopback_url()`: strictly validates scheme, rejects embedded credentials, parses hostname via `urlsplit().hostname` with exact membership in `{"127.0.0.1", "localhost", "::1"}`, and enforces port range [1, 65535].
    - Implemented `NoRedirectHandler` subclassing `urllib.request.HTTPRedirectHandler` rejecting HTTP status codes 301, 302, 303, 307, 308 fail-closed.
    - Implemented `safe_urlopen()` applying both loopback validation and redirect blocking.
    - Sanitizes sensitive tokens, API keys, passwords, and URL-embedded credentials in `sanitize_sensitive_data()`.
    - Tested by 12 dedicated contract unit tests in `integrations/tests/test_contract.py`.
  - Adapter Lockdown (`integrations/`):
    - **OpenClaw:** Enforces `explicit_tool_denial: bool = True` with CLI parameter `--deny-tools *`; caps process output to 1MB; defaults route to `UNKNOWN` unless `verified_local=True` (26 unit tests).
    - **Dodaios:** Verified against real upstream source `/api/runtime-gateway/v1/chat/completions` and `/api/health`; uses `safe_urlopen`; blocks privileged action names (`computer_use`, `mission`, `shell`, `config`, etc.); defaults route to `UNKNOWN` (20 unit tests).
    - **Leon:** Pinned to Leon 1.x stable REST API (`/api/v1/query`, `/api/v1/info`) and explicitly rejects 2.x preview; uses `safe_urlopen`; redacts `X-API-Key` headers; defaults route to `UNKNOWN` (21 unit tests).
    - **OpenVoiceOS:** Clarified voice event bus role; flags mock-only status via `is_mock_only`; blocks system control commands (`shutdown`, `reboot`, `install`, `shell`); enforces broadcast confidentiality guard rejecting credential-laden prompts before bus publication; defaults route to `UNKNOWN` (20 unit tests).
    - Total: 99 unit tests passing across all integration suites.
* **Future Intent:** These zero-trust boundaries ensure that any future live integration testing or production invocation of local models occurs within a tightly audited, fail-closed perimeter that preserves user privacy and system integrity.
