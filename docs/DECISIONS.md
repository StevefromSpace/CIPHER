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
10. [Persistent Memory Foundation](#10-persistent-memory-foundation)

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
