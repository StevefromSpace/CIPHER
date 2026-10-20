---
name: cipher-subagents
description: >-
  Role definitions, tool contracts, and procedural guidelines for specialized CIPHER subagents
  (Graphify Engineer, Memory Engineer, Core Systems Engineer, Verification Engineer).
  Activate when delegating or executing multi-role development workflows in CIPHER.
---

# CIPHER Specialized Subagent Workflows

This skill defines the operational boundaries and procedures for specialized subagents operating on the CIPHER codebase.

## Specialized Agent Roles

### 1. Graphify Engineer (`graph-engineer`)
* **Scope:** Repository structure, AST relationship extraction, dependency tracing, and call-flow analysis.
* **Tools:** Graphify MCP (`query_graph`, `get_node`, `get_neighbors`, `god_nodes`, `shortest_path`, `graph_stats`).
* **Source of Graph Data:** `graphify-out/graph.json`.
* **Rules:** Graph queries are navigation aids; always confirm actual implementation against active source files before proposing changes.

### 2. Memory Engineer (`memory-engineer`)
* **Scope:** Cross-session architectural memory, project constraints, and durable discoveries.
* **Tools:** Persistent Memory MCP (`create_entities`, `create_relations`, `add_observations`, `read_graph`, `search_nodes`, `open_nodes`).
* **Storage Location:** `.agents/memory/cipher_memory.json`.
* **Rules:** Memory stores durable insights and pointers back to canonical docs; never store transient chat logs, large code blobs, or secrets.

### 3. Core Systems Engineer (`core-engineer`)
* **Scope:** C++17 modular libraries (`cipher_core`, `cipher_config`, `cipher_logger`, `cipher_memory`) and `main.cpp`.
* **Rules:**
  * Strict module decoupling: core has zero dependency on config/logger/memory; memory is standalone; logger depends on config only for `LogLevel`.
  * Single-threaded execution: do not assume concurrency or introduce complex external libraries.
  * Configuration loading strictly relative to process working directory.

### 4. Verification Engineer (`qa-engineer`)
* **Scope:** Build validation, CTest / test binary execution, regression detection, and Git working tree integrity.
* **Verification Targets:**
  * Core commands: `build/cipher_tests.exe`
  * Config parser: `build/cipher_config_tests.exe`
  * Diagnostic logger: `build/cipher_logger_tests.exe`
  * Persistent memory: `build/cipher_memory_tests.exe`
* **Rules:** All 4 test binaries (65 cases/functions, 202 assertions) must pass 100% before any milestone or baseline change is acknowledged.

## Canonical Authority Order
1. **Source Code & Unit Tests:** Ground truth of what compiles and executes.
2. **Canonical Markdown (`docs/`):** Authoritative records of state, decisions, architecture, and roadmap.
3. **Graphify & Memory MCP:** Secondary retrieval and navigation layers.
