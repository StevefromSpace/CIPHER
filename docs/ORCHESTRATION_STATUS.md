# Orchestration Status (Stages 1-9 and 10A, non-live)

Status of the `cipher_orchestration` library against [AGENT_ROLE_CONTRACT.md](AGENT_ROLE_CONTRACT.md), which is the governance reference.
This document records what is implemented and tested, what is real versus fake, and what is missing.
It does not change the contract.

## Stage status

| Stage | Scope | Status |
|---|---|---|
| 1-4 | State machine, orchestrator, file job store, GitHub facts, scheduler, worktree allocator | Implemented and tested |
| 5 | Typed GitHub argv adapter, governed merge gates, merge lock | Implemented and tested |
| 6-8 | Claude executor and role executor, process runner, recovery, event log | Implemented and tested |
| 9 | Executable resolver, `gh` process runner | Implemented and tested with fakes |
| 10A | Real git worktree backend, strict JSON GitHub read, Claude executor hardening | Implemented and tested (this PR) |

## Components

| Component | Real or fake | Tested with | Notes |
|---|---|---|---|
| State machine, orchestrator, file job store | Real | Unit, flow, store, recovery tests | Two executions max, one BLOCKED diagnostic, security escalation before Review. `recover()` never re-runs an executor. |
| Scheduler, merge gates, merge lock, event log | Real | Concurrency and flow tests | Max 3 jobs. A stale `merge.lock` blocks all merges and is never auto-deleted. |
| Worktree allocator | Real | Worktree and concurrency tests | Job-id validation, strict root containment, owner records. |
| `GitWorktreeBackend` | **Real** (runs `git`) | Worktree tests against temporary repositories | See below. Deterministic fake backend is kept for other tests. |
| GitHub adapter, TSV read | Real argv builder | Adapter tests with fake runner | Lenient legacy read. Does not populate checks or reviews. |
| GitHub adapter, JSON read (`read_pull_request_detail`) | **Real** parser | Tests plus the PR #49 fixture | See below. |
| `gh` process runner, executable resolver | Real | Fake process runner | `argv[0]` must be an absolute path. Nothing resolves `gh` from PATH implicitly. |
| Process runner | Real, Windows and POSIX | Child-process tests | Timeout, output cap, no shell. |
| Claude executor, role executor | Real argv, parse and mapping | Fake runner only | No paid call is made by any test. |

## Worktree capability

`GitWorktreeBackend` creates detached worktrees under a managed root, one per validated job id, and removes them again.

- Job ids are validated before any path is built. Traversal (`..`, separators, drive prefixes) and root escape are rejected.
- Access to the owner's main checkout is refused. `CIPHER-wt-release` and `tools/pipeline/` are protected and never touched.
- Removal verifies the worktree is managed (owner record present, path under the root, registered with git) before deleting. Unmanaged paths are rejected.
- Stale worktrees are recovered, dirty worktrees are not silently destroyed, and repeated cleanup is safe.
- Covered by `cipher_orchestration_worktree_tests`: creation, invalid ids, traversal, root escape, protected roots, remove, unmanaged rejection, dirty and stale recovery.

## GitHub read capability

`GhAdapter::read_pull_request_detail` runs `gh pr view N --json number,state,headRefOid,baseRefOid,mergeable,statusCheckRollup,reviews` and parses it strictly. It fails closed (throws) on a non-zero exit, empty or malformed JSON, missing or mistyped fields, non-full or uppercase SHAs, unknown enum values, and a PR number that does not match the request. Missing values are never invented.

- Mergeable is true only for `MERGEABLE`. `UNKNOWN` is not mergeable.
- Checks: failure beats pending beats passed. An empty rollup is pending. Unknown check types or conclusions are rejected.
- Reviews: only `OWNER`, `MEMBER` and `COLLABORATOR` reviewers count. The latest decisive review per author wins. Any outstanding change request wins. An approval must name a full commit SHA.
- Review text, PR bodies, comments and commit messages are never read, so untrusted text cannot authorize anything.
- The one authorized live read of PR #49 was saved, sanitized (URLs and timestamps removed), as `tests/fixtures/github_pr49.json`. Automated tests use that fixture only.
- Write operations (comment, merge) still exist as typed argv builders from Stage 5. Nothing calls them from tests.

## Claude execution boundary

The Claude call is tool-less. It can answer Research and Architecture roles but cannot edit files, push, or produce a head SHA.

- Model, budget and prompt are validated before launch. Invalid input throws and the runner is never invoked.
- A runner exception becomes `RUNNER_ERROR`, then `BLOCKED` at the role executor. Timeouts, non-zero exits, malformed results and Claude `is_error` map to failure.
- Exactly one runner invocation per attempt. There is no hidden retry. The two-execution orchestration limit is unchanged.

## Governance boundaries

- Security-sensitive jobs remain `NEEDS_HUMAN`. No independent security-review backend exists, and none is faked.
- The implementation agent cannot approve its own work.
- Gemini is available to the owner through Antigravity, but no verified executable integration exists. Gemini CLI is not installed and no Antigravity CLI is assumed.
- Autonomous merging is not implemented. `evaluate_merge_gates` and `MergeLock` are tested in isolation, and nothing re-reads job B's PR after job A merges.
- A live pilot is intentionally deferred.

## Known limitations

- No tool-enabled implementation executor, so no end-to-end run is possible.
- The orchestration library is not wired into the `cipher` executable.
- `GitWorktreeBackend` is tested against local temporary repositories, not against the real project repository.
- The JSON read has not been run against open PRs with live checks, requested changes or `StatusContext` entries. Those paths are covered by synthetic tests only.
- `gh` and `claude` paths must be supplied by the caller as absolute paths.
- The budget flag is not a hard ceiling: a call capped at $0.05 once cost $0.138.

## Contract tensions

- Round 3 of review is unreachable under the 2-execution limit (sections 5.1 and 5.2 item 3).
- Section 5.3 asks for a one-shot higher-tier re-evaluation on the same head, which section 5.2 item 3 forbids. It is treated as a one-shot exception that does not count as a round.
- Section 8.2 defines 7 merge gates. The implementation has those 7 plus 3 adapter preconditions (PR open, head unchanged, mergeable).

## Next checkpoint

Stage 10B, only after the owner approves it: a tool-enabled implementation executor and a disposable-branch exercise of the real `gh` and worktree backends. Pilot requirements stay as before: owner-approved paid-call budget, docs-only and non-security path.
