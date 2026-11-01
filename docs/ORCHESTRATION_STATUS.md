# Orchestration Status (Stages 1-8, non-live)

Status of the `cipher_orchestration` library against [AGENT_ROLE_CONTRACT.md](AGENT_ROLE_CONTRACT.md).
This document records what is implemented and tested, and what is still fake or missing.
It does not change the contract.

## Components

| Component | Implementation | Tested with | Notes |
|---|---|---|---|
| State machine (`state_machine`) | Real | Unit and flow tests | Implements contract sections 5 and 8. Two executions max, one BLOCKED diagnostic, security escalation before Review. |
| Orchestrator (`orchestrator`) | Real | Flow and recovery tests | Persists `execution_in_flight` before each counted implementation run. `recover()` never re-runs an executor. |
| File job store (`job_store_file`) | Real file I/O | Store and recovery tests | Key=value, temp file then rename. A corrupt or partial `.state` throws and is never reset to a fresh record. |
| Scheduler (`scheduler`) | Pure function | Concurrency tests | Capacity counts non-terminal job states, rebuilt from the store each tick. Max 3 jobs. |
| Worktree allocator (`worktree_allocator`) | Real directory and record logic | Concurrency tests | The git worktree backend is a fake interface. There is no real `git worktree add` backend yet. |
| Merge gates (`governed_merge`) | Pure function | Concurrency tests | 7 contract gates plus 3 adapter preconditions. |
| Merge lock (`MergeLock`) | Real file lock (`wx` create) | Concurrency tests | A stale `merge.lock` after a crash blocks all merges. It is never auto-deleted. |
| Event log (`event_log`) | Real append-only file | Flow tests (`test_transitions_are_logged_one_line_each`) | One line per transition. |
| Process runner (`process_runner`) | Real on Windows only | Child-process tests | CreateProcessW, Job Object kill-on-close, output cap. Non-Windows throws. |
| GitHub adapter (`github_adapter`) | Typed argv builder | Adapter tests | Closed operation enum. No `gh api` passthrough. |
| GitHub process runner (`gh_process_runner`) | Maps `gh` argv onto the process runner. The caller passes an absolute path. | Fake process runner | Requires argv[0] to be the literal `gh`. Nothing resolves `gh` from PATH. No live `gh` call has been made by tests. |
| Claude executor (`claude_executor`) | Real argv and parse | Fake process runner | Tool-less. One earlier live probe succeeded. |
| Claude role executor (`claude_role_executor`) | Research and Architecture only | Fake process runner | Security, Implementation, QA and Reviewer escalate without any process call. |

## Fake or missing

- **No tool-enabled implementation executor.** The Claude call is tool-less, so it cannot edit files, push, or produce a head SHA. A live pilot cannot run end to end without one.
- **No live GitHub interaction.** The tests use fakes. Nothing has read or mutated the repository through the adapter.
- **No merge controller.** `evaluate_merge_gates` and `MergeLock` are tested in isolation. Nothing re-reads job B's PR after job A merges. Re-reading state after each merge is the caller's job, and that is untested.
- **No `gh` or `claude` path resolver.** The caller must pass absolute paths.
- **No real git worktree backend.** The allocator is tested with a fake backend.
- **No independent security executor.** Gemini and Antigravity are unavailable. Security-sensitive jobs stay in `NEEDS_HUMAN`.
- **Not wired into the `cipher` executable.** The orchestration library is standalone.

## Contract tensions

- Round 3 of review is unreachable under the 2-execution limit (sections 5.1 and 5.2 item 3).
- Section 5.3 asks for a one-shot higher-tier re-evaluation on the same head, which section 5.2 item 3 forbids. It is treated as a one-shot exception that does not count as a round.
- Section 8.2 defines 7 merge gates. The implementation has those 7 plus 3 adapter preconditions (PR open, head unchanged, mergeable).

## Budget note

Paid Claude calls so far cost about $0.30 in total. The budget flag is not a hard ceiling: a call capped at $0.05 cost $0.138.

## Pilot requirements

A live pilot is blocked until all of these hold:

1. A tool-enabled implementation executor exists and is verified.
2. The real `gh` and worktree backends are exercised against a disposable branch.
3. The owner approves the paid-call budget for the pilot.
4. The pilot path is docs-only and non-security.
