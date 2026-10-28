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

---

### 15. Operation Hardening Gate 2: Bounded I/O Primitives, Genuine Tool Lockdown, and Downstream Safety Boundaries

* **Status:** Accepted (Operation Hardening Gate 2, Milestone v0.4)
* **Principle:** Bounded I/O limits must be enforced during stream consumption, not after unbounded buffering into memory. Tool denial mechanisms must reflect authentic upstream contracts rather than unverified CLI flags. When downstream assistants execute host actions without a guaranteed pure text mode, live execution must remain disabled and unavailable fail-closed.
* **Rationale:** A security review of Operation Hardening Gate 1 identified four structural vulnerabilities:
  1. Output size limits in HTTP and subprocess adapters were checked after full unbounded reads (`read()` or `subprocess.run(capture_output=True)`), exposing CIPHER to denial-of-service via memory exhaustion from malicious or runaway local streams.
  2. OpenClaw Commander CLI has no `--deny-tools` argument; mock tests accepted this string without proving actual tool policy enforcement. Authentic tool lockdown is configured via `openclaw.json` (`tools: { profile: "minimal", deny: ["*"] }`), and ambient user configuration (`~/.openclaw`) was inherited without explicit Gateway URL binding.
  3. Dodaios's `/api/runtime-gateway/v1/chat/completions` is an agent-runtime gateway designed for model and tool calls; it cannot be characterized as a verified tool-free endpoint, and live compatibility is unverified without a running local deployment.
  4. Leon's `/api/v1/query` dispatches utterances to skills that execute arbitrary code (`run-action`) on the host; no proven text-only mode exists.
  5. OpenVoiceOS's `destination: ["skills"]` utterance route dispatches to arbitrary skills; keyword denylists cannot provide safety.
* **Current Implementation:**
  - `integrations/contract.py` (Bounded I/O Primitive):
    - Implemented `read_bounded(stream, max_bytes, chunk_size)` which reads in chunks of `min(chunk_size, max_bytes - total + 1)`.
    - If total consumed exceeds `max_bytes`, it immediately aborts, calls `stream.close()`, and raises `ResponseTooLargeError`, preventing memory exhaustion.
    - Verified by 10 dedicated unit tests in `test_contract.py` covering exact limit, 1-byte over limit abort & close, 10MB stream consuming only 1001 bytes, chunked reads, short reads, stream exceptions, and `HTTPError.fp` streams (22 total contract tests).
  - `integrations/openclaw/` (Genuine Isolation & Bounded Subprocess Streaming):
    - Replaced unverified `--deny-tools *` with verified isolated configuration schema: writes `openclaw.json` with `"gateway": {"url": self.config.gateway_url}` and `"tools": {"profile": "minimal", "deny": ["*"]}` inside a dedicated temporary directory.
    - Binds `OPENCLAW_CONFIG_PATH`, `OPENCLAW_STATE_DIR`, `OPENCLAW_GATEWAY_URL`, and `OPENCLAW_PROFILE` in the subprocess environment, preventing ambient global configuration inheritance.
    - Implemented `_execute_bounded_process()` using `subprocess.Popen` and streaming reader threads running `read_bounded()` on `proc.stdout` and `proc.stderr`.
    - If output exceeds `max_output_bytes`, the process is terminated/killed immediately and stream consumption ceases.
    - Verified by 29 unit tests in `test_bridge.py`, including real subprocess tests for bounded streams, process termination, and timeout kill.
  - `integrations/dodaios/` (Contract Alignment & Bounded Transport):
    - Explicitly documents `/api/runtime-gateway/v1/chat/completions` as an agent-runtime gateway proxy, not a verified no-tools endpoint.
    - Classifies live compatibility as unverified (`verified_local=False`, route `UNKNOWN`).
    - Enforces `read_bounded` on success responses (1MB limit) and `HTTPError.fp` error bodies (4KB limit), closing streams promptly.
    - Blocks tool/function definitions, computer use, and mission dispatch fail-closed (21 unit tests).
  - `integrations/leon/` (Tool Isolation Fail-Closed Boundary & Bounded Transport):
    - Documents that `/api/v1/query` dispatches to host skill actions and safe text-only mode cannot be established.
    - Enforces tool isolation policy check: live execution is disabled and unavailable fail-closed (`enable_live_execution=False`).
    - Enforces `read_bounded` on success bodies and `HTTPError.fp` error bodies, closing streams promptly.
    - Verified by 23 unit tests in `test_bridge.py`.
  - `integrations/ovos/` (Safety Boundary & Explicit Correlation):
    - Replaced keyword denylist with structural safety boundary: live utterance dispatch to skills is disabled fail-closed; general assistant prompt submission is strictly mock-only (`is_mock_only=True`).
    - Implemented strict explicit response correlation in `OVOSMessage.is_correlated_response_to()` requiring non-empty, exactly matching `session_id` in context and valid response event types (`speak`, `ovos.utterance.handled`, `complete_intent_failure`). Uncorrelated messages fail closed.
    - Clarified outbound sensitive data detection as a best-effort pattern heuristic before broadcast, not complete secret protection (20 unit tests).
  - Total Test Suite: 115 unit tests passing across all Python integration suites (100% pass rate).
* **Future Intent:** Live assistant integration tests require an isolated sandbox, a verified local route, and explicit owner authorization. All four bridges default to fail-closed behavior when tool safety or route provenance cannot be proven.

---

### 16. Operation Hardening Gate 2.1: Effective Subprocess Isolation, Mock-Only Enforcement, and CI Pipeline Reproducibility

* **Status:** Accepted (Operation Hardening Gate 2.1, Milestone v0.4)
* **Principle:** Security safeguards must be enforced in executing code and proven through runtime invariants rather than superficial configuration flags. A Boolean opt-in must never be accepted as proof of endpoint safety. Subprocess collection must terminate running processes immediately upon buffer limits without waiting for process exit.
* **Rationale:** A targeted follow-up review of Gate 2 identified specific gaps:
  1. Subprocess reader recorded buffer overflow but waited for child process exit or timeout before terminating, delaying termination of hanging or broken-pipe children.
  2. The generated OpenClaw configuration included `gateway.url`, which violated upstream's strict configuration schema. Client-side configs cannot restrict already-running Gateway daemons, and `openclaw agent exec` (embedded) vs `openclaw agent` (gateway-mediated) execution contracts required explicit distinction.
  3. The Gate 2 report claimed `enable_live_execution=False` was enforced in Dodaios, but `DodaiosConfig` lacked this field.
  4. Leon permitted a Boolean opt-in (`enable_live_execution=True`) without establishing downstream tool isolation.
  5. OVOS reported `is_mock_only=True` but permitted any injected `bus_client_factory` to dispatch live utterances.
  6. CI failed because developer pipeline tests require `mcp`, which was not installed in clean Python CI runners.
* **Current Implementation:**
  - `integrations/openclaw/` (Immediate Termination & Strict Schema):
    - Implemented immediate overflow notification in `_execute_bounded_process()` via `threading.Event()` and responsive poll loop, immediately terminating and killing the child process upon limit breach without waiting for process timeout.
    - Verified against stdout overflow, stderr overflow, child ignoring broken pipe, hanging child, and reader thread exceptions.
    - Generated configuration strictly adheres to upstream OpenClaw schema (`tools` object with `profile: "minimal"` and `deny: ["*"]`, removing invalid `gateway.url`).
    - Differentiated `openclaw agent` (gateway-mediated) and `openclaw agent exec` (isolated embedded execution).
    - Enforced `_verify_empty_tool_inventory()` gate and fail-closed `enable_live_execution=False` policy (36 unit tests).
  - `integrations/dodaios/` (Enforced Live Execution Guard):
    - Added `enable_live_execution: bool = False` and `verified_tool_isolation: bool = False` to `DodaiosConfig`.
    - Enforced `is_tool_policy_safe()` gate in `DodaiosBridge.submit_prompt()` failing closed as `AssistantStatus.UNAVAILABLE` unless downstream isolation is proven (23 unit tests).
  - `integrations/leon/` (No Boolean Bypass):
    - Enforced that `enable_live_execution=True` alone does not bypass safety; requires `verified_tool_isolation=True` to prevent arbitrary host skill action execution (`run-action`) (24 unit tests).
  - `integrations/ovos/` (Mock-Only Client Verification):
    - Enforced that injected `bus_client_factory` must produce a verified test mock (`is_mock=True` / `_is_cipher_mock=True`). Real or live-like client factories cannot bypass the mock-only restriction and fail closed without dispatch (22 unit tests).
  - `CI & Developer Dependencies`:
    - Created `requirements-dev.txt` declaring `mcp>=1.0.0` for development and CI runners. CIPHER C++ core remains self-contained with standard library only.
    - Updated `.github/workflows/ci.yml` to install `requirements-dev.txt` in the Python test job.
  - Test Invariants:
    - CTest: 5/5 targets passed (`cipher_core_tests`, `cipher_config_tests`, `cipher_logger_tests`, `cipher_memory_tests`, `cipher_assistant_tests` - 75 test cases, 264 assertions).
    - Python Integrations: 127/127 tests passed.
    - Developer Pipeline: 49/49 tests passed.
* **Future Intent:** All live assistant integration tests require separately authorized sandbox plans and verified local routes.

---

### 17. Final Isolation Verification: Mock-Only Boundary Rigor, Tool Inventory Schema Validation, and Operator Assertion Provenance

* **Status:** Accepted (Final Isolation Verification, Milestone v0.4)
* **Principle:** Security gates must not rely on duck-typed flags that an untrusted or live-like client could set. Response schema validation for security probes must fail closed on missing or unexpected properties rather than defaulting to permitted states. Route provenance attributes must be strictly qualified as operator configuration assertions rather than unverified hardware claims.
* **Rationale:** A final pre-release isolation audit identified remaining defensive gaps:
  1. In OVOS, checking `is_mock=True` or `_is_cipher_mock=True` allowed an injected live-like client to bypass mock-only boundary simply by defining those attributes.
  2. In OpenClaw, `tools_data.get("tools", [])` treated a response dictionary missing the `"tools"` property (such as `{}` or `{"status": "ok"}`) as having an empty inventory, inadvertently permitting live submission. The CLI version was also unverified against supported upstream releases.
  3. `verified_local` was not universally documented across contracts and adapters as an operator configuration assertion of loopback compliance (`127.0.0.1`, `localhost`, `::1`).
  4. The remote repository lacked the published `rollback-hardening-gate-2.1-baseline` rollback tag.
* **Implementation:**
  - `integrations/ovos/` (Mock-Only Strict Type Equality):
    - Replaced duck-typed attribute checks with exact type equality `type(client) is MockBusClient` against CIPHER's offline in-memory test double.
    - Exported canonical `MockBusClient` in `integrations.ovos`. Subclasses and marker-spoofing live clients fail closed as `AssistantStatus.UNAVAILABLE` with zero messages sent (25 unit tests).
  - `integrations/openclaw/` (Strict Schema Validation & Version Check):
    - Added `supported_cli_versions: Tuple[str, ...] = ("2026.", "1.")` and `is_cli_version_supported()` to `OpenClawConfig`.
    - In `_verify_empty_tool_inventory()`, probed CLI `--version` against supported upstream versions before dispatching tools probe.
    - Enforced strict schema validation on `openclaw tools list --json` output:
      - Valid empty list `[]` accepted.
      - Dict with `"tools": []` accepted.
      - Missing `"tools"` key strictly rejected as unexpected schema (never defaulted to `[]`).
      - Non-list `"tools"` property strictly rejected as malformed schema.
      - Non-empty tools list rejected fail-closed.
      - Malformed or invalid JSON rejected fail-closed.
    - In `get_status()`, reported `live_enforcement: "unverified"` whenever compatible CLI is unavailable or live execution / verified local is not configured (48 unit tests).
  - `Operator Assertion Provenance`:
    - Documented `verified_local` across `ProcessingRoute.VERIFIED_LOCAL`, `OpenClawConfig`, `DodaiosConfig`, `LeonConfig`, and `OVOSConfig` as an operator configuration assertion of loopback hostname compliance, not hardware airgap proof.
    - Included `verified_local_status` attribute in `get_status()` across all four bridges.
  - `Remote Tag Verification`:
    - Verified and published `rollback-hardening-gate-2.1-baseline` to remote `origin` at `ccc33273cdf3e36851b2b5b40847b90676362269`.
  - Test Invariants:
    - CTest: 5/5 targets passed (75 test cases, 264 assertions, 100%).
    - Python Integrations: 144/144 tests passed (Contract: 22, OpenClaw: 48, Dodaios: 24, Leon: 25, OVOS: 25).
    - Developer Pipeline: 49/49 tests passed.
* **Future Intent:** Live model invocation and live skill execution remain disabled under CIPHER safety policy until independently verifiable route evidence and sandbox isolation are implemented.

### 18. Permissioned Tool Registry on the Existing PermissionPolicy (v0.5)

- **Status:** Accepted (v0.5 Permissioned Tools, owner approved). Full design in `docs/V05_TOOL_ARCHITECTURE.md`.
- **Decision:** Tools are identified by a stable dotted ID and registered statically as built-ins only. Each invocation is evaluated by the existing `PermissionPolicy` against `"<tool id> <target>"` before the handler runs. Denied calls never execute. Results carry an `executed` flag, so "ran and failed" is distinct from "never allowed".
- **Rejected for v0.5:** provider-supplied or plugin tools, model-initiated tool calls, in-process timeouts, process/network/write side effects (refused at registration), shared broadcast bus invocation, and a second permission system.
- **Consequence:** PR #12 is not merged as-is. Its tier-denial and dry-run logic is reused in a rewrite on `main`.

### 19. Reuse Before Build: Vendored JSON, Owned Control Plane (v0.6)

- **Status:** Accepted.
- **Decision:** Generic plumbing is reused when a mature, permissively licensed, dependency-free option exists. The Ollama backend's hand-written JSON reader and writer (about 180 lines) are replaced by the vendored single header `third_party/nlohmann/json.hpp` (nlohmann/json 3.11.3, MIT, SHA-256 `9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6`). It is used with the non-throwing parse and the replacing UTF-8 error handler, so a bad body or prompt becomes an error result, never an exception.
- **Kept as CIPHER code:** the loopback HTTP client (`src/loopback_http.cpp`). cpp-httplib (MIT) was considered. It does not shrink the code that matters: the 127.0.0.1-only connect, the 4 MiB response bound and the timeout would still need CIPHER wrappers, and it adds a ~10k-line header for a 240-line client. Revisit if CIPHER needs TLS, keep-alive or a server.
- **Not delegated:** agent and tool-calling runtimes (Ollama native tool calling, LangChain-style frameworks, MCP hosts) were not adopted as the tool loop. The tool registry, permission policy, owner confirmation, offered-tool list, audit and routing stay CIPHER code. A runtime may later sit behind `IAssistantBackend` as a text provider only.
- **Also:** model-proposed calls now share the owner's registry and are audited with `origin=model`, closing the v0.5 known gap.

### 20. Owner-Shared Memory for the Local Model (v0.6)

- **Status:** Accepted.
- **Decision:** Memory reaches a model only through the owner-typed `ask --memory <id> <question>`. CIPHER retrieves exactly that one entry, checks the permission policy for action `share` on the id (a `share <id>` blacklist pattern refuses it), and sends it with `local_only` set, so the request fails closed unless the backend route is `VerifiedLocal`. The existing prompt budget (32 KiB) bounds the size. Sharing is allowed by default like `recall`, because the owner typing the id is the authorization; `memory_deny = share <id>` (or `share *`) in `cipher.conf` refuses it. The REPL logs the shared id, never the content.
- **No tools with memory:** a shared-memory ask bypasses the model tool loop, so a model that has seen memory can never propose a tool call in that turn.
- **Unchanged defaults:** plain `ask`, the tool loop and every other route send no memory. There is no automatic retrieval, search or summarisation of memory into prompts, and no memory tool is offered to the model.
- **Reuse assessment:** memory frameworks (Mem0, Letta, LangChain memory) were not adopted. They decide on their own what to store and retrieve, which is the policy CIPHER must own; the remaining work here is a one-entry lookup.

### 21. In-Process llama.cpp Backend, Text Only (v0.6)

- **Status:** Accepted (owner approved the in-process llama.cpp runtime, 2026-10-28).
- **Decision:** `LlamaCppBackend` implements the existing `IAssistantBackend`; no new interface. llama.cpp (release v0.7.0, MIT) does GGUF loading, tokenization, chat templates, sampling and CPU inference. CIPHER keeps validation (`AssistantManager::submit`), `local_only`/`VerifiedLocal`, memory sharing and `memory_deny`, tools, owner confirmation and logging. The model receives only the prompt CIPHER builds and its answer is plain text.
- **Build:** optional `-DCIPHER_WITH_LLAMA=ON` (default OFF, so the default build stays dependency-free). CMake `FetchContent` downloads the release tarball and rejects it unless the SHA-256 matches; `FETCHCONTENT_SOURCE_DIR_LLAMA_CPP` allows offline builds. Static, CPU-only; llama.cpp tools, server, examples, tests, `common`, OpenSSL/HTTP and the ggml RPC backend are off.
- **Config:** `llama_model_path` names an owner-supplied GGUF file. CIPHER never downloads or picks a model. Setting it together with `ollama_port` is a configuration error, and a model that fails to load stops startup: no silent fallback between backends. A build without llama.cpp refuses to start if the key is set.
- **Generation settings:** `llama_context_tokens` (llama.cpp `n_ctx` and `n_batch`), `llama_max_output_tokens` (CIPHER's generation loop stops after this many tokens) and `llama_temperature` (the sampler: 0 = greedy, otherwise min-p 0.05 + temperature + random seed) default to 4096 / 512 / 0.8. Token counts are plain digits in 1..2147483647, the `int32_t` range llama.cpp takes; overflow is rejected, not wrapped. Temperature must be finite and >= 0 (llama.cpp treats <= 0 as greedy and has no upper bound), parsed with the C locale. No upper context limit is imposed: llama.cpp catches a failed allocation and CIPHER reports it and refuses to start. The rule max_output < context is checked when the backend is built.
- **Behavior:** the model loads once; each request clears the KV cache so prompts cannot leak between `ask` calls; `timeout_seconds` is enforced through llama.cpp's abort callback; prompts longer than the context window are refused; `model_override` is refused (one configured model).
- **Text only:** the model tool loop stays Ollama-only. A llama.cpp answer is never parsed for tool requests in v0.6.
- **Rejected:** running `llama-server` as a child process (a second process and an HTTP port to secure, which the Ollama backend already covers); vendoring the llama.cpp tree (~38 MB) into the repository; enabling GPU backends by default (toolchain-specific, not needed for a first milestone).
- **Tests:** deterministic tests (bad path, directory, non-GGUF file, bad settings, fail-closed `ask`) run in every llama build. Real-inference tests load the file in `CIPHER_TEST_GGUF`; the `llama-runtime` CI job sets it to a pinned 1.2 MB GGUF on Ubuntu and Windows. Without the variable they print `SKIP`.
