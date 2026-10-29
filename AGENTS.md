# CIPHER Developer Guidelines

CIPHER (Contextual Intelligence Platform for Home, Enterprise & Research) is an independent, local-first platform built incrementally in small, verifiable milestones.

## Core Directives
1. **Local-First & Provider-Independent:** Core operations must not depend on commercial cloud APIs or mandatory hosted models.
2. **Language Boundaries:** Modern C++17 is the primary language. C is reserved for low-level OS/hardware routines. Rust is optional when memory/concurrency guarantees strongly justify it.
3. **AI Tools as Assistants Only:** External AI agents are development aids, never mandatory runtime components of CIPHER.
4. **Comprehension & Simplicity:** CIPHER must remain understandable and maintainable by the project owner. Avoid unnecessary abstractions.
5. **No Speculative Architecture:** Do not modify system architecture, add dependencies, or invent capabilities without verification.

## Canonical Sources of Truth
Do NOT re-explore the entire codebase or guess project state. Consult these authoritative documents first:
- [docs/PROJECT_STATE.md](docs/PROJECT_STATE.md): Current verified baseline, test suites, verified commit hashes, and capabilities.
- [docs/DECISIONS.md](docs/DECISIONS.md): Foundational engineering and architectural decisions.
- [docs/ROADMAP.md](docs/ROADMAP.md): Milestone progress and forward roadmap (Current: v1.0.0, the v1 assistant loop; merged milestones are listed in the status section).
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): Subsystem layout (`cipher_core`, `cipher_config`, `cipher_logger`, `cipher_memory`) and decoupling rules.
- [docs/VISION.md](docs/VISION.md): Founding principles and independence charter.

## Knowledge & Navigation Infrastructure
- **Graphify MCP (`graphify`):** Use for code relationship queries, symbol lookups, neighbor analysis, and call paths (`graphify-out/graph.json`).
- **Persistent Memory MCP (`cipher-memory`):** Use for durable cross-session context, discoveries, and architectural constraints (`.agents/memory/cipher_memory.json`).
- **Precedence Rule:** Generated graph and memory layers are retrieval aids; source code and the `docs/` Markdown files remain the sole authoritative record.

## Autonomous Operation Policy
- **Durable record:** GitHub issues and PRs hold engineering state. Canonical `docs/` files hold verified project state and decisions. Conversation history and uncommitted notes are not records.
- **Issues:** Search for duplicates first. One issue per coherent outcome, with measurable acceptance criteria. Use existing labels. Do not open issues for trivial edits.
- **Branches and PRs:** Branch from the correct base, which is the stack parent when stacked. Open Draft PRs linked to their issue. Keep each PR's diff to its own change. Before retargeting or rebasing, inspect the ancestry and the remote head. Use `--force-with-lease` with an explicit SHA only.
- **Worktrees:** One writable worktree per implementation stream. Never let two agents edit the same files in one checkout.
- **Definition of done:** Acceptance criteria met, regression tests added, required checks run and reported with actual results, and the diff inspected for unrelated changes and secrets. Where CI applies, the final commit must have a successful CI run for that exact commit SHA before the work is considered verified.
- **Owner approval required:** Merges, pushes to `main`, force-pushes without lease, deleting remote branches, product or architecture decisions, security boundary changes, billed or paid model calls, and public releases. Mark such work `WAITING_FOR_OWNER` and continue independent tasks.
- **Verification evidence:** Distinguish local, CI, and live results. Do not report a check as passing unless a run actually executed. Do not count an edited workflow file as a run.
- **Handoff:** Before stopping, record branch, last verified SHA, open PRs, pending owner decisions, and the next safe action in the linked issue or PR.
