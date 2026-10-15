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
